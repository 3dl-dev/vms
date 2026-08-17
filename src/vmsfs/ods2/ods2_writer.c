/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_writer.c - Writer for GENUINE ODS-2 (Files-11 level 2) volumes.
 *
 * Increment 3 of the genuine-ODS-2 effort: produces byte-genuine ODS-2
 * volume images (home block pair, index file bitmap, the ten reserved
 * system files, BITMAP.SYS storage bitmap with real allocation accounting,
 * the MFD, and caller-created files/directories with real FM2 retrieval
 * pointers and directory-record insertion). See the "WRITER" section of
 * vmsfs/ods2.h for full clean-room provenance ([N2] storage-bitmap bit
 * semantics from Nankervis's deallocfile(), [N3] FH2$M_* characteristic
 * bits, [F] reserved-file names read off the real-VAX increment-2 fixture)
 * and the simplifications explicitly labeled [OVMX-inferred] there.
 *
 * The original (in-memory) writer operates purely over a caller-owned image
 * buffer and does no I/O of its own. Increment 11 (vms-6d3b, R2 of the
 * real-ODS-2-runtime epic vms-5eb) ADDS a block-device-backed mode -- see
 * ods2.h's "BLOCK-DEVICE-BACKED WRITER" section for the full design. Every
 * static helper below still reaches the volume through the single wblk()
 * choke point; only wblk() itself, ods2_volume_format() (split into a
 * shared format_common() + a thin in-memory-mode wrapper), the block
 * bump-allocator, and ods2_wvolume_dir_insert()'s header re-read are
 * touched to add the second mode. Both modes coexist; the in-memory path's
 * behavior/output is unchanged.
 */

/* _POSIX_C_SOURCE must precede ALL system headers (glibc locks feature-test
 * macros at the first include). Needed userspace for lseek/off_t in the
 * fd-based block-device constructors below; harmless in the kernel build. */
#ifndef OVMX_ODS2_KERNEL
#define _POSIX_C_SOURCE 200809L
#endif

#include "vmsfs/ods2.h"

#include "ods2_kcompat.h"   /* string/mem/snprintf + ods2_kalloc/kzalloc/kfree */
#include "ods2_block.h"     /* block-access seam (userspace pread / kernel bio) */

#ifndef OVMX_ODS2_KERNEL
#include <unistd.h>         /* lseek / off_t / SEEK_END for format_bdev/open_bdev */
#endif

/* ---- little-endian scalar writes (endian-independent) ---- */

static inline void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* ================================================================
 * Sparse block cache -- backs BLOCK-DEVICE-BACKED mode (increment 11,
 * vms-6d3b). See ods2.h's "BLOCK-DEVICE-BACKED WRITER" section for the
 * full design. Fixed capacity, open-addressed by LBN; a cache miss on an
 * LBN not explicitly seeded (wcache_seed_zero_range()) is fetched via
 * pread -- this writer never depends on ambient/foreign device content, so
 * every miss it can legitimately hit is either a fresh seed or a re-read
 * of its OWN earlier pwrite. OVMX design choice, not an on-disk fact: the
 * cache shape is pure bookkeeping and changes nothing about the bytes
 * committed to the device.
 * ================================================================ */

#define WCACHE_CAP 4096u   /* ~2MB; bounds one top-level call's own working
                            * set (format()'s fixed reserved-layout region,
                            * or one create_file/create_dir/dir_insert's
                            * touched blocks -- see ods2_wvolume_flush(),
                            * called automatically at the end of every
                            * top-level entry point so the cache never
                            * needs to hold more than that). Exceeding it is
                            * reported via wvol->io_error (ODS2_ERR_NOSPACE),
                            * never silently dropped -- the same convention
                            * ODS2_WDIR_MAX_BLOCKS/_EXTENTS already use. */

typedef struct wcache_ent {
    uint32_t lbn;
    uint8_t  used;
    uint8_t  data[ODS2_BLOCK_SIZE];
} wcache_ent_t;

typedef struct wcache {
    unsigned      cap;
    unsigned      n;
    wcache_ent_t *ent;
} wcache_t;

/*
 * On cache-full / I/O-failure wblk() returns a harmless scratch block so a
 * caller's subsequent field writes never dereference NULL; wvol->io_error is
 * set so the failure still surfaces honestly at the top-level call's return.
 * rd vms-dcd moved this scratch OUT of a file-scope static and INTO the wvolume
 * handle (wvol->wcache_scratch): the kernel-resident ACP runs this writer
 * concurrently in many caller contexts, where a shared static would be a
 * cross-process data race. No file-scope mutable state remains in this TU.
 */

#ifndef OVMX_ODS2_KERNEL
/* Only the userspace fd-based block-device constructors (format_bdev/open_bdev,
 * gated below) initialize the write cache; the kernel-resident writer runs
 * in-memory mode only (is_bdev stays 0), so wcache_init is unused there. */
static ods2_status_t wcache_init(ods2_wvolume_t *wvol)
{
    wcache_t *c = (wcache_t *)ods2_kzalloc(sizeof(*c));
    if (!c)
        return ODS2_ERR_NOSPACE;
    c->ent = (wcache_ent_t *)ods2_kzalloc((size_t)WCACHE_CAP * sizeof(wcache_ent_t));
    if (!c->ent) {
        ods2_kfree(c);
        return ODS2_ERR_NOSPACE;
    }
    c->cap = WCACHE_CAP;
    c->n   = 0;
    wvol->wcache_priv = c;
    wvol->io_error    = ODS2_OK;
    return ODS2_OK;
}
#endif /* !OVMX_ODS2_KERNEL -- wcache_init */

static void wcache_free(ods2_wvolume_t *wvol)
{
    wcache_t *c = (wcache_t *)wvol->wcache_priv;
    if (!c)
        return;
    ods2_kfree(c->ent);
    ods2_kfree(c);
    wvol->wcache_priv = NULL;
}

/* Open-addressed find-or-insert. Returns NULL (cache full, LBN not already
 * present) rather than evicting -- callers report ODS2_ERR_NOSPACE. */
static wcache_ent_t *wcache_slot(wcache_t *c, uint32_t lbn, int *created)
{
    unsigned start = (unsigned)((lbn * 2654435761u) % c->cap);
    unsigned i = start;

    for (;;) {
        wcache_ent_t *e = &c->ent[i];
        if (e->used && e->lbn == lbn) {
            *created = 0;
            return e;
        }
        if (!e->used) {
            *created = 1;
            return e;
        }
        i = (i + 1) % c->cap;
        if (i == start)
            return NULL;   /* full, and lbn not present */
    }
}

/*
 * Fetch (or create) the cache entry for `lbn`. A newly created entry is
 * either zero-filled (`zero_fill`, for an LBN this writer is about to
 * overwrite from scratch -- see wcache_seed_zero_range()) or pread from
 * `wvol->bdev_fd` (a re-read of this writer's own earlier pwrite). Never
 * fails outwardly -- a real failure is recorded in `wvol->io_error` and the
 * shared scratch block is returned so the caller's writes land somewhere
 * valid but harmless.
 */
static uint8_t *wcache_block(ods2_wvolume_t *wvol, uint32_t lbn, int zero_fill)
{
    wcache_t *c = (wcache_t *)wvol->wcache_priv;
    wcache_ent_t *e;
    int created;

    if (!c) {
        wvol->io_error = ODS2_ERR_ARGS;
        return wvol->wcache_scratch;
    }
    e = wcache_slot(c, lbn, &created);
    if (!e) {
        wvol->io_error = ODS2_ERR_NOSPACE;
        return wvol->wcache_scratch;
    }
    if (created) {
        e->used = 1;
        e->lbn  = lbn;
        c->n++;
        if (zero_fill) {
            memset(e->data, 0, ODS2_BLOCK_SIZE);
        } else {
            /* A cache miss on an LBN this writer itself wrote earlier: fetch it
             * back through the block-access seam (userspace pread / kernel
             * vmsfs_bio). */
            ods2_status_t rst = ods2_blk_read(ODS2_WVOL_DEV(wvol), wvol->nblocks,
                                              lbn, e->data, ODS2_BLOCK_SIZE);
            if (rst != ODS2_OK)
                wvol->io_error = ODS2_ERR_IO;
        }
    }
    return e->data;
}

/*
 * Seed LBNs [base, base+count) into the cache as all-zero, WITHOUT any
 * pread -- the block-device-backed equivalent of ods2_volume_format()'s
 * whole-image memset(0), scoped to only the LBNs about to be used (the
 * fixed reserved-layout region at format time, or a range just handed out
 * by the bump allocator). No-op in in-memory mode (already zeroed by
 * ods2_volume_format()'s own top-level memset).
 */
static void wcache_seed_zero_range(ods2_wvolume_t *wvol, uint32_t base_lbn,
                                   uint32_t count)
{
    uint32_t i;
    if (!wvol->is_bdev)
        return;
    for (i = 0; i < count; i++)
        (void)wcache_block(wvol, base_lbn + i, /*zero_fill=*/1);
}

ods2_status_t ods2_wvolume_flush(ods2_wvolume_t *wvol)
{
    wcache_t *c;
    unsigned i;

    if (!wvol)
        return ODS2_ERR_ARGS;
    if (!wvol->is_bdev)
        return ODS2_OK;   /* in-memory mode: nothing to flush */
    c = (wcache_t *)wvol->wcache_priv;
    if (!c)
        return ODS2_OK;

    for (i = 0; i < c->cap; i++) {
        wcache_ent_t *e = &c->ent[i];
        ods2_status_t wst;

        if (!e->used)
            continue;
        /* Commit through the block-access seam (userspace pwrite / kernel
         * vmsfs_bio bdirty+sync). */
        wst = ods2_blk_write(ODS2_WVOL_DEV(wvol), wvol->nblocks,
                             e->lbn, e->data, ODS2_BLOCK_SIZE);
        if (wst != ODS2_OK)
            wvol->io_error = ODS2_ERR_IO;
        e->used = 0;
    }
    c->n = 0;

    return wvol->io_error;
}

void ods2_wvolume_close(ods2_wvolume_t *wvol)
{
    if (!wvol || !wvol->is_bdev)
        return;
    (void)ods2_wvolume_flush(wvol);   /* best-effort */
    wcache_free(wvol);
}

/*
 * The single choke point every static helper in this file uses to reach a
 * block's bytes. In-memory mode: unchanged pointer arithmetic. Block-
 * device-backed mode (increment 11): the sparse cache above -- a hit
 * returns the cached copy (so a block written earlier in the same
 * top-level call is seen by a later touch within that call, exactly like
 * the in-memory buffer); a miss is fetched via pread (never zero-filled
 * here -- wcache_seed_zero_range() is the only zero-fill path, called
 * explicitly by format()/alloc_blocks() for LBNs about to be overwritten
 * from scratch).
 */
static uint8_t *wblk(ods2_wvolume_t *wvol, uint32_t lbn)
{
    if (!wvol->is_bdev)
        return wvol->image + (size_t)lbn * ODS2_BLOCK_SIZE;
    return wcache_block(wvol, lbn, /*zero_fill=*/0);
}

/*
 * Tail-call helper for every top-level ods2_wvolume_*() entry point
 * (create_file/create_dir/dir_insert -- format_bdev() does the same thing
 * inline since it also needs to free the cache on failure). In-memory mode:
 * a no-op, ODS2_OK. Block-device-backed mode: surface any sticky
 * pread/pwrite/cache-capacity failure recorded during the call (see
 * wcache_block()'s design note), then commit + clear the cache via
 * ods2_wvolume_flush() so this call's working set never lingers into the
 * next one.
 */
static ods2_status_t wvol_commit(ods2_wvolume_t *wvol, ods2_status_t st)
{
    if (st != ODS2_OK)
        return st;
    if (!wvol->is_bdev)
        return ODS2_OK;
    if (wvol->io_error != ODS2_OK)
        return wvol->io_error;
    return ods2_wvolume_flush(wvol);
}

/* ================================================================
 * Storage bitmap / index-file bitmap bit accounting. [N2] see ods2.h.
 * Both bitmaps use the same 32-bit-word, 4096-bits-per-block packing;
 * they differ only in bit sense and base LBN, so one helper serves both.
 * ================================================================ */

#define BITS_PER_WORD   32u
#define WORDS_PER_BLOCK (ODS2_BLOCK_SIZE / 4u)   /* 128 32-bit words */
#define BITS_PER_BLOCK  (WORDS_PER_BLOCK * BITS_PER_WORD) /* 4096 */

/* Set or clear bit `bitno` within a bitmap region starting at `base_lbn`
 * (block 0 of the region == bits 0..4095, block 1 == bits 4096..8191, ...). */
static void ods2_bitmap_set(ods2_wvolume_t *wvol, uint32_t base_lbn,
                       uint32_t bitno, int value)
{
    uint32_t blk_idx  = bitno / BITS_PER_BLOCK;
    uint32_t in_block = bitno % BITS_PER_BLOCK;
    uint32_t word_idx = in_block / BITS_PER_WORD;
    uint32_t bit_idx  = in_block % BITS_PER_WORD;
    uint8_t *b = wblk(wvol, base_lbn + blk_idx) + (size_t)word_idx * 4;
    uint32_t w = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

    if (value)
        w |= (1u << bit_idx);
    else
        w &= ~(1u << bit_idx);

    put32(b, w);
}

/* Fill an entire bitmap region (blocks..blocks) with all-1 (all free/unused
 * bits), covering `nbits` bits starting at `base_lbn`. */
static void ods2_bitmap_init_all_set(ods2_wvolume_t *wvol, uint32_t base_lbn,
                                uint32_t nblocks)
{
    uint32_t i;
    for (i = 0; i < nblocks; i++)
        memset(wblk(wvol, base_lbn + i), 0xFF, ODS2_BLOCK_SIZE);
}

/* Mark `count` consecutive LBNs (clusterno == LBN, cluster factor 1)
 * ALLOCATED (bit -> 0) in the storage bitmap. */
static void storage_bitmap_mark_used(ods2_wvolume_t *wvol,
                                     uint32_t lbn, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        ods2_bitmap_set(wvol, wvol->bitmap_scb_lbn + 1, lbn + i, 0);
}

/* Mark FID `fidnum` (1-based) in-use (bit -> 1) in the index file bitmap. */
static void ifile_bitmap_mark_used(ods2_wvolume_t *wvol, uint32_t fidnum)
{
    ods2_bitmap_set(wvol, wvol->ibmap_lbn, fidnum - 1, 1);
}

/* ================================================================
 * FM2 retrieval-pointer encoder. Chooses the smallest FM2 format that
 * covers `count` blocks (the inverse of ods2_fh2_map_walk()'s decode in
 * ods2_reader.c [N]):
 *   format 1 (2 words): count <= 256,   lbn < 2^22
 *   format 2 (3 words): count <= 16384, lbn < 2^32
 *   format 3 (4 words): count <= 2^30,  lbn < 2^32
 * A large CONTIGUOUS file (an image like AUTHORIZE.EXE / LIBVMS$SHR.EXE)
 * is thus one pointer, exactly as real VMS records a contiguous file --
 * NOT a fan of 256-block format-1 pointers, which would blow the runtime
 * ACP file-window slot budget (ACP_WINDOW_MAX). vms-5f0: before this the
 * encoder was format-1-only and any file over 128 KB (256 blocks) failed
 * ODS2_ERR_ARGS, so a genuine ODS-2 system disk could not hold real
 * binaries. On success *words_out gets the pointer size in 16-bit words. */
static ods2_status_t encode_map_extent(uint8_t *mp, uint32_t lbn,
                                       uint32_t count, unsigned *words_out)
{
    if (count < 1)
        return ODS2_ERR_ARGS;

    if (count <= 256 && lbn < (1u << 22)) {
        uint32_t high6 = (lbn >> 16) & 0x3F;
        put16(mp + 0, (uint16_t)(0x4000u | ((count - 1) & 0xFF) | (high6 << 8)));
        put16(mp + 2, (uint16_t)(lbn & 0xFFFF));
        if (words_out) *words_out = 2;
        return ODS2_OK;
    }
    if (count <= (1u << 14)) {                       /* format 2: 3 words */
        put16(mp + 0, (uint16_t)(0x8000u | ((count - 1) & 0x3FFF)));
        put16(mp + 2, (uint16_t)(lbn & 0xFFFF));
        put16(mp + 4, (uint16_t)((lbn >> 16) & 0xFFFF));
        if (words_out) *words_out = 3;
        return ODS2_OK;
    }
    if (count <= (1u << 30)) {                       /* format 3: 4 words */
        uint32_t cm1 = count - 1;
        put16(mp + 0, (uint16_t)(0xC000u | ((cm1 >> 16) & 0x3FFF)));
        put16(mp + 2, (uint16_t)(cm1 & 0xFFFF));
        put16(mp + 4, (uint16_t)(lbn & 0xFFFF));
        put16(mp + 6, (uint16_t)((lbn >> 16) & 0xFFFF));
        if (words_out) *words_out = 4;
        return ODS2_OK;
    }
    return ODS2_ERR_ARGS;
}

/* ================================================================
 * FH2 file header construction.
 * ================================================================ */

#define ID_OFF_WORDS 54     /* ident area at byte 108 */
#define MP_OFF_WORDS 114    /* map area at byte 228 (after 120-byte ident) */

/*
 * RECATTR (FAT) presets by file kind, grounded against the real fixture
 * (tests/ods2/real_vax_ods2.dsk, see PROVENANCE increment-3 addendum):
 * system files (INDEXF/BITMAP/BADBLK/CORIMG/CONTIN) show rtype=1/rattrib=0,
 * both directories (000000.DIR/OVMXDIR.DIR) show rtype=2/rattrib=0x08, and
 * both plain data files (HELLO.TXT/WORLD.TXT) show rtype=2/rattrib=0x02
 * (ODS2_RTYPE_VAR / ODS2_RAT_CR -- variable-length records, implied
 * carriage-return carriage control). maxrec: all three kinds use a fixed
 * cap (512) EXCEPT data files, which use 0 (no cap -- matches the real
 * fixture, where neither HELLO.TXT nor WORLD.TXT specifies one either).
 *
 * rsize is NOT a fixed preset for data files: [F16] (increment 9, vms-0f3)
 * re-decoding the real fixture's OWN FID12/FID13 found rsize=105/66, each
 * exactly the longest RECORD actually written to that file, not 0 as this
 * comment previously (and wrongly -- never itself re-checked) claimed. See
 * ods2_wvolume_create_file()'s post-write rsize patch below and the
 * ods2_recattr_t comment in ods2.h.
 */
/*
 * FH2_KIND_DATA_FIX (vms-5eb R6-build): a data file whose bytes are written
 * VERBATIM under RFM=FIXED (fat_rtype==1, [S]) rather than re-framed into
 * RFM=VAR records like FH2_KIND_DATA. Used by ods2_wvolume_create_file_raw()
 * for binary image files (.EXE) the boot master lays down for IMGACT to read
 * raw. efblk/ffbyte are computed exactly as for FH2_KIND_DATA so the reader's
 * ods2_recattr_data_bytes() returns the true byte length.
 */
enum fh2_kind { FH2_KIND_SYSTEM = 0, FH2_KIND_DIR, FH2_KIND_DATA,
                FH2_KIND_DATA_FIX };

/*
 * Write a complete FH2 header for FID `fidnum` at that FID's slot
 * (wvol->hdr_base_lbn + (fidnum - 1)). `name` is the base "NAME.TYPE"
 * (no version); the ident area gets "NAME.TYPE;version" per the real
 * fixture's observed convention. If map_count > 0, writes a single
 * format-1 retrieval pointer covering [map_lbn, map_lbn+map_count) and
 * populates fh2_recattr's hiblk/efblk from map_count (== the file's
 * allocated block count -- see the ods2_recattr_t comment for why this
 * matters: a real VAX MOUNT rejected an earlier version of this writer's
 * output with "Files-11 home block not found" / "index file header is
 * bad" because RECATTR was left all-zero, making every file including
 * INDEXF.SYS itself look zero-length).
 */
static ods2_status_t write_fh2_header_ext(ods2_wvolume_t *wvol, uint32_t fidnum,
                                          uint16_t seq, const char *name,
                                          uint16_t version, uint32_t filechar,
                                          enum fh2_kind kind,
                                          const ods2_extent_t *extents,
                                          unsigned n_extents,
                                          size_t data_len, ods2_fid_t backlink)
{
    uint32_t lbn = wvol->hdr_base_lbn + (fidnum - 1);
    uint8_t *h;
    /* "NAME;VERSION" ident text: fi2_filename (20) + fi2_filenamext (66)
     * == 86 chars max ([N] ods2_ident_t), plus a NUL. Names that fit in
     * 20 chars only ever use the first 21 bytes of this buffer -- see
     * the byte-identical-for-short-names note below. */
    char idbuf[20 + 66 + 1];
    size_t base_len, n;
    ods2_status_t st;
    uint8_t rtype, rattrib;
    uint16_t rsize, maxrec, ffbyte;
    uint32_t hiblk, efblk, total_count;
    unsigned ei;
    static const ods2_uic_t system_owner_uic = { 4, 1 }; /* [F11] SYSTEM [1,4] */

    if (fidnum < 1 || fidnum > wvol->maxfiles)
        return ODS2_ERR_ARGS;
    if (n_extents > 0 && !extents)
        return ODS2_ERR_ARGS;

    total_count = 0;
    for (ei = 0; ei < n_extents; ei++)
        total_count += extents[ei].count;

    base_len = strlen(name);
    if (base_len == 0)
        return ODS2_ERR_ARGS;
    /* "NAME;VERSION" -- must fit in the ident area's 20+66 = 86 chars
     * (fi2_filename + fi2_filenamext, [N] ods2_ident_t). vms-88d: real
     * distro-tree names like "LIBVMSPROCESS$SHR.EXE;1" (23 chars) exceed
     * the old 20-char-only cap; the ident write below splits anything
     * over 20 chars across both fields. */
    n = (size_t)snprintf(idbuf, sizeof(idbuf), "%s;%u", name, (unsigned)version);
    if (n >= sizeof(idbuf))
        return ODS2_ERR_ARGS;

    h = wblk(wvol, lbn);
    memset(h, 0, ODS2_BLOCK_SIZE);

    h[offsetof(ods2_fh2_t, fh2_idoffset)] = ID_OFF_WORDS;
    h[offsetof(ods2_fh2_t, fh2_mpoffset)] = MP_OFF_WORDS;
    /* [F4] fh2_acoffset MUST be the sentinel 255 ("no ACL area"), NOT
     * mpoffset. An earlier draft set acoffset == mpoffset (a common "empty
     * area starts right after the previous one" convention) and a real
     * VAX MOUNT rejected EVERY volume built that way with "%MOUNT-W-
     * IDXHDRBAD, index file header is bad" / "Files-11 home block not
     * found" -- even when every other field (checksum, recattr, backlink,
     * map, ident) was bit-for-bit identical to a real, working INDEXF.SYS
     * header. Bisected empirically on lab-2 by taking the REAL fixture's
     * own FID1 header and changing ONLY its offset fields: acoffset=255
     * (matching the real fixture) mounts; acoffset==mpoffset does not.
     * See ods2.h's WRITER provenance and PROVENANCE-real_vax_ods2.md's
     * increment-3 addendum for the full bisection trail.
     */
    h[offsetof(ods2_fh2_t, fh2_acoffset)] = 255;
    h[offsetof(ods2_fh2_t, fh2_rsoffset)] = 255;

    put16(h + offsetof(ods2_fh2_t, fh2_struclev), ODS2_STRUCLEV_V2);

    /* fh2_fid: num (16 low bits), seq, rvn, nmx (high 8 bits). [F2] see
     * ods2.h: reserved files use seq == their own fid_num; created files
     * use seq == 1 (their first generation). */
    put16(h + offsetof(ods2_fh2_t, fh2_fid) + 0, (uint16_t)(fidnum & 0xFFFF));
    put16(h + offsetof(ods2_fh2_t, fh2_fid) + 2, seq);
    h[offsetof(ods2_fh2_t, fh2_fid) + 4] = 0;         /* rvn */
    h[offsetof(ods2_fh2_t, fh2_fid) + 5] = (uint8_t)(fidnum >> 16); /* nmx */

    /* fh2_backlink: the containing directory's FID. [F2] see ods2.h --
     * a real VAX MOUNT rejects a volume where this is zero/unresolvable. */
    put16(h + offsetof(ods2_fh2_t, fh2_backlink) + 0, backlink.fid_num);
    put16(h + offsetof(ods2_fh2_t, fh2_backlink) + 2, backlink.fid_seq);
    h[offsetof(ods2_fh2_t, fh2_backlink) + 4] = backlink.fid_rvn;
    h[offsetof(ods2_fh2_t, fh2_backlink) + 5] = backlink.fid_nmx;

    put32(h + offsetof(ods2_fh2_t, fh2_filechar), filechar);

    /* ---- RECATTR (FAT) ---- */
    switch (kind) {
    case FH2_KIND_DIR:
        rtype = 2; rattrib = 0x08; rsize = 512; maxrec = 512;
        break;
    case FH2_KIND_DATA:
        rtype = 2; rattrib = 0x02; rsize = 0;   maxrec = 0;
        break;
    case FH2_KIND_DATA_FIX:
        /* RFM=FIXED, 512-byte records, no record attributes -- the shape a
         * real VMS image (.EXE) file carries [S]. Verbatim bytes; the
         * reader bounds valid length off efblk/ffbyte below, not the
         * record framing. */
        rtype = ODS2_RTYPE_FIX; rattrib = 0x00; rsize = 512; maxrec = 512;
        break;
    default: /* FH2_KIND_SYSTEM */
        rtype = 1; rattrib = 0x00; rsize = 512; maxrec = 512;
        break;
    }
    if (total_count == 0) {
        /* Matches every zero-length reserved stub in the real fixture
         * (BADBLK/CORIMG/VOLSET/CONTIN/BACKUP/BADLOG): hiblk=0, efblk=1. */
        hiblk = 0;
        efblk = 1;
        ffbyte = 0;
    } else {
        /* Matches HELLO.TXT/WORLD.TXT exactly (hiblk==efblk==block count);
         * INDEXF.SYS/BITMAP.SYS/SECURITY.SYS showed a +1 (or, for
         * SECURITY.SYS, a smaller) efblk in the real fixture, patched
         * post-hoc below by their own dedicated writers.
         *
         * [F15] (increment 8, vms-0f3): DIRECTORY files are a THIRD case,
         * not the same as plain data files. Directly re-decoding
         * real_vax_ods2.dsk's own FID11 (OVMXDIR.DIR) header this
         * increment (python struct-decode of the FH2 recattr + map area,
         * not a transcription) gives hiblk=1, efblk=2, ffbyte=0 -- i.e.
         * EFBLK EXCEEDS the file's own single allocated block (map extent
         * is exactly 1 block at LBN 31; there is no second block backing
         * "efblk=2"). This is the documented Files-11/RMS "end-of-file
         * lands exactly on a block boundary" convention (OpenVMS Guide to
         * File Applications / RMS Utility Reference: when the last valid
         * byte is the last byte of a block, EFBLK is set to the FOLLOWING
         * block and FFBYTE to 0 -- the position pointer can legitimately
         * exceed the file's own allocation). Cross-checked against FID4
         * (000000.DIR): hiblk=2/efblk=2/ffbyte=0 there is the SAME rule
         * applied to a file whose trailing (2nd) block happens to already
         * be allocated-but-empty -- consistent, not contradictory, once
         * "efblk = (last block containing data) + 1" is the invariant
         * rather than "efblk == hiblk". Every directory THIS writer
         * creates (MFD and caller dirs alike, via ods2_wvolume_create_dir()
         * and the MFD-population path above) is a single CONTIG block
         * (total_count == 1), so hiblk+1 is the right generalization for
         * both: it reproduces FID11 exactly and does not need to model
         * FID4's separately-flagged [OVMX-inferred] "MFD is only 1 block"
         * simplification (a REAL 2-block MFD is out of this writer's
         * current capability regardless of this recattr fix). ffbyte
         * stays 0, matching both real samples. */
        hiblk = total_count;
        if (kind == FH2_KIND_DIR) {
            efblk = total_count + 1;
            ffbyte = 0;
        } else {
            efblk = total_count;
            if ((kind == FH2_KIND_DATA || kind == FH2_KIND_DATA_FIX) &&
                data_len > 0) {
                size_t last_block_bytes = data_len - (size_t)(total_count - 1) * ODS2_BLOCK_SIZE;
                ffbyte = (uint16_t)last_block_bytes; /* 1..512 */
            } else {
                ffbyte = 0;
            }
        }
    }
    h[offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_rtype)]   = rtype;
    h[offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_rattrib)] = rattrib;
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_rsize), rsize);
    /* hi,lo word order -- see the ods2_recattr_t comment. */
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_hiblk) + 0,
          (uint16_t)(hiblk >> 16));
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_hiblk) + 2,
          (uint16_t)(hiblk & 0xFFFF));
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_efblk) + 0,
          (uint16_t)(efblk >> 16));
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_efblk) + 2,
          (uint16_t)(efblk & 0xFFFF));
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_ffbyte), ffbyte);
    put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_maxrec), maxrec);

    /*
     * [F11] fh2_fileowner / fh2_fileprot / fh2_reserved1 / fh2_highwater
     * (increment 6, vms-0f3): found by diffing this writer's OWN output
     * field-by-field against every one of the real fixture's 13 real
     * headers (tests/ods2/real_vax_ods2.dsk, script in
     * PROVENANCE-real_vax_ods2.md's increment-6 addendum) -- ALL FOUR were
     * previously left zero/default, and all four are consistently
     * NON-zero on every single real header (reserved AND caller-created):
     *
     *   - fh2_fileowner: EVERY one of the 13 real headers reads SYSTEM
     *     [1,4] (member=4, group=1) -- no exceptions. This writer has no
     *     notion of "current process UIC" (ods2_wvolume_create_file/dir
     *     take no owner argument), so SYSTEM is applied uniformly, the
     *     same choice already made for hm2_volowner/SECURITY.SYS's owner
     *     field elsewhere in this file.
     *   - fh2_fileprot: the real fixture's 10 reserved files (and both
     *     plain data files, FID 12/13) all show 0xFA00; both real
     *     DIRECTORY files (000000.DIR and OVMXDIR.DIR) show a DIFFERENT
     *     directory-shaped mask (0xBA00 / 0xBA88 respectively -- differ
     *     from EACH OTHER too, single-sample-each, not further resolved).
     *     This writer reproduces the two clearly-repeated constants
     *     (0xFA00 for system/data files, 0xBA00 for directories); the
     *     OVMXDIR.DIR 0xBA88 discrepancy is [OVMX-inferred: open, n=1,
     *     plausibly a process-default-directory-protection value this
     *     writer has no way to know].
     *   - fh2_reserved1 (offset 56, labeled "reserved" in the public
     *     Nankervis struct this writer otherwise follows): NOT actually
     *     always zero -- every one of the 10 TRADITIONALLY RESERVED files
     *     (FID 1-10) reads the identical constant 0x0000FE00 here, while
     *     every CALLER-CREATED file (FID 11-13) reads 0. The correlation
     *     is exact and content-independent (different rtype/rattrib/name/
     *     size on every FID 1-10 sample), i.e. a structural marker keyed
     *     purely on "is this one of the ten reserved files", the same
     *     category of reproducible-but-not-fully-decoded constant as
     *     ods2_security_template[]/[F6] and the FH2$M_* bits/[F]/[N3]
     *     above -- not asserted to be understood, just faithfully
     *     reproduced per the observed correlation.
     *   - fh2_highwater: the VBN of the first NEVER-PHYSICALLY-WRITTEN
     *     block. All 13 real samples fit `highwater == hiblk + 1` -- i.e.
     *     every block this writer's own volume ever allocates for a file
     *     is fully written by the same call that allocates it (no spare
     *     pre-extended block, unlike the real fixture's own MFD, which
     *     has 2 allocated but only 1 written and is the one real sample
     *     that reads highwater == hiblk exactly instead of +1 -- a
     *     detail this writer's single-block MFD never has to reproduce,
     *     since hiblk+1 already gives the right answer whenever hiblk
     *     covers ONLY written blocks, which is true for everything this
     *     writer emits).
     *
     * None of these four were previously tried in isolation -- increment
     * 4's bisection only tried the HOME block's hm2_volowner (see [F9]),
     * never these per-FILE-header fields. Discovered/applied together in
     * increment 6 rather than one at a time, to conserve the lab-2 MOUNT
     * trial budget on an already-heavily-bisected defect.
     */
    put16(h + offsetof(ods2_fh2_t, fh2_reserved1),
          (fidnum <= ODS2_RESFILES) ? 0xFE00u : 0u);
    put16(h + offsetof(ods2_fh2_t, fh2_fileowner) + 0, system_owner_uic.uic_member);
    put16(h + offsetof(ods2_fh2_t, fh2_fileowner) + 2, system_owner_uic.uic_group);
    put16(h + offsetof(ods2_fh2_t, fh2_fileprot),
          (kind == FH2_KIND_DIR) ? 0xBA00u : 0xFA00u);
    put32(h + offsetof(ods2_fh2_t, fh2_highwater), hiblk + 1);

    /* ident area: fi2_filename, space-padded, "NAME.TYPE;VERSION".
     *
     * vms-88d: names <=20 chars (n <= 20) fit entirely in fi2_filename
     * and this is BYTE-IDENTICAL to the pre-fix encoding -- fi2_filenamext
     * is never touched, so it stays zero via the block-wide memset()
     * above, exactly as before.
     *
     * Names >20 chars ([N] ods2_ident_t: fi2_filename[20] at offset 0,
     * fi2_filenamext[66] at offset 54 is the documented "name.type;ver
     * continuation") split: the first 20 chars go in fi2_filename (which
     * is then completely full, needing no space padding of its own), and
     * chars 21..n go in fi2_filenamext, space-padded to fill the
     * remaining 66 bytes -- the same space-pad convention the Files-11
     * On-Disk Structure Specification documents for fi2_filename itself,
     * applied to its continuation field.
     */
    {
        uint8_t *id = h + (size_t)ID_OFF_WORDS * 2;
        size_t first_len = (n < 20) ? n : 20;
        size_t ext_len = (n > 20) ? (n - 20) : 0;

        memset(id, ' ', 20);
        memcpy(id, idbuf, first_len);
        put16(id + 20, version); /* fi2_revision */
        /* fi2_credate .. fi2_bakdate left zero:
         * [OVMX-inferred] timestamps not modeled, see ods2.h. */
        if (ext_len > 0) {
            uint8_t *ext = id + offsetof(ods2_ident_t, fi2_filenamext);

            memset(ext, ' ', sizeof(((ods2_ident_t *)0)->fi2_filenamext));
            memcpy(ext, idbuf + 20, ext_len);
        }
    }

    /* map area: one FM2 retrieval pointer per extent, each sized by
     * encode_map_extent (format 1/2/3 by block count). A large contiguous
     * file is a single format-2/3 pointer. The map area runs from
     * MP_OFF_WORDS*2 to the checksum at byte 510, so bound-check each
     * pointer against that ceiling (vms-5f0). */
    {
        const size_t map_cap = ODS2_BLOCK_SIZE - (size_t)MP_OFF_WORDS * 2 - 2;
        size_t mp_bytes = 0;
        for (ei = 0; ei < n_extents; ei++) {
            unsigned words = 0;
            uint8_t *mp = h + (size_t)MP_OFF_WORDS * 2 + mp_bytes;
            if (mp_bytes + 8 > map_cap)          /* worst-case pointer = 4 words */
                return ODS2_ERR_ARGS;
            st = encode_map_extent(mp, extents[ei].lbn, extents[ei].count, &words);
            if (st != ODS2_OK)
                return st;
            mp_bytes += (size_t)words * 2;
        }
        h[offsetof(ods2_fh2_t, fh2_map_inuse)] = (uint8_t)(mp_bytes / 2);
    }

    /* checksum over first 255 words -> offset 510 */
    put16(h + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(h));

    return ODS2_OK;
}

/* Single-extent convenience wrapper -- every reserved/created file except
 * INDEXF.SYS itself has exactly one (or zero) retrieval-pointer extents. */
static ods2_status_t write_fh2_header(ods2_wvolume_t *wvol, uint32_t fidnum,
                                      uint16_t seq, const char *name,
                                      uint16_t version, uint32_t filechar,
                                      enum fh2_kind kind,
                                      uint32_t map_lbn, uint32_t map_count,
                                      size_t data_len, ods2_fid_t backlink)
{
    ods2_extent_t ext;

    if (map_count == 0)
        return write_fh2_header_ext(wvol, fidnum, seq, name, version, filechar,
                                    kind, NULL, 0, data_len, backlink);

    ext.lbn = map_lbn;
    ext.count = map_count;
    return write_fh2_header_ext(wvol, fidnum, seq, name, version, filechar,
                                kind, &ext, 1, data_len, backlink);
}

/* ================================================================
 * SECURITY.SYS VBN1 data block (increment 4). See ods2.h's WRITER [F6]
 * provenance comment for the full clean-room derivation.
 * ================================================================ */

#define SECURITY_LABEL_OFF   0x52u
#define SECURITY_ENDPTR_OFF  0x08u
#define SECURITY_UIC_OFF     0x1Cu
#define SECURITY_LENFLD_OFF  0x50u
#define SECURITY_TEMPLATE_LEN 76u   /* bytes 0x04..0x4F */

/*
 * The constant "zero ACL entries" template, bytes 0x04..0x4F of the block
 * (76 bytes). Observed byte-for-byte IDENTICAL across 12 real lab-2
 * INITIALIZE+MOUNT trials spanning 9 distinct volume labels and 2 device
 * geometries -- see [F6]. Bytes at template-relative offset 4 (absolute
 * 0x08, the end-of-label pointer) and 24..27 (absolute 0x1C..0x1F, the
 * owner UIC) are placeholders here; ods2_security_build() overwrites both
 * per-volume, so their values in this array are never actually used.
 */
static const uint8_t ods2_security_template[SECURITY_TEMPLATE_LEN] = {
    0x00, 0x00, 0x00, 0x00, 0x5a, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x08, 0x00,
    0x04, 0x00, 0x01, 0x00, 0x02, 0x00, 0x06, 0x00,
    0x02, 0x08, 0x08, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00,
    0x14, 0x00, 0xe0, 0xff, 0xff, 0xff, 0xe0, 0xff,
    0xff, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xf0, 0xff,
    0xff, 0xff, 0x17, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x14, 0x00,
};

/*
 * The checksum algorithm derived under [F6]: byte-lane XOR fold over
 * [4, 4+n) where n = ((78 + strlen) / 4) * 4 (round down to a multiple of
 * 4). Reproduces all 12 real-fixture samples exactly (see ods2.h).
 */
static uint32_t ods2_security_checksum(const uint8_t *block, size_t strlen_)
{
    size_t total = 78u + strlen_;
    size_t n = (total / 4u) * 4u;
    uint8_t lanes[4] = { 0, 0, 0, 0 };
    size_t i;

    for (i = 0; i < n; i++)
        lanes[i % 4] ^= block[4 + i];

    return (uint32_t)lanes[0] | ((uint32_t)lanes[1] << 8) |
           ((uint32_t)lanes[2] << 16) | ((uint32_t)lanes[3] << 24);
}

ods2_status_t ods2_security_build(uint8_t block[ODS2_BLOCK_SIZE],
                                  const char *volname, ods2_uic_t owner_uic)
{
    size_t vlen;
    uint32_t chk;

    if (!block || !volname)
        return ODS2_ERR_ARGS;
    vlen = strlen(volname);
    if (vlen < 1 || vlen > 12)
        return ODS2_ERR_ARGS;

    memset(block, 0, ODS2_BLOCK_SIZE);
    memcpy(block + 4, ods2_security_template, SECURITY_TEMPLATE_LEN);

    block[SECURITY_ENDPTR_OFF] = (uint8_t)(SECURITY_LABEL_OFF + vlen);
    put16(block + SECURITY_UIC_OFF + 0, owner_uic.uic_member);
    put16(block + SECURITY_UIC_OFF + 2, owner_uic.uic_group);
    put16(block + SECURITY_LENFLD_OFF, (uint16_t)(vlen + 4));
    memcpy(block + SECURITY_LABEL_OFF, volname, vlen);

    chk = ods2_security_checksum(block, vlen);
    put32(block + 0, chk);

    return ODS2_OK;
}

/* ================================================================
 * Home block construction. [S]/[N] see ods2.h + PROVENANCE addendum.
 * ================================================================ */

static void write_home_block(ods2_wvolume_t *wvol, uint32_t self_lbn,
                             uint32_t alt_lbn, uint32_t altidx_lbn,
                             const ods2_format_params_t *params)
{
    uint8_t *hb = wblk(wvol, self_lbn);
    char volname[13];
    size_t vn_len;

    memset(hb, 0, ODS2_BLOCK_SIZE);

    put32(hb + offsetof(ods2_home_t, hm2_homelbn),   self_lbn);
    put32(hb + offsetof(ods2_home_t, hm2_alhomelbn), alt_lbn);
    put32(hb + offsetof(ods2_home_t, hm2_altidxlbn), altidx_lbn);
    put16(hb + offsetof(ods2_home_t, hm2_struclev),  ODS2_STRUCLEV_V2);
    put16(hb + offsetof(ods2_home_t, hm2_cluster),   1);

    /* [F12] INDEXF.SYS's own map is now genuinely 3 extents (see
     * write_indexf_header()), matching the real fixture's own VBN
     * numbering exactly: extent 1 is the boot block + home pair (LBN 0-2,
     * VBN 1-3), extent 2 is the alternate index header alone (VBN 4 --
     * even though it sits physically AFTER the header area, at altidx_lbn;
     * the real fixture's own hm2_altidxvbn==4 while hm2_altidxlbn==24
     * shows the same VBN/LBN divergence), and extent 3 is the index file
     * bitmap immediately followed by the header area (VBN 5 onward). See
     * ods2.h [F3] for why extent 1 starts at LBN 0 (the boot block, not
     * LBN 1) -- unchanged from before. */
    put16(hb + offsetof(ods2_home_t, hm2_homevbn),   2);
    put16(hb + offsetof(ods2_home_t, hm2_alhomevbn), 3);
    put16(hb + offsetof(ods2_home_t, hm2_altidxvbn), 4);
    put16(hb + offsetof(ods2_home_t, hm2_ibmapvbn),  5);

    put32(hb + offsetof(ods2_home_t, hm2_ibmaplbn),  wvol->ibmap_lbn);
    put32(hb + offsetof(ods2_home_t, hm2_maxfiles),  wvol->maxfiles);
    put16(hb + offsetof(ods2_home_t, hm2_ibmapsize), (uint16_t)wvol->ibmap_size);
    put16(hb + offsetof(ods2_home_t, hm2_resfiles),  ODS2_RESFILES);
    put16(hb + offsetof(ods2_home_t, hm2_devtype),   0);
    put16(hb + offsetof(ods2_home_t, hm2_rvn),       0);
    put16(hb + offsetof(ods2_home_t, hm2_setcount),  0);
    put16(hb + offsetof(ods2_home_t, hm2_volchar),   0);

    /* Defaults observed on the real fixture (protect, fileprot, window,
     * lru_lim, extend) -- see PROVENANCE-real_vax_ods2.md addendum. */
    put16(hb + offsetof(ods2_home_t, hm2_protect),   0x0000);
    put16(hb + offsetof(ods2_home_t, hm2_fileprot),  0xFA00);
    hb[offsetof(ods2_home_t, hm2_window)]  = 7;
    hb[offsetof(ods2_home_t, hm2_lru_lim)] = 3;
    put16(hb + offsetof(ods2_home_t, hm2_extend),    5);
    /* [F11] hm2_volowner: SYSTEM [1,4], matching EVERY reserved/created
     * file's fh2_fileowner on the real fixture (see write_fh2_header's
     * [F11] comment) -- previously left zero. Increment 4 tried patching
     * ONLY this field in isolation (see [F9]) with no MOUNT change; kept
     * here now that it is set CONSISTENTLY with every file's own owner
     * field too, not on its own. */
    put16(hb + offsetof(ods2_home_t, hm2_volowner) + 0, 4); /* member */
    put16(hb + offsetof(ods2_home_t, hm2_volowner) + 2, 1); /* group  */

    vn_len = strlen(params->volname);
    if (vn_len > 12) vn_len = 12;
    memset(volname, ' ', 12);
    memcpy(volname, params->volname, vn_len);
    memcpy(hb + offsetof(ods2_home_t, hm2_volname), volname, 12);

    memcpy(hb + offsetof(ods2_home_t, hm2_format), ODS2_FORMAT_STRING,
           ODS2_FORMAT_LEN);

    /* checksum1: first 29 words (bytes 0..57) -> offset 58 */
    put16(hb + offsetof(ods2_home_t, hm2_checksum1), ods2_checksum(hb, 29));
    /* checksum2: first 255 words (bytes 0..509) -> offset 510 */
    put16(hb + offsetof(ods2_home_t, hm2_checksum2), ods2_block_checksum(hb));
}

/* ================================================================
 * ods2_volume_format() / ods2_wvolume_format_bdev()
 *
 * format_common() is the SHARED body -- everything that was originally
 * ods2_volume_format() from "---- fixed layout ----" onward, unchanged --
 * reaching the volume purely through wvol/wblk(), so it runs identically in
 * either mode. Split out in increment 11 (vms-6d3b) so the block-device-
 * backed entry point below does not duplicate any byte-encoding logic.
 * ================================================================ */

static ods2_status_t format_common(ods2_wvolume_t *wvol,
                                   const ods2_format_params_t *params)
{
    uint32_t total_blocks = wvol->nblocks, maxfiles = wvol->maxfiles;
    uint32_t ibmap_size, hdr_base, altidx_lbn;
    uint32_t bitmap_scb_lbn, bitmap_bits_blocks, bitmap_total;
    uint32_t mfd_lbn, security_lbn, reserved_end;
    ods2_status_t st;
    ods2_uic_t system_uic;

    /* ---- fixed layout ----
     * [F12] (increment 6, vms-0f3): PHYSICAL LBN order now matches the
     * real fixture's own observed order exactly (decoded directly off
     * tests/ods2/real_vax_ods2.dsk: FID2/BITMAP.SYS's own retrieval
     * pointer is (5,2) and FID4/000000.DIR's is (3,2), i.e. BOTH sit
     * BETWEEN the home block pair and the index file bitmap on a real
     * volume -- NOT after the header area, as an earlier revision of this
     * writer placed them). INDEXF.SYS's own retrieval map, correspondingly,
     * is now genuinely 3 extents (matching the real fixture's own FID1 --
     * map_inuse==6 there, not 2) instead of one flattened run spanning the
     * whole file -- see write_indexf_header() below. This removes the
     * ods2.h WRITER section's previously-flagged "INDEXF.SYS single-
     * contiguous-extent" [OVMX-inferred] simplification.
     *
     * LBN 0              : boot block (left zero)
     * LBN 1              : home block (primary)   [VBN1 of INDEXF.SYS]
     * LBN 2              : home block (alternate) [VBN2 of INDEXF.SYS]
     * LBN mfd_lbn        : MFD ([000000]) data block           -- NOT part
     *                      of INDEXF.SYS's own retrieval map (it belongs to
     *                      FID 4's own header/map), exactly like the real
     *                      fixture's LBN 3-4.
     * LBN bitmap_scb_lbn : BITMAP.SYS's SCB (its own VBN1)     -- likewise
     * LBN +1..+bb        : BITMAP.SYS's storage bitmap bits (its VBN2..)   owned
     *                      by FID 2's own map, not INDEXF.SYS's.
     * LBN ibmap_lbn..+isz-1: index file bitmap       [VBN5.. of INDEXF.SYS,
     *                      via its 3rd extent -- see write_indexf_header()]
     * LBN hdr_base..+mf-1: FID headers 1..maxfiles -- the FULL header area,
     *                      not just the 10 reserved ones: the reader (and
     *                      any real ODS-2 driver) computes a file's header
     *                      LBN as hdr_base + (fid_num - 1) for ANY valid
     *                      fid_num up to maxfiles, so nothing else may be
     *                      placed inside this range or user-file headers
     *                      (fid > ODS2_RESFILES) collide with it. Still
     *                      contiguous with ibmap_lbn (both live in
     *                      INDEXF.SYS's 3rd extent), so the physical LBN
     *                      formula above is unchanged.
     * LBN altidx_lbn     : alternate index header (copy of FID1's header)
     *                      -- physically AFTER the header area (like the
     *                      real fixture's own LBN 24, right after its own
     *                      LBN 8-23 header area) but VBN4 (INDEXF.SYS's
     *                      2nd extent, right after the home-block pair),
     *                      exactly matching the real fixture's own
     *                      hm2_altidxvbn==4 despite altidxlbn==24 -- i.e.
     *                      VBN order and LBN order genuinely diverge here,
     *                      as they do on the real volume.
     * LBN security_lbn..+5: SECURITY.SYS's 6-block CONTIG data extent
     *                      (only VBN1/security_lbn carries content)
     *
     * SECURITY.SYS (FID 10): the SPECIFIC increment-3 failure mode
     * ("%MOUNT-F-BADSECSYS -SYSTEM-E-BADCHECKSUM") -- real MOUNT enforcing
     * a genuine checksum over VBN1's content -- is RESOLVED in increment 4;
     * see ods2.h's WRITER [F6] provenance comment for the clean-room
     * (differential lab-2 observation, not disassembly or verbatim byte
     * copying) derivation of the checksum algorithm and data-block layout.
     * See [F9]/[F12] in ods2.h for the FILENUMCHK investigation history.
     */
    mfd_lbn        = 3;                 /* MFD data: 1 block, matches [3,2)
                                          * in spirit -- see the [OVMX-
                                          * inferred] MFD-is-1-block note
                                          * elsewhere in this function. */
    bitmap_bits_blocks = (total_blocks + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    if (bitmap_bits_blocks < 1) bitmap_bits_blocks = 1;
    bitmap_scb_lbn = mfd_lbn + 1;
    bitmap_total   = 1 + bitmap_bits_blocks; /* SCB + bit blocks */

    ibmap_size = (maxfiles + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    if (ibmap_size < 1) ibmap_size = 1;
    hdr_base   = bitmap_scb_lbn + bitmap_total + ibmap_size;
    altidx_lbn = hdr_base + maxfiles;   /* right after the FULL header area */

    security_lbn = altidx_lbn + 1;
    reserved_end = security_lbn + ODS2_SECURITY_DATA_BLOCKS; /* first LBN
                                        available to callers */

    if (reserved_end >= total_blocks)
        return ODS2_ERR_SIZE;   /* volume too small for the fixed layout */

    /* Block-device-backed mode only (increment 11): seed the ENTIRE fixed
     * reserved-layout region [0, reserved_end) as zero before anything
     * below touches it -- the scoped equivalent of the in-memory path's
     * whole-image memset(0) at the top of ods2_volume_format(). No-op in
     * in-memory mode (already zeroed there). */
    wcache_seed_zero_range(wvol, 0, reserved_end);

    wvol->ibmap_lbn          = bitmap_scb_lbn + bitmap_total;
    wvol->ibmap_size         = ibmap_size;
    wvol->hdr_base_lbn       = hdr_base;
    wvol->bitmap_scb_lbn     = bitmap_scb_lbn;
    wvol->bitmap_data_blocks = bitmap_bits_blocks;
    wvol->next_free_lbn      = reserved_end;
    wvol->next_free_fid      = ODS2_RESFILES + 1;

    /* ---- index file bitmap: FIDs 1..RESFILES in use, rest free ---- */
    ods2_bitmap_init_all_set(wvol, wvol->ibmap_lbn, wvol->ibmap_size);
    {
        uint32_t i;
        for (i = 1; i <= ODS2_RESFILES; i++)
            ods2_bitmap_set(wvol, wvol->ibmap_lbn, i - 1, 1); /* mark IN USE */
        for (i = ODS2_RESFILES + 1; i <= maxfiles; i++)
            ods2_bitmap_set(wvol, wvol->ibmap_lbn, i - 1, 0); /* mark free */
    }

    /* ---- storage bitmap: init all free, then mark the fixed layout used --- */
    ods2_bitmap_init_all_set(wvol, wvol->bitmap_scb_lbn + 1, wvol->bitmap_data_blocks);
    storage_bitmap_mark_used(wvol, 0, reserved_end); /* LBN 0..reserved_end-1 */
    /* also mark padding bits beyond total_blocks (inside the last bitmap
     * block) as allocated, so they never get handed out. */
    {
        uint32_t nbits = wvol->bitmap_data_blocks * BITS_PER_BLOCK;
        uint32_t i;
        for (i = total_blocks; i < nbits; i++)
            ods2_bitmap_set(wvol, wvol->bitmap_scb_lbn + 1, i, 0);
    }

    /* ---- home block pair ---- */
    write_home_block(wvol, 1, 2, altidx_lbn, params);
    write_home_block(wvol, 2, 2, altidx_lbn, params);

    /* ---- reserved system files 1..10 ----
     * [F2] (see ods2.h): every reserved file's fh2_backlink points to the
     * MFD (FID 4, seq 4 -- including 000000.DIR's own header, which
     * backlinks to itself), and each reserved file's fh2_fid.seq equals
     * its own fid_num. */
    {
        ods2_fid_t mfd_bl;
        mfd_bl.fid_num = ODS2_FID_MFD;
        mfd_bl.fid_seq = ODS2_FID_MFD;
        mfd_bl.fid_rvn = 0;
        mfd_bl.fid_nmx = 0;

        /* [F12] INDEXF.SYS's own retrieval map: genuinely 3 extents,
         * matching the real fixture's own FID1 (map_inuse==6, decoded
         * exactly as extents (0,3),(altidxlbn,1),(ibmaplbn,ibsz+maxfiles)
         * from tests/ods2/real_vax_ods2.dsk -- see the [F12] layout
         * comment above ods2_volume_format()'s LBN arithmetic). This
         * replaces increment 3's single-contiguous-extent simplification
         * (previously [OVMX-inferred], flagged in ods2.h as the leading
         * FILENUMCHK candidate). */
        {
            ods2_extent_t indexf_ext[3];
            indexf_ext[0].lbn = 0;              indexf_ext[0].count = 3;
            indexf_ext[1].lbn = altidx_lbn;     indexf_ext[1].count = 1;
            indexf_ext[2].lbn = wvol->ibmap_lbn;
            indexf_ext[2].count = ibmap_size + maxfiles;

            st = write_fh2_header_ext(wvol, ODS2_FID_INDEXF, ODS2_FID_INDEXF,
                                      "INDEXF.SYS", 1, 0, FH2_KIND_SYSTEM,
                                      indexf_ext, 3, 0, mfd_bl);
            if (st != ODS2_OK) return st;
            /* [F12] efblk = hiblk+1 -- matches the real fixture's own
             * INDEXF.SYS (hiblk=21/efblk=22) and BITMAP.SYS (hiblk=2/
             * efblk=3) "+1" convention (see the ods2_recattr_t comment's
             * cited real values); write_fh2_header_ext's generic
             * hiblk==efblk convention (used as-is for plain data files)
             * doesn't reproduce it, so patched directly here exactly like
             * SECURITY.SYS's existing [F7] patch below. */
            {
                uint8_t *ih = wblk(wvol, wvol->hdr_base_lbn + (ODS2_FID_INDEXF - 1));
                uint32_t indexf_hiblk = 3 + 1 + (ibmap_size + maxfiles);
                size_t efblk_off = offsetof(ods2_fh2_t, fh2_recattr) +
                                   offsetof(ods2_recattr_t, fat_efblk);
                uint32_t efblk1 = indexf_hiblk + 1;
                put16(ih + efblk_off + 0, (uint16_t)(efblk1 >> 16));
                put16(ih + efblk_off + 2, (uint16_t)(efblk1 & 0xFFFF));
                put16(ih + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(ih));
            }
        }
        st = write_fh2_header(wvol, ODS2_FID_BITMAP, ODS2_FID_BITMAP,
                              "BITMAP.SYS", 1, ODS2_FH2_M_CONTIG, FH2_KIND_SYSTEM,
                              wvol->bitmap_scb_lbn, bitmap_total, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        /* [F12] efblk = hiblk+1, same real-fixture "+1" convention as
         * INDEXF.SYS above (real BITMAP.SYS: hiblk=2/efblk=3). */
        {
            uint8_t *bh = wblk(wvol, wvol->hdr_base_lbn + (ODS2_FID_BITMAP - 1));
            size_t efblk_off = offsetof(ods2_fh2_t, fh2_recattr) +
                               offsetof(ods2_recattr_t, fat_efblk);
            uint32_t efblk1 = bitmap_total + 1;
            put16(bh + efblk_off + 0, (uint16_t)(efblk1 >> 16));
            put16(bh + efblk_off + 2, (uint16_t)(efblk1 & 0xFFFF));
            put16(bh + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(bh));
        }
        st = write_fh2_header(wvol, ODS2_FID_BADBLK, ODS2_FID_BADBLK,
                              "BADBLK.SYS", 1, 0, FH2_KIND_SYSTEM, 0, 0, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_MFD, ODS2_FID_MFD,
                              "000000.DIR", 1,
                              ODS2_FH2_M_CONTIG | ODS2_FH2_M_DIRECTORY,
                              FH2_KIND_DIR, mfd_lbn, 1, 0, mfd_bl); /* self */
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_CORIMG, ODS2_FID_CORIMG,
                              "CORIMG.SYS", 1, 0, FH2_KIND_SYSTEM, 0, 0, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_VOLSET, ODS2_FID_VOLSET,
                              "VOLSET.SYS", 1, 0, FH2_KIND_SYSTEM, 0, 0, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_CONTIN, ODS2_FID_CONTIN,
                              "CONTIN.SYS", 1, 0, FH2_KIND_SYSTEM, 0, 0, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_BACKUP, ODS2_FID_BACKUP,
                              "BACKUP.SYS", 1, 0, FH2_KIND_SYSTEM, 0, 0, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_BADLOG, ODS2_FID_BADLOG,
                              "BADLOG.SYS", 1, 0, FH2_KIND_SYSTEM, 0, 0, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        /* Genuine data extent (increment 4, [F6] -- see ods2.h): 6
         * contiguous blocks, matching the real fixture's observed size. */
        st = write_fh2_header(wvol, ODS2_FID_SECURITY, ODS2_FID_SECURITY,
                              "SECURITY.SYS", 1, ODS2_FH2_M_CONTIG, FH2_KIND_SYSTEM,
                              security_lbn, ODS2_SECURITY_DATA_BLOCKS, 0, mfd_bl);
        if (st != ODS2_OK) return st;
        /*
         * [F7] The real fixture's OWN SECURITY.SYS header shows hiblk=6/
         * efblk=2 (re-decoded field-by-field this increment via
         * tests/ods2/PROVENANCE-real_vax_ods2.md's real_vax_ods2.dsk
         * fixture, hdr_base derived from ITS OWN home block rather than
         * assumed) -- NOT the generic hiblk==efblk==map_count==6
         * convention write_fh2_header() uses for INDEXF.SYS/BITMAP.SYS.
         * Patched here to match exactly. NOTE (see ods2.h's [F9] status
         * comment): this patch alone did NOT resolve the real-MOUNT
         * FILENUMCHK failure that survives the [F6] checksum fix --
         * bisection proved that failure is NOT in SECURITY.SYS's header or
         * content at all. Kept anyway because it is still real, oracle-
         * grounded byte-genuineness, independent of whether it is
         * load-bearing for MOUNT.
         */
        {
            uint8_t *sech = wblk(wvol, wvol->hdr_base_lbn + (ODS2_FID_SECURITY - 1));
            size_t efblk_off = offsetof(ods2_fh2_t, fh2_recattr) +
                               offsetof(ods2_recattr_t, fat_efblk);
            size_t ffbyte_off = offsetof(ods2_fh2_t, fh2_recattr) +
                                offsetof(ods2_recattr_t, fat_ffbyte);
            put16(sech + efblk_off + 0, 0);   /* efblk high word */
            put16(sech + efblk_off + 2, 2);   /* efblk low word  */
            put16(sech + ffbyte_off, 0);
            put16(sech + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(sech));
        }
    }

    /* alternate index header: exact duplicate of FID1's header block
     * (matches the real fixture's byte-identical altidxlbn block). */
    memcpy(wblk(wvol, altidx_lbn), wblk(wvol, hdr_base + (ODS2_FID_INDEXF - 1)),
           ODS2_BLOCK_SIZE);

    /* BITMAP.SYS's SCB (its own VBN1). */
    {
        uint8_t *scb = wblk(wvol, bitmap_scb_lbn);
        memset(scb, 0, ODS2_BLOCK_SIZE);
        put16(scb + offsetof(ods2_scb_t, scb_struclev), ODS2_STRUCLEV_V2);
        put16(scb + offsetof(ods2_scb_t, scb_cluster),  1);
        put32(scb + offsetof(ods2_scb_t, scb_volsize),  total_blocks);
        put32(scb + offsetof(ods2_scb_t, scb_blksize),  1); /* [OVMX-inferred] */
        /* No physical device geometry claimed for a virtual volume; left
         * zero unless the caller's total_blocks matches a known real
         * geometry the operator chooses to encode themselves. */
        put16(scb + offsetof(ods2_scb_t, scb_checksum), ods2_block_checksum(scb));
    }

    /* MFD data block: empty directory (all-0xFF -> ODS2_DIR_END at offset0). */
    memset(wblk(wvol, mfd_lbn), 0xFF, ODS2_BLOCK_SIZE);

    /* SECURITY.SYS VBN1: genuine "zero ACL entries" data block (increment
     * 4, [F6]). VBN2..6 stay zeroed (already zero from the top-of-function
     * memset), matching the real fixture where only VBN1 carries content.
     * Owner UIC: SYSTEM [1,4] -- every real trial used the implicit
     * default (no /OWNER_UIC given); this writer does not yet parameterize
     * volume ownership elsewhere either (hm2_volowner above is also left
     * zero), so SYSTEM is the consistent choice here too. The label is
     * truncated to 12 chars first, matching write_home_block()'s own
     * silent truncation of an over-long params->volname above -- so both
     * records agree on the same (possibly-truncated) label. */
    {
        char sec_volname[13];
        size_t sec_vn_len = strlen(params->volname);
        if (sec_vn_len > 12) sec_vn_len = 12;
        memcpy(sec_volname, params->volname, sec_vn_len);
        sec_volname[sec_vn_len] = '\0';

        system_uic.uic_member = 4;
        system_uic.uic_group  = 1;
        st = ods2_security_build(wblk(wvol, security_lbn), sec_volname, system_uic);
        if (st != ODS2_OK) return st;
    }

    /*
     * [F10] wvol->mfd_fid is handed back to callers as the `parent_dir` FID
     * for anything created directly inside [000000] (ods2_wvolume_create_dir/
     * create_file build the new file's fh2_backlink from exactly this
     * struct). It MUST equal what is actually stored in the MFD's own
     * on-disk fh2_fid -- which write_fh2_header(ODS2_FID_MFD, ODS2_FID_MFD,
     * ...) above wrote with seq == ODS2_FID_MFD (4), matching every other
     * reserved file's fh2_fid.seq == its own fid_num (see [F2]) and the
     * real fixture's own FID4 header (fid=(4,4,0,0), decoded in
     * PROVENANCE-real_vax_ods2.md). A prior revision set fid_seq = 1 here
     * (a plain copy-paste of the "first generation" convention used for
     * NEWLY CREATED files), which made every caller-created top-level
     * file/dir's backlink read (num=4, seq=1) -- disagreeing with the MFD
     * header's own self-declared (num=4, seq=4). Bug found in increment 6
     * (vms-0f3) while auditing every FID the writer manufactures for
     * internal self-consistency: Nankervis's accesshead() (access.c,
     * simh/simtools) rejects a file lookup whenever the header found at a
     * FID's computed position does not self-report that SAME num+seq
     * (SS$_NOSUCHFILE there; real VMS's MOUNT-time equivalent is
     * "-SYSTEM-W-FILENUMCHK, file identification number check" -- the
     * exact secondary status this writer's own real-VAX MOUNT trials
     * reproduce, see PROVENANCE increment-4/5 addenda). [000000] is
     * scanned by name during MOUNT's quota/security file lookups (the
     * QUOTAFAIL warning immediately preceding each FILENUMCHK line is
     * QUOTA.SYS's OWN by-name lookup failing not-found, in the identical
     * MOUNT phase) -- so a mismatched backlink on a directory record
     * MOUNT's directory scan visits while hunting for SECURITY.SYS is a
     * concrete, self-consistency defect that would surface exactly there.
     */
    wvol->mfd_fid.fid_num = ODS2_FID_MFD;
    wvol->mfd_fid.fid_seq = ODS2_FID_MFD;
    wvol->mfd_fid.fid_rvn = 0;
    wvol->mfd_fid.fid_nmx = 0;

    /*
     * [F8] Directory entries for the 10 reserved system files in [000000],
     * INCLUDING SECURITY.SYS. Discovered lab-2-side while chasing a SECOND
     * real-MOUNT failure that survived the [F6] checksum fix: even with a
     * byte-identical (to a real, working fixture) SECURITY.SYS header AND
     * data content spliced into this writer's own volume, a real VAX still
     * rejected it with "%MOUNT-F-BADSECSYS -SYSTEM-W-FILENUMCHK" -- proving
     * the defect was NOT in SECURITY.SYS's own header/content at all. A
     * control mount of an untouched real fixture came back completely
     * clean (no QUOTAFAIL, no BADSECSYS), and decoding that real fixture's
     * OWN [000000] directory data block (`strings` over it) showed named
     * entries for 000000.DIR, INDEXF.SYS, BITMAP.SYS, BADBLK.SYS,
     * CORIMG.SYS, VOLSET.SYS, CONTIN.SYS, BACKUP.SYS, BADLOG.SYS, and
     * SECURITY.SYS -- i.e. a real INIT lists ALL reserved files by name in
     * the MFD, not just user-created ones. This writer previously inserted
     * NONE of them (only caller-created files/dirs got directory records).
     * Reproducing that (matching real fixture behavior, not copying its
     * bytes) is a real, oracle-grounded fidelity improvement -- but by
     * itself it did NOT resolve the FILENUMCHK failure either (re-tested
     * after adding this). See ods2.h's [F9] status comment: that failure
     * is still open, isolated to somewhere else in this writer's volume-
     * wide structure, most likely the INDEXF.SYS single-contiguous-extent
     * simplification.
     */
    {
        static const char *resnames[ODS2_RESFILES] = {
            "INDEXF.SYS", "BITMAP.SYS", "BADBLK.SYS", "000000.DIR",
            "CORIMG.SYS", "VOLSET.SYS", "CONTIN.SYS", "BACKUP.SYS",
            "BADLOG.SYS", "SECURITY.SYS",
        };
        uint32_t fid;
        for (fid = 1; fid <= ODS2_RESFILES; fid++) {
            ods2_fid_t entry_fid;
            entry_fid.fid_num = (uint16_t)fid;
            entry_fid.fid_seq = (uint16_t)fid;
            entry_fid.fid_rvn = 0;
            entry_fid.fid_nmx = 0;
            st = ods2_wvolume_dir_insert(wvol, wvol->mfd_fid,
                                         resnames[fid - 1], 1, entry_fid);
            if (st != ODS2_OK) return st;
        }
    }

    if (wvol->is_bdev && wvol->io_error != ODS2_OK)
        return wvol->io_error;
    return ODS2_OK;
}

ods2_status_t ods2_volume_format(uint8_t *image, size_t image_len,
                                 const ods2_format_params_t *params,
                                 ods2_wvolume_t *wvol)
{
    uint32_t total_blocks, maxfiles;

    if (!image || !params || !wvol || !params->volname)
        return ODS2_ERR_ARGS;

    total_blocks = params->total_blocks;
    maxfiles     = params->maxfiles;
    if (total_blocks == 0 || (size_t)total_blocks * ODS2_BLOCK_SIZE > image_len)
        return ODS2_ERR_ARGS;
    if (maxfiles < ODS2_RESFILES)
        return ODS2_ERR_ARGS;

    memset(image, 0, (size_t)total_blocks * ODS2_BLOCK_SIZE);

    memset(wvol, 0, sizeof(*wvol));
    wvol->image     = image;
    wvol->image_len = image_len;
    wvol->nblocks   = total_blocks;
    wvol->maxfiles  = maxfiles;

    return format_common(wvol, params);
}

#ifndef OVMX_ODS2_KERNEL
/*
 * The fd-based block-device constructors (format_bdev / open_bdev) are
 * USERSPACE-only: they auto-detect volume size via lseek and attach an
 * fd-based reader (ods2_bdev_open). The kernel-resident writer has no fd; when
 * an in-kernel WRITE path is chartered (a later rung of epic vms-208) it adds
 * host-based twins. The block cache's own reads/writes go through the seam in
 * BOTH worlds, so create_file/create_dir/dir_insert/append still compile
 * kernel-resident (in-memory mode) unchanged.
 */

/*
 * Block-device-backed twin of ods2_volume_format() (increment 11, vms-6d3b).
 * See ods2.h's "BLOCK-DEVICE-BACKED WRITER" section for the full design.
 */
ods2_status_t ods2_wvolume_format_bdev(int fd, uint64_t span_bytes,
                                       const ods2_format_params_t *params,
                                       ods2_wvolume_t *wvol)
{
    uint32_t total_blocks, maxfiles;
    ods2_status_t st;

    if (fd < 0 || !params || !wvol || !params->volname)
        return ODS2_ERR_ARGS;

    /* Auto-detect the volume size when the caller passes 0, same convention
     * as ods2_bdev_open(). */
    if (span_bytes == 0) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0)
            return ODS2_ERR_IO;
        span_bytes = (uint64_t)end;
    }

    total_blocks = params->total_blocks;
    maxfiles     = params->maxfiles;
    if (total_blocks == 0 ||
        (uint64_t)total_blocks * ODS2_BLOCK_SIZE > span_bytes)
        return ODS2_ERR_ARGS;
    if (maxfiles < ODS2_RESFILES)
        return ODS2_ERR_ARGS;

    memset(wvol, 0, sizeof(*wvol));
    wvol->is_bdev  = 1;
    wvol->bdev_fd  = fd;
    wvol->nblocks  = total_blocks;
    wvol->maxfiles = maxfiles;

    st = wcache_init(wvol);
    if (st != ODS2_OK)
        return st;

    st = format_common(wvol, params);
    if (st != ODS2_OK) {
        wcache_free(wvol);
        return st;
    }

    /* Commit the whole fixed reserved layout (home block pair, index file
     * bitmap, ten reserved files, BITMAP.SYS SCB + storage bitmap, MFD) to
     * the real block device via pwrite before returning -- no buffer sized
     * to total_blocks was ever allocated to get here. */
    st = ods2_wvolume_flush(wvol);
    if (st != ODS2_OK) {
        wcache_free(wvol);
        return st;
    }
    return ODS2_OK;
}

/* ================================================================
 * Reattach to an EXISTING volume for incremental write (vms-02e, epic
 * vms-5eb, the WRITE half of the ODS-2 runtime flip). See ods2.h's
 * ods2_wvolume_open_bdev() contract.
 * ================================================================ */

/*
 * Reconstruct the deterministic reserved-layout LBN fields into `wvol` from
 * its (already-set) nblocks/maxfiles -- the SAME arithmetic format_common()
 * runs (lines under its "---- fixed layout ----" comment). Kept a SEPARATE
 * function rather than refactored out of format_common() so the heavily
 * lab-validated golden format path is not disturbed; ods2_wvolume_open_bdev()
 * additionally cross-checks the result against the on-disk home block, so a
 * divergence between the two is caught at runtime, not silently mis-appended.
 * Returns the first LBN available to callers (reserved_end) via
 * *reserved_end_out, or ODS2_ERR_SIZE if the volume is too small.
 */
static ods2_status_t reconstruct_layout(ods2_wvolume_t *wvol,
                                        uint32_t *reserved_end_out)
{
    uint32_t total_blocks = wvol->nblocks, maxfiles = wvol->maxfiles;
    uint32_t ibmap_size, hdr_base, altidx_lbn;
    uint32_t bitmap_scb_lbn, bitmap_bits_blocks, bitmap_total;
    uint32_t mfd_lbn, security_lbn, reserved_end;

    mfd_lbn = 3;
    bitmap_bits_blocks = (total_blocks + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    if (bitmap_bits_blocks < 1) bitmap_bits_blocks = 1;
    bitmap_scb_lbn = mfd_lbn + 1;
    bitmap_total   = 1 + bitmap_bits_blocks;

    ibmap_size = (maxfiles + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    if (ibmap_size < 1) ibmap_size = 1;
    hdr_base   = bitmap_scb_lbn + bitmap_total + ibmap_size;
    altidx_lbn = hdr_base + maxfiles;
    security_lbn = altidx_lbn + 1;
    reserved_end = security_lbn + ODS2_SECURITY_DATA_BLOCKS;

    if (reserved_end >= total_blocks)
        return ODS2_ERR_SIZE;

    wvol->ibmap_lbn          = bitmap_scb_lbn + bitmap_total;
    wvol->ibmap_size         = ibmap_size;
    wvol->hdr_base_lbn       = hdr_base;
    wvol->bitmap_scb_lbn     = bitmap_scb_lbn;
    wvol->bitmap_data_blocks = bitmap_bits_blocks;

    *reserved_end_out = reserved_end;
    return ODS2_OK;
}

/*
 * Read one bit out of an on-disk bitmap region (storage bitmap or index-file
 * bitmap) via the block-backed READER -- the read-only inverse of the writer's
 * ods2_bitmap_set(). Used only by ods2_wvolume_open_bdev() to rebuild the bump
 * watermark; it reads a whole block per bit (a bounded, one-time scan), so it
 * stays out of the write cache entirely (open touches no write state).
 */
static ods2_status_t bdev_bitmap_get(const ods2_bdev_t *bv, uint32_t base_lbn,
                                     uint32_t bitno, int *out)
{
    uint32_t blk_idx  = bitno / BITS_PER_BLOCK;
    uint32_t in_block = bitno % BITS_PER_BLOCK;
    uint32_t word_idx = in_block / BITS_PER_WORD;
    uint32_t bit_idx  = in_block % BITS_PER_WORD;
    uint8_t buf[ODS2_BLOCK_SIZE];
    const uint8_t *b;
    uint32_t w;
    ods2_status_t st = ods2_bdev_read_block(bv, base_lbn + blk_idx,
                                            buf, sizeof(buf));
    if (st != ODS2_OK)
        return st;
    b = buf + (size_t)word_idx * 4;
    w = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    *out = (int)((w >> bit_idx) & 1u);
    return ODS2_OK;
}

ods2_status_t ods2_wvolume_open_bdev(int fd, uint64_t span_bytes,
                                     ods2_wvolume_t *wvol)
{
    ods2_bdev_t rd;
    ods2_scb_t  scb;
    uint8_t     blk[ODS2_BLOCK_SIZE];
    uint32_t    reserved_end, total_blocks, i;
    ods2_status_t st;

    if (fd < 0 || !wvol)
        return ODS2_ERR_ARGS;

    /* Validate + read the home block (checksums + DECFILE11B + strict level)
     * over the existing block-backed reader -- open_bdev attaches to a volume
     * already formatted; a non-genuine backing store fails here, honestly. */
    st = ods2_bdev_open(&rd, fd, span_bytes);
    if (st != ODS2_OK)
        return st;

    /* Authoritative total_blocks == the SCB's scb_volsize. BITMAP.SYS's VBN1
     * (the SCB) is always LBN 4 for this writer's fixed layout (mfd_lbn 3 + 1,
     * see format_common()); reusing scb_volsize keeps the reconstructed
     * bitmap geometry identical to the format that produced the volume. */
    st = ods2_bdev_read_block(&rd, 4, blk, sizeof(blk));
    if (st != ODS2_OK)
        return st;
    st = ods2_scb_parse(blk, sizeof(blk), &scb);
    if (st != ODS2_OK)
        return st;
    total_blocks = scb.scb_volsize;
    if (total_blocks == 0 || total_blocks > rd.nblocks)
        return ODS2_ERR_FORMAT;   /* SCB disagrees with the backing store */

    memset(wvol, 0, sizeof(*wvol));
    wvol->is_bdev  = 1;
    wvol->bdev_fd  = fd;
    wvol->nblocks  = total_blocks;
    wvol->maxfiles = rd.home.hm2_maxfiles;
    if (wvol->maxfiles < ODS2_RESFILES)
        return ODS2_ERR_FORMAT;

    st = reconstruct_layout(wvol, &reserved_end);
    if (st != ODS2_OK)
        return st;

    /* Cross-check the reconstructed geometry against the on-disk home block:
     * refuse (fail-honest) any volume whose layout this writer did not
     * produce -- e.g. a real-VAX INITIALIZE with a different bitmap placement
     * that open_bdev cannot safely continue allocating into. */
    if (wvol->ibmap_lbn      != rd.home.hm2_ibmaplbn ||
        wvol->ibmap_size     != rd.home.hm2_ibmapsize ||
        wvol->bitmap_scb_lbn != 4)
        return ODS2_ERR_FORMAT;

    /* next_free_lbn: the bump watermark == the first FREE storage-bitmap bit
     * (bit == 1 -> free, [N2]) at/after reserved_end. This writer's allocator
     * is a pure gapless bump with no deallocation, so the first free block
     * after the fixed layout is exactly where the next allocation continues. */
    {
        uint32_t nf = total_blocks;   /* volume full unless a free bit is found */
        for (i = reserved_end; i < total_blocks; i++) {
            int bit;
            st = bdev_bitmap_get(&rd, wvol->bitmap_scb_lbn + 1, i, &bit);
            if (st != ODS2_OK)
                return st;
            if (bit) { nf = i; break; }
        }
        wvol->next_free_lbn = nf;
    }

    /* next_free_fid: the first FREE index-file-bitmap bit (bit == 0 -> free,
     * [N2]) past the ten reserved files; FIDs are bump-allocated gaplessly. */
    {
        uint32_t nf = wvol->maxfiles + 1;   /* index full unless a free FID found */
        for (i = ODS2_RESFILES + 1; i <= wvol->maxfiles; i++) {
            int bit;
            st = bdev_bitmap_get(&rd, wvol->ibmap_lbn, i - 1, &bit);
            if (st != ODS2_OK)
                return st;
            if (!bit) { nf = i; break; }
        }
        wvol->next_free_fid = nf;
    }

    wvol->mfd_fid.fid_num = ODS2_FID_MFD;
    wvol->mfd_fid.fid_seq = ODS2_FID_MFD;
    wvol->mfd_fid.fid_rvn = 0;
    wvol->mfd_fid.fid_nmx = 0;

    st = wcache_init(wvol);
    if (st != ODS2_OK)
        return st;

    return ODS2_OK;
}
#endif /* !OVMX_ODS2_KERNEL -- fd-based block-device constructors */

/* ================================================================
 * Block / FID allocation
 * ================================================================ */

ods2_status_t ods2_wvolume_alloc_blocks(ods2_wvolume_t *wvol,
                                        uint32_t count, uint32_t *lbn_out)
{
    uint32_t lbn;

    if (!wvol || !lbn_out || count == 0)
        return ODS2_ERR_ARGS;
    if ((uint64_t)wvol->next_free_lbn + count > wvol->nblocks)
        return ODS2_ERR_NOSPACE;

    lbn = wvol->next_free_lbn;
    /* Block-device-backed mode only (increment 11): the caller is about to
     * overwrite [lbn, lbn+count) from scratch (a fresh file's data blocks,
     * a fresh header slot, a grown directory's new blocks) -- seed it zero
     * without a pread, the same "no ambient content matters" reasoning
     * format_common() applies to the fixed reserved-layout region. No-op in
     * in-memory mode (the whole image was already zeroed at format time). */
    wcache_seed_zero_range(wvol, lbn, count);
    storage_bitmap_mark_used(wvol, lbn, count);
    wvol->next_free_lbn += count;

    *lbn_out = lbn;
    if (wvol->is_bdev && wvol->io_error != ODS2_OK)
        return wvol->io_error;
    return ODS2_OK;
}

static ods2_status_t alloc_fid(ods2_wvolume_t *wvol, uint32_t *fid_out)
{
    if (!wvol || !fid_out)
        return ODS2_ERR_ARGS;
    if (wvol->next_free_fid > wvol->maxfiles)
        return ODS2_ERR_NOSPACE;

    *fid_out = wvol->next_free_fid++;
    ifile_bitmap_mark_used(wvol, *fid_out);
    return ODS2_OK;
}

static void fid_from_num(uint32_t num, ods2_fid_t *out)
{
    out->fid_num = (uint16_t)(num & 0xFFFF);
    out->fid_seq = 1;
    out->fid_rvn = 0;
    out->fid_nmx = (uint8_t)(num >> 16);
}

/* ================================================================
 * File / directory creation
 * ================================================================ */

/*
 * [F16] (increment 9, vms-0f3): frame caller-supplied bytes as an RMS
 * variable-length (RFM=VAR) record stream -- the on-disk shape a real VAX
 * expects given this writer's own FH2_KIND_DATA recattr preset
 * (rtype=ODS2_RTYPE_VAR, rattrib=ODS2_RAT_CR, see write_fh2_header_ext()).
 * Previously ods2_wvolume_create_file() wrote `data` to disk completely
 * unframed -- a real VAX's DUMP proved the bytes were genuinely present,
 * but TYPE printed nothing: RMS's VAR-record reader expects every record
 * to begin with its own 2-byte little-endian length word, which a raw
 * byte copy never supplies (see tests/ods2/PROVENANCE-real_vax_ods2.md's
 * increment-8 addendum, where this was first isolated but not chased).
 *
 * [OVMX-inferred] `data` is treated as newline-delimited TEXT LINES, one
 * VMS record per line with the terminator itself stripped (a lone '\r'
 * immediately before the '\n' is stripped too, so CRLF source text also
 * works) -- i.e. the same shape COPY/EDIT produce on a real VAX. A
 * trailing '\n' does not produce a spurious empty final record; an
 * embedded blank line ("\n\n") does, matching ordinary line-splitting.
 * This is a design choice this writer makes for its own caller-supplied
 * byte buffers, not something read off any oracle -- arbitrary binary
 * content is out of scope.
 *
 * On-disk record framing itself IS oracle-grounded: see
 * ods2_var_records_decode()'s provenance comment in ods2.h -- 2-byte LE
 * length prefix, one 0x00 pad byte after an odd-length record to keep the
 * next length word word-aligned, and (confirmed by direct byte decode of
 * tests/ods2/real_vax_ods2.dsk's own HELLO.TXT, which spans 34 blocks)
 * NO extra padding at 512-byte block boundaries -- records may straddle
 * them freely.
 *
 * Two-pass: pass 1 (dst == NULL) computes the total framed length and the
 * longest record's content length (for fat_rsize, see [F16] in ods2.h)
 * without writing anything, so the caller can size/allocate the file's
 * data blocks first; pass 2 (dst != NULL, pointing at those now-allocated
 * blocks) performs the identical walk and actually writes the bytes.
 */
static void ods2_var_frame_lines(const uint8_t *data, size_t data_len,
                                 uint8_t *dst, size_t *total_len_out,
                                 uint16_t *max_reclen_out)
{
    size_t i, line_start = 0, off = 0;
    uint16_t max_reclen = 0;

    for (i = 0; i <= data_len; i++) {
        int at_end = (i == data_len);

        if (!at_end && data[i] != '\n')
            continue;
        if (at_end && line_start == data_len)
            break; /* no dangling partial line -- empty file, or data
                     * already ended cleanly on a '\n' */

        {
            size_t content_len = i - line_start;
            if (content_len > 0 && data[line_start + content_len - 1] == '\r')
                content_len--;               /* CRLF source text */
            if (content_len > max_reclen)
                max_reclen = (uint16_t)(content_len > 0xFFFFu ? 0xFFFFu
                                                               : content_len);

            if (dst) {
                put16(dst + off, (uint16_t)content_len);
                if (content_len > 0)
                    memcpy(dst + off + 2, data + line_start, content_len);
            }
            off += 2 + content_len;
            if (content_len & 1) {
                if (dst)
                    dst[off] = 0;             /* word-alignment pad byte */
                off++;
            }
        }
        line_start = i + 1;
    }

    *total_len_out = off;
    if (max_reclen_out)
        *max_reclen_out = max_reclen;
}

ods2_status_t ods2_wvolume_create_file(ods2_wvolume_t *wvol,
                                       const char *name, uint16_t version,
                                       const uint8_t *data, size_t data_len,
                                       ods2_fid_t parent_dir,
                                       ods2_fid_t *fid_out)
{
    uint32_t fidnum, lbn, nblocks;
    ods2_status_t st;
    size_t framed_len;
    uint16_t max_reclen;

    if (!wvol || !name || (!data && data_len > 0) || !fid_out)
        return ODS2_ERR_ARGS;

    /* Pass 1: size the framed record stream (see ods2_var_frame_lines()'s
     * provenance comment above) before allocating data blocks. */
    ods2_var_frame_lines(data, data_len, NULL, &framed_len, &max_reclen);

    nblocks = (uint32_t)((framed_len + ODS2_BLOCK_SIZE - 1) / ODS2_BLOCK_SIZE);
    if (nblocks == 0)
        nblocks = 1;   /* even a zero-length file gets one data block */

    st = ods2_wvolume_alloc_blocks(wvol, nblocks, &lbn);
    if (st != ODS2_OK)
        return st;

    /* Pass 2: write the same framing for real into the now-allocated
     * blocks. Tail padding beyond framed_len is already zero from
     * volume-format's initial memset (never reused, since the bump
     * allocator never revisits blocks). */
    if (framed_len > 0)
        ods2_var_frame_lines(data, data_len, wblk(wvol, lbn), &framed_len,
                             &max_reclen);

    st = alloc_fid(wvol, &fidnum);
    if (st != ODS2_OK)
        return st;

    /* seq == 1 (first generation) for a freshly created file; backlink ==
     * parent_dir. [F2] see ods2.h. `framed_len` (not the caller's raw
     * data_len) is what write_fh2_header_ext's FH2_KIND_DATA branch uses
     * to compute fh2_recattr's efblk/ffbyte -- it must match what is
     * actually on disk. */
    st = write_fh2_header(wvol, fidnum, 1, name, version, 0, FH2_KIND_DATA,
                          lbn, nblocks, framed_len, parent_dir);
    if (st != ODS2_OK)
        return st;

    /* [F16] fat_rsize = the longest record actually written (see ods2.h's
     * ods2_recattr_t comment) -- write_fh2_header_ext's generic
     * FH2_KIND_DATA preset always leaves rsize at 0 (correct for a
     * zero-record file), so patch it here exactly like the existing
     * INDEXF.SYS/BITMAP.SYS/SECURITY.SYS post-write field patches above. */
    if (max_reclen > 0) {
        uint8_t *fh = wblk(wvol, wvol->hdr_base_lbn + (fidnum - 1));
        size_t rsize_off = offsetof(ods2_fh2_t, fh2_recattr) +
                           offsetof(ods2_recattr_t, fat_rsize);
        put16(fh + rsize_off, max_reclen);
        put16(fh + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(fh));
    }

    fid_from_num(fidnum, fid_out);
    return wvol_commit(wvol, ODS2_OK);
}

ods2_status_t ods2_wvolume_create_file_raw(ods2_wvolume_t *wvol,
                                           const char *name, uint16_t version,
                                           const uint8_t *data, size_t data_len,
                                           ods2_fid_t parent_dir,
                                           ods2_fid_t *fid_out)
{
    uint32_t fidnum, lbn, nblocks, b;
    ods2_status_t st;

    if (!wvol || !name || (!data && data_len > 0) || !fid_out)
        return ODS2_ERR_ARGS;

    nblocks = (uint32_t)((data_len + ODS2_BLOCK_SIZE - 1) / ODS2_BLOCK_SIZE);
    if (nblocks == 0)
        nblocks = 1;   /* even a zero-length file gets one data block */

    st = ods2_wvolume_alloc_blocks(wvol, nblocks, &lbn);
    if (st != ODS2_OK)
        return st;

    /*
     * Copy `data` VERBATIM into the allocated contiguous run, block by
     * block via wblk() -- correct in BOTH in-memory mode (wblk returns a
     * flat-image pointer) and block-device-backed mode (wblk returns a
     * per-block cache entry, which is why this never memcpy's across a
     * 512-byte span). The tail of the last block is zero-padded; the
     * blocks were already zero-seeded by ods2_wvolume_alloc_blocks().
     */
    for (b = 0; b < nblocks; b++) {
        uint8_t *blk = wblk(wvol, lbn + b);
        size_t off = (size_t)b * ODS2_BLOCK_SIZE;
        size_t take = 0;
        if (off < data_len)
            take = data_len - off < ODS2_BLOCK_SIZE
                       ? data_len - off : ODS2_BLOCK_SIZE;
        if (take < ODS2_BLOCK_SIZE)
            memset(blk + take, 0, ODS2_BLOCK_SIZE - take);
        if (take > 0)
            memcpy(blk, data + off, take);
    }

    st = alloc_fid(wvol, &fidnum);
    if (st != ODS2_OK)
        return st;

    /* seq == 1 (first generation); backlink == parent_dir. `data_len` (the
     * verbatim byte count) drives write_fh2_header_ext's FH2_KIND_DATA_FIX
     * efblk/ffbyte, so ods2_recattr_data_bytes() reports exactly data_len. */
    st = write_fh2_header(wvol, fidnum, 1, name, version, 0, FH2_KIND_DATA_FIX,
                          lbn, nblocks, data_len, parent_dir);
    if (st != ODS2_OK)
        return st;

    fid_from_num(fidnum, fid_out);
    return wvol_commit(wvol, ODS2_OK);
}

ods2_status_t ods2_wvolume_create_dir(ods2_wvolume_t *wvol,
                                      const char *name, uint16_t version,
                                      ods2_fid_t parent_dir,
                                      ods2_fid_t *fid_out)
{
    uint32_t fidnum, lbn;
    ods2_status_t st;

    if (!wvol || !name || !fid_out)
        return ODS2_ERR_ARGS;

    st = ods2_wvolume_alloc_blocks(wvol, 1, &lbn);
    if (st != ODS2_OK)
        return st;

    memset(wblk(wvol, lbn), 0xFF, ODS2_BLOCK_SIZE); /* empty dir: no records */

    st = alloc_fid(wvol, &fidnum);
    if (st != ODS2_OK)
        return st;

    st = write_fh2_header(wvol, fidnum, 1, name, version,
                          ODS2_FH2_M_CONTIG | ODS2_FH2_M_DIRECTORY,
                          FH2_KIND_DIR, lbn, 1, 0, parent_dir);
    if (st != ODS2_OK)
        return st;

    fid_from_num(fidnum, fid_out);
    return wvol_commit(wvol, ODS2_OK);
}

/* ================================================================
 * Directory-record insertion.
 * ================================================================ */

/*
 * A directory file's block set, collected VBN-order from its FM2 map.
 * ODS2_WDIR_MAX_EXTENTS / _MAX_BLOCKS bound the on-stack work arrays: a
 * single FH2 map area holds at most ~70 retrieval pointers (byte 228..510,
 * 4 bytes each), and 256 blocks * ~20 records is thousands of entries --
 * far past anything the system-disk hierarchy (R6) builds. Overrun of
 * either is reported (ODS2_ERR_NOSPACE), never silently truncated.
 */
#define ODS2_WDIR_MAX_EXTENTS 70u
#define ODS2_WDIR_MAX_BLOCKS  256u

struct wdir_extents {
    ods2_extent_t ext[ODS2_WDIR_MAX_EXTENTS];
    unsigned      n;
    int           overflow;
};

static int wdir_extent_collect(const ods2_extent_t *ext, void *ctx)
{
    struct wdir_extents *c = (struct wdir_extents *)ctx;
    if (c->n < ODS2_WDIR_MAX_EXTENTS)
        c->ext[c->n] = *ext;
    else
        c->overflow = 1;
    c->n++;
    return 0;
}

/* [F13] Byte-wise ascending name order, shorter-is-smaller-on-a-shared-
 * prefix -- i.e. plain memcmp/strcmp semantics over the raw (unterminated)
 * name bytes. See ods2_wvolume_dir_insert()'s [F13] comment for why this
 * exists and how it was derived. */
static int dir_name_cmp(const char *a, unsigned alen, const char *b, unsigned blen)
{
    unsigned n = alen < blen ? alen : blen;
    int c = n ? memcmp(a, b, n) : 0;
    if (c != 0)
        return c;
    if (alen != blen)
        return alen < blen ? -1 : 1;
    return 0;
}

/*
 * [F17] (increment 10, vms-1bd): MULTI-BLOCK DIRECTORY GROWTH.
 *
 * ORACLE GROUNDING (Rule 8). The multi-block directory layout is NOT
 * groundable from tests/ods2/real_vax_ods2.dsk: that fixture's only
 * >1-block directory is its own MFD (FID 4), which reads as extent
 * [LBN 3, count 2] but efblk=2/ffbyte=0 -- i.e. VBN 2 (LBN 4) is
 * ALLOCATED SLACK holding NO records (a direct byte decode of LBN 4
 * shows it zero-filled, past EOF). So the fixture never demonstrates how
 * records DISTRIBUTE across two live blocks. That rule therefore comes
 * from the PUBLIC Files-11 spec (the on-disk directory structure), not
 * from the fixture, and is FLAGGED as such:
 *
 *   1. A directory file is a sequence of variable-length records sorted
 *      in ascending file-name order -- across the WHOLE file, not just
 *      within a block ([F13] already grounded the sort key against the
 *      fixture MFD's own 11 records).
 *   2. A directory record NEVER crosses a 512-byte virtual-block
 *      boundary. Each block holds a whole number of records terminated by
 *      a dir_size == 0xFFFF (ODS2_DIR_END) sentinel; when the next record
 *      would not fit, the block is left terminated and the record starts
 *      the next block. (This is exactly what the reader already assumes:
 *      ods2_dir_block_scan() decodes each block independently and stops
 *      at ODS2_DIR_END, and ods2_volume_list_dir()/ods2_bdev_list_dir()
 *      map-walk EVERY extent block. So "records do not straddle" is the
 *      invariant the validated reader already enforces -- distinct from a
 *      DATA file's VAR records, which the fixture proved MAY straddle,
 *      see [F16]/ods2_var_records_decode().)
 *   3. EFBLK/FFBYTE express an end-of-file POSITION, not allocation:
 *      efblk = (last block containing records) + 1, ffbyte = 0 ([F15],
 *      fixture-grounded on FID 4 and FID 11).
 *
 * IMPLEMENTATION. Because records must stay globally sorted and inserts
 * arrive in arbitrary order, each insert rebuilds the directory: flatten
 * every existing record (raw bytes -- verlimit/flags/all value entries
 * preserved verbatim, so [F13]/[F14] stay byte-exact) into one sorted
 * stream, splice the new record at its sorted position, then re-pack the
 * stream greedily into the file's blocks (rule 2 above, reserving the
 * 2-byte ODS2_DIR_END terminator per block exactly as the prior
 * single-block path did). If the packed form needs more blocks than the
 * file has, the extra blocks are allocated and the FH2 map + recattr are
 * rewritten (rule 3). A single-block directory re-packs byte-for-byte
 * identically to the prior in-place code, so the real-VAX-MOUNT-clean
 * [F13]/[F14]/[F15] output is unchanged when no growth occurs. FLAGGED
 * OPEN (n=0 fixture samples): the exact record-to-block DISTRIBUTION a
 * real VAX produces is unobserved; greedy first-fit packing satisfies
 * rules 1-3 and any real VAX MOUNT/DIRECTORY reads it, but is not claimed
 * to be byte-identical to what VMS's own directory maintenance would lay
 * down for the same insert sequence. See PROVENANCE-real_vax_ods2.md's
 * increment-10 addendum.
 */
/*
 * [vms-9794] Merge a new {version, entry_fid} value entry into an EXISTING
 * directory record for the SAME name, so a NAME can carry more than one
 * version (";2"/";3" of an already-inserted file) instead of the prior
 * hard reject on any duplicate name.
 *
 * GROUNDING (Rule 8): this adds NO new on-disk format fact. The reader
 * (ods2_dir_block_scan(), ods2_reader.c) ALREADY walks a record's value-
 * entry array as a `while (val_off + sizeof(ods2_dir_ent_t) <= rec_end)`
 * loop, i.e. it already expects zero or more {dir_version,dir_fid} 8-byte
 * entries per name -- this writer change only teaches ods2_wvolume_dir_
 * insert() to EMIT what the reader already consumes. The entry layout
 * itself (uint16 dir_version + 6-byte ods2_fid_t, 8 bytes, [N] direct.h
 * dir$ent) is unchanged and already cited in ods2.h. Ordering (descending
 * by version, highest first) matches ods2.h's directory-record comment
 * ("one or more value entries {version, fid} in DESCENDING version
 * order") and this file's own [F17] comment. dir_verlimit and dir_flags
 * are left byte-identical to the existing record -- [F14] already
 * establishes dir_verlimit is a per-NAME policy value, not per-version,
 * so a second/third version of the same name must not perturb it.
 *
 * `src` points at the existing record's dir_size word (blk+off); `src_len`
 * is that record's whole on-disk length (2 + dir_size). `namecount` is the
 * record's own dir_namecount (already validated by the caller against
 * `src_len`). Writes the merged record into `out` (caller-owned, must have
 * room for at least src_len + sizeof(ods2_dir_ent_t) bytes) and its length
 * into *out_len.
 *
 * Fail-honest (Rule 9 convention, no silent fabrication): ODS2_ERR_FORMAT
 * if the existing record's value-entry area is not a whole number of
 * 8-byte entries (corrupt directory); ODS2_ERR_ARGS if `version` already
 * has an entry for this name (duplicate version -- never silently
 * overwritten); ODS2_ERR_NOSPACE if the grown record would not fit in a
 * single 512-byte directory block (a record may never cross a block
 * boundary, [F17] rule 2).
 */
static ods2_status_t merge_dir_record(const uint8_t *src, unsigned src_len,
                                      unsigned namecount, uint16_t version,
                                      ods2_fid_t entry_fid,
                                      uint8_t *out, unsigned *out_len)
{
    unsigned val_off = 6u + namecount;
    unsigned n_old, i, new_idx;
    unsigned new_len;

    if (val_off & 1)
        val_off++;
    if (val_off > src_len || ((src_len - val_off) % 8u) != 0)
        return ODS2_ERR_FORMAT;           /* malformed existing record */
    n_old = (src_len - val_off) / 8u;

    /* Find the descending-order insertion index; reject an exact-version
     * duplicate rather than silently overwriting it. */
    new_idx = n_old;
    for (i = 0; i < n_old; i++) {
        uint16_t v = rd16(src + val_off + i * 8u);
        if (v == version)
            return ODS2_ERR_ARGS;         /* duplicate version */
        if (v < version) {
            new_idx = i;
            break;
        }
    }

    new_len = val_off + (n_old + 1u) * 8u;
    if (new_len > ODS2_BLOCK_SIZE - 2u)
        return ODS2_ERR_NOSPACE;          /* cannot fit any single block */

    memset(out, 0xFF, new_len);
    put16(out + 0, (uint16_t)(new_len - 2));      /* dir_size */
    memcpy(out + 2, src + 2, 4);                  /* verlimit + flags + namecount, unchanged */
    memcpy(out + 6, src + 6, namecount);           /* name bytes, unchanged */
    for (i = 0; i < new_idx; i++)
        memcpy(out + val_off + i * 8u, src + val_off + i * 8u, 8u);
    put16(out + val_off + new_idx * 8u + 0, version);
    put16(out + val_off + new_idx * 8u + 2, entry_fid.fid_num);
    put16(out + val_off + new_idx * 8u + 4, entry_fid.fid_seq);
    out[val_off + new_idx * 8u + 6] = entry_fid.fid_rvn;
    out[val_off + new_idx * 8u + 7] = entry_fid.fid_nmx;
    for (i = new_idx; i < n_old; i++)
        memcpy(out + val_off + (i + 1u) * 8u, src + val_off + i * 8u, 8u);

    *out_len = new_len;
    return ODS2_OK;
}

ods2_status_t ods2_wvolume_dir_insert(ods2_wvolume_t *wvol,
                                      ods2_fid_t dir_fid,
                                      const char *name, uint16_t version,
                                      ods2_fid_t entry_fid)
{
    uint32_t dir_fidnum;
    ods2_status_t st;
    struct wdir_extents exts;
    unsigned nlbn = 0, blocks_before, namecount, e, k, b;
    unsigned new_valoff, newrec_len;
    uint8_t *flat = NULL;
    size_t flatcap, flat_used = 0, insert_off = 0;
    int have_insert = 0, found_name = 0;
    unsigned nblk, cur;
    size_t foff;
    /*
     * rd vms-4a8: hdr[512] + hdr_parsed(512) + newrec[512] + lbns[256]*4(1024)
     * = ~2.6KB of on-stack scratch pushed this frame to 3288 bytes, over the
     * kernel's 2048-byte -Werror=frame-larger-than= limit once the codec is
     * compiled kernel-resident (OVMX_ODS2_KERNEL) -- the kernel stack is a few
     * pages and this is a leaf of the ACP call chain. Move them into ONE heap
     * block through the codec's allocator seam (ods2_kzalloc -> kvmalloc in the
     * kernel, malloc in userspace); byte-for-byte identical behaviour, freed
     * once at the single `done:` exit alongside `flat`.
     */
    struct di_scratch {
        uint8_t    hdr[ODS2_BLOCK_SIZE];
        ods2_fh2_t hdr_parsed;
        uint8_t    newrec[ODS2_BLOCK_SIZE];  /* one record is always < 1 block */
        uint32_t   lbns[ODS2_WDIR_MAX_BLOCKS];
    } *s = NULL;

    if (!wvol || !name)
        return ODS2_ERR_ARGS;

    namecount = (unsigned)strlen(name);
    if (namecount == 0 || namecount > 255)
        return ODS2_ERR_ARGS;

    /*
     * Read the directory's own header (already finalized/checksummed by an
     * earlier create_dir()/format() call) straight through wblk() -- the
     * SAME choke point every other helper in this file uses, mode-agnostic
     * between in-memory and block-device-backed. wvol->hdr_base_lbn is, by
     * construction, exactly what ods2_volume_read_header() would compute
     * from the home block itself (hm2_ibmaplbn + hm2_ibmapsize -- see
     * write_home_block() above, which writes hm2_ibmaplbn == wvol->
     * ibmap_lbn, and wvol->hdr_base_lbn == wvol->ibmap_lbn + wvol->
     * ibmap_size), so this is provably equivalent to the increment-1..10
     * "reopen a reader view" approach it replaces (increment 11, vms-6d3b)
     * -- it was needed as a reader-reuse convenience for the in-memory-only
     * `ods2_volume_open(vol, wvol->image, ...)` call, which has no
     * block-device-backed equivalent (there is no flat `image` to open a
     * view over in that mode). ods2_fh2_parse() still validates the header's
     * checksum exactly as ods2_volume_read_header() did internally.
     */
    dir_fidnum = ods2_fid_number(&dir_fid);
    if (dir_fidnum < 1)
        return ODS2_ERR_ARGS;
    if (dir_fidnum > wvol->maxfiles)
        return ODS2_ERR_RANGE;

    s = (struct di_scratch *)ods2_kzalloc(sizeof(*s));
    if (!s)
        return ODS2_ERR_NOSPACE;    /* honest failure -- never a silent fake */

    memcpy(s->hdr, wblk(wvol, wvol->hdr_base_lbn + (dir_fidnum - 1)), ODS2_BLOCK_SIZE);
    st = ods2_fh2_parse(s->hdr, ODS2_BLOCK_SIZE, &s->hdr_parsed);
    if (st != ODS2_OK)
        goto done;

    /* Collect the directory file's block set, VBN order. */
    exts.n = 0;
    exts.overflow = 0;
    st = ods2_fh2_map_walk(s->hdr, wdir_extent_collect, &exts, NULL);
    if (st != ODS2_OK)
        goto done;
    if (exts.n == 0 || exts.overflow) {
        st = ODS2_ERR_NOSPACE;
        goto done;
    }
    for (e = 0; e < exts.n; e++) {
        for (k = 0; k < exts.ext[e].count; k++) {
            if (nlbn >= ODS2_WDIR_MAX_BLOCKS) {
                st = ODS2_ERR_NOSPACE;
                goto done;
            }
            s->lbns[nlbn++] = exts.ext[e].lbn + k;
        }
    }
    blocks_before = nlbn;

    /* Build the new record's on-disk bytes. [F13] name-sorted position is
     * found below; [F14] dir_verlimit is the per-NAME version-LIMIT policy
     * (reserved files: their own locked version; caller-created: 0x7FFF),
     * NOT this entry's own version (that is dir_version, in the value
     * entry). memset 0xFF first so the name/value word-alignment pad byte
     * (present iff namecount is odd) reads back as the empty-fill 0xFF the
     * reader expects. */
    new_valoff = 6 + namecount;
    if (new_valoff & 1)
        new_valoff++;
    newrec_len = new_valoff + (unsigned)sizeof(ods2_dir_ent_t);
    memset(s->newrec, 0xFF, sizeof(s->newrec));
    put16(s->newrec + 0, (uint16_t)(newrec_len - 2));               /* dir_size */
    put16(s->newrec + 2,
          (entry_fid.fid_num <= ODS2_RESFILES) ? version
                                                : ODS2_DIR_VERLIMIT_DEFAULT);
    s->newrec[4] = 0;                                               /* dir_flags */
    s->newrec[5] = (uint8_t)namecount;                             /* dir_namecount */
    memcpy(s->newrec + 6, name, namecount);
    put16(s->newrec + new_valoff + 0, version);
    put16(s->newrec + new_valoff + 2, entry_fid.fid_num);
    put16(s->newrec + new_valoff + 4, entry_fid.fid_seq);
    s->newrec[new_valoff + 6] = entry_fid.fid_rvn;
    s->newrec[new_valoff + 7] = entry_fid.fid_nmx;

    /* Flatten every existing record (raw bytes) into one sorted stream and
     * locate the new name's sorted insertion point. Upper bound on the
     * stream is one full block per existing block plus the new record. */
    flatcap = (size_t)nlbn * ODS2_BLOCK_SIZE + newrec_len + 8;
    flat = (uint8_t *)ods2_kalloc(flatcap);
    if (!flat) {
        st = ODS2_ERR_NOSPACE;      /* honest failure -- never a silent fake */
        goto done;
    }

    for (b = 0; b < nlbn; b++) {
        const uint8_t *blk = wblk(wvol, s->lbns[b]);
        unsigned off = 0;
        for (;;) {
            uint16_t rec_size;
            unsigned reclen, nc;
            if (off + 6 > ODS2_BLOCK_SIZE)
                break;
            rec_size = rd16(blk + off);
            if (rec_size == ODS2_DIR_END)
                break;
            reclen = 2u + rec_size;
            if (off + reclen > ODS2_BLOCK_SIZE) {
                st = ODS2_ERR_FORMAT;        /* malformed existing block */
                goto done;
            }
            nc = blk[off + 5];
            if (6u + nc > reclen) {
                st = ODS2_ERR_FORMAT;
                goto done;
            }
            if (!found_name && !have_insert) {
                int cmp = dir_name_cmp(name, namecount,
                                       (const char *)blk + off + 6, nc);
                if (cmp == 0) {
                    /* [vms-9794] NAME already exists: merge the new
                     * {version, fid} into THIS record's value-entry array
                     * (in place of the record's raw bytes) instead of
                     * rejecting -- see merge_dir_record() above. The
                     * merged record replaces this record at its own sorted
                     * position; no separate splice is needed. */
                    unsigned merged_len;
                    st = merge_dir_record(blk + off, reclen, nc, version,
                                          entry_fid, flat + flat_used,
                                          &merged_len);
                    if (st != ODS2_OK)
                        goto done;
                    flat_used += merged_len;
                    found_name = 1;
                    off += reclen;
                    continue;
                }
                if (cmp < 0) {
                    insert_off = flat_used;
                    have_insert = 1;
                }
            }
            memcpy(flat + flat_used, blk + off, reclen);
            flat_used += reclen;
            off += reclen;
        }
    }

    if (!found_name) {
        if (!have_insert)
            insert_off = flat_used;     /* sorts after everything existing */

        /* Splice the new record in at its sorted byte offset. */
        if (insert_off < flat_used)
            memmove(flat + insert_off + newrec_len, flat + insert_off,
                    flat_used - insert_off);
        memcpy(flat + insert_off, s->newrec, newrec_len);
        flat_used += newrec_len;
    }

    /* Count blocks needed: greedy pack, a record never crosses a block
     * boundary, and every block keeps >=2 trailing bytes for its
     * ODS2_DIR_END terminator (the same +2 reservation the single-block
     * path enforced -- see [F17] rule 2). */
    nblk = 1;
    cur = 0;
    for (foff = 0; foff < flat_used; ) {
        unsigned reclen = 2u + rd16(flat + foff);
        if (cur + reclen + 2u > ODS2_BLOCK_SIZE) {
            nblk++;
            cur = 0;
        }
        cur += reclen;
        foff += reclen;
    }
    if (nblk > ODS2_WDIR_MAX_BLOCKS) {
        st = ODS2_ERR_NOSPACE;
        goto done;
    }

    /* Grow the directory file if the packed form needs more blocks. New
     * blocks come contiguous from the bump allocator (and are marked used
     * in BITMAP.SYS by ods2_wvolume_alloc_blocks). */
    if (nblk > blocks_before) {
        uint32_t extra = nblk - blocks_before;
        uint32_t grown_lbn;
        st = ods2_wvolume_alloc_blocks(wvol, extra, &grown_lbn);
        if (st != ODS2_OK)
            goto done;
        for (k = 0; k < extra; k++)
            s->lbns[nlbn++] = grown_lbn + k;
    }

    /* Lay the sorted stream into the (possibly grown) block set. Each block
     * is 0xFF-filled first so trailing space reads back as ODS2_DIR_END. */
    for (b = 0; b < nblk; b++)
        memset(wblk(wvol, s->lbns[b]), 0xFF, ODS2_BLOCK_SIZE);
    {
        unsigned bi = 0;
        uint8_t *dst = wblk(wvol, s->lbns[0]);
        cur = 0;
        for (foff = 0; foff < flat_used; ) {
            unsigned reclen = 2u + rd16(flat + foff);
            if (cur + reclen + 2u > ODS2_BLOCK_SIZE) {
                bi++;
                cur = 0;
                dst = wblk(wvol, s->lbns[bi]);
            }
            memcpy(dst + cur, flat + foff, reclen);
            cur += reclen;
            foff += reclen;
        }
    }
    ods2_kfree(flat);
    flat = NULL;

    /* Rewrite the FH2 map + recattr ONLY when the block count changed --
     * a same-block re-pack keeps the header byte-identical (map, hiblk,
     * efblk all unchanged), preserving the validated single-block output.
     * [F15]/[F17] rule 3: hiblk = allocated blocks, efblk = last-data-block
     * + 1, ffbyte = 0. */
    if (nblk > blocks_before) {
        uint32_t hlbn = wvol->hdr_base_lbn + (ods2_fid_number(&dir_fid) - 1);
        uint8_t *h = wblk(wvol, hlbn);
        uint8_t *mp_base = h + (size_t)MP_OFF_WORDS * 2;
        size_t map_cap = ODS2_BLOCK_SIZE - (size_t)MP_OFF_WORDS * 2 - 2;
        unsigned nx = 0;
        uint32_t hiblk = nblk, efblk = nblk + 1;

        memset(mp_base, 0, map_cap);     /* clear stale FM2 pointers */
        b = 0;
        while (b < nblk) {
            uint32_t run_lbn = s->lbns[b];
            uint32_t run = 1;
            while (b + run < nblk && run < 256 &&
                   s->lbns[b + run] == run_lbn + run)
                run++;
            if ((size_t)nx * 4 + 4 > map_cap) {
                st = ODS2_ERR_NOSPACE;   /* map area full */
                goto done;
            }
            st = encode_map_extent(mp_base + (size_t)nx * 4, run_lbn, run, NULL);
            if (st != ODS2_OK)
                goto done;
            nx++;
            b += run;
        }
        h[offsetof(ods2_fh2_t, fh2_map_inuse)] = (uint8_t)(nx * 2);

        put16(h + offsetof(ods2_fh2_t, fh2_recattr) +
              offsetof(ods2_recattr_t, fat_hiblk) + 0, (uint16_t)(hiblk >> 16));
        put16(h + offsetof(ods2_fh2_t, fh2_recattr) +
              offsetof(ods2_recattr_t, fat_hiblk) + 2, (uint16_t)(hiblk & 0xFFFF));
        put16(h + offsetof(ods2_fh2_t, fh2_recattr) +
              offsetof(ods2_recattr_t, fat_efblk) + 0, (uint16_t)(efblk >> 16));
        put16(h + offsetof(ods2_fh2_t, fh2_recattr) +
              offsetof(ods2_recattr_t, fat_efblk) + 2, (uint16_t)(efblk & 0xFFFF));
        put16(h + offsetof(ods2_fh2_t, fh2_recattr) +
              offsetof(ods2_recattr_t, fat_ffbyte), 0);
        put32(h + offsetof(ods2_fh2_t, fh2_highwater), hiblk + 1);
        put16(h + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(h));
    }

    st = wvol_commit(wvol, ODS2_OK);

done:
    /* Single-exit cleanup (rd vms-4a8): free the heap directory scratch and the
     * flatten buffer. `flat` is NULL on the success path (freed + cleared above)
     * and on the early failures before it was allocated, so the guard makes the
     * free idempotent; `s` is always live here (every goto is after its alloc). */
    if (flat)
        ods2_kfree(flat);
    ods2_kfree(s);
    return st;
}

/* ================================================================
 * Append to an EXISTING file (vms-02e, epic vms-5eb -- the WRITE half of the
 * ODS-2 runtime flip). See ods2.h's ods2_wvolume_append_file() contract.
 * ================================================================ */

/* Translate a 0-based file block index to its LBN through the extent list. */
static ods2_status_t append_block_lbn(const struct wdir_extents *ex,
                                      uint32_t block_index, uint32_t *lbn_out)
{
    uint32_t acc = 0, i;
    for (i = 0; i < ex->n; i++) {
        if (block_index < acc + ex->ext[i].count) {
            *lbn_out = ex->ext[i].lbn + (block_index - acc);
            return ODS2_OK;
        }
        acc += ex->ext[i].count;
    }
    return ODS2_ERR_RANGE;
}

ods2_status_t ods2_wvolume_append_file(ods2_wvolume_t *wvol,
                                       ods2_fid_t file_fid,
                                       const void *data, size_t data_len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t fidnum, hlbn, cur_hiblk, new_alloc, additional;
    size_t cur_valid, new_valid, k, bi0, bi1;
    uint8_t hdr_copy[ODS2_BLOCK_SIZE];
    ods2_fh2_t parsed;
    struct wdir_extents ex;
    unsigned mp_off_words;
    size_t map_cap;
    ods2_status_t st;
    uint8_t *h;

    if (!wvol || (!data && data_len > 0))
        return ODS2_ERR_ARGS;
    if (data_len == 0)
        return ODS2_OK;                 /* a zero-length append is a no-op */

    fidnum = ods2_fid_number(&file_fid);
    if (fidnum < 1 || fidnum > wvol->maxfiles)
        return ODS2_ERR_ARGS;

    /* Read + validate the file's existing header. wblk() pread-fills it in
     * block-device mode; parse a COPY so map-walk sees the ORIGINAL extents
     * while we patch `h` in place afterward. */
    hlbn = wvol->hdr_base_lbn + (fidnum - 1);
    h = wblk(wvol, hlbn);
    memcpy(hdr_copy, h, ODS2_BLOCK_SIZE);

    st = ods2_fh2_parse(hdr_copy, sizeof(hdr_copy), &parsed);
    if (st != ODS2_OK)
        return st;
    /* The header at this FID's slot must self-report this FID -- never extend
     * a stale/garbage block. */
    if (ods2_fid_number(&parsed.fh2_fid) != fidnum)
        return ODS2_ERR_NOTFOUND;
    /* Append is defined for VERBATIM (RFM=FIXED, create_file_raw-shaped)
     * data files only -- raw-appending a VAR text file or a directory would
     * corrupt its record/directory framing. Refuse honestly. */
    if (parsed.fh2_recattr.fat_rtype != ODS2_RTYPE_FIX ||
        (parsed.fh2_filechar & ODS2_FH2_M_DIRECTORY))
        return ODS2_ERR_ARGS;

    cur_hiblk = ods2_recattr_hiblk(&parsed.fh2_recattr);
    cur_valid = ods2_recattr_data_bytes(&parsed.fh2_recattr);
    new_valid = cur_valid + data_len;
    new_alloc = (uint32_t)((new_valid + ODS2_BLOCK_SIZE - 1) / ODS2_BLOCK_SIZE);
    if (new_alloc == 0)
        new_alloc = 1;
    additional = (new_alloc > cur_hiblk) ? (new_alloc - cur_hiblk) : 0;

    /* Collect the file's current retrieval-pointer extents (VBN order). */
    ex.n = 0;
    ex.overflow = 0;
    st = ods2_fh2_map_walk(hdr_copy, wdir_extent_collect, &ex, NULL);
    if (st != ODS2_OK)
        return st;
    if (ex.overflow)
        return ODS2_ERR_NOSPACE;

    /* Extend the allocation when the appended bytes overflow it. A single
     * bump allocation yields one contiguous run; chain it onto the map,
     * merging into the last extent where physically contiguous and within the
     * FM2 format-1 256-block-per-pointer cap, else adding new <=256 extents. */
    if (additional > 0) {
        uint32_t newlbn, at, remaining;
        st = ods2_wvolume_alloc_blocks(wvol, additional, &newlbn);
        if (st != ODS2_OK)
            return st;
        at = newlbn;
        remaining = additional;
        while (remaining > 0) {
            if (ex.n > 0 &&
                ex.ext[ex.n - 1].lbn + ex.ext[ex.n - 1].count == at &&
                ex.ext[ex.n - 1].count < 256) {
                uint32_t room = 256 - ex.ext[ex.n - 1].count;
                uint32_t take = remaining < room ? remaining : room;
                ex.ext[ex.n - 1].count += take;
                at += take;
                remaining -= take;
            } else {
                uint32_t take = remaining < 256 ? remaining : 256;
                if (ex.n >= ODS2_WDIR_MAX_EXTENTS)
                    return ODS2_ERR_NOSPACE;
                ex.ext[ex.n].lbn = at;
                ex.ext[ex.n].count = take;
                ex.n++;
                at += take;
                remaining -= take;
            }
        }
    }

    /* Write the appended bytes across the (now sufficient) extent map. Each
     * touched block goes through wblk(): the partially-filled last existing
     * block is pread so its head content is preserved; freshly allocated
     * blocks were zero-seeded by ods2_wvolume_alloc_blocks(). Never memcpy
     * across a 512-byte block boundary (block-device-backed cache is
     * per-block). */
    bi0 = cur_valid / ODS2_BLOCK_SIZE;
    bi1 = (new_valid - 1) / ODS2_BLOCK_SIZE;
    for (k = bi0; k <= bi1; k++) {
        uint32_t lbn;
        size_t blk_start = k * ODS2_BLOCK_SIZE;
        size_t blk_end   = blk_start + ODS2_BLOCK_SIZE;
        size_t lo = cur_valid > blk_start ? cur_valid : blk_start;
        size_t hi = new_valid < blk_end ? new_valid : blk_end;
        uint8_t *blk;
        st = append_block_lbn(&ex, (uint32_t)k, &lbn);
        if (st != ODS2_OK)
            return st;
        blk = wblk(wvol, lbn);
        memcpy(blk + (lo - blk_start), bytes + (lo - cur_valid), hi - lo);
    }

    /* Patch the FH2 header: grown extent map + map_inuse, new end-of-file
     * position (fat_hiblk/efblk/ffbyte -- the FH2_KIND_DATA_FIX convention:
     * efblk == allocated blocks, ffbyte == bytes in the last block), highwater
     * (first never-written VBN == hiblk+1, [F11]), and a fresh checksum.
     * Re-fetch `h` (the sparse cache never moves a live entry, but be
     * explicit) and use the file's OWN stored map offset. */
    h = wblk(wvol, hlbn);
    mp_off_words = h[offsetof(ods2_fh2_t, fh2_mpoffset)];
    map_cap = ODS2_BLOCK_SIZE - (size_t)mp_off_words * 2 - 2;
    if ((size_t)ex.n * 4 > map_cap)
        return ODS2_ERR_NOSPACE;        /* map area cannot hold the extents */
    {
        uint8_t *mp_base = h + (size_t)mp_off_words * 2;
        unsigned ei;
        memset(mp_base, 0, map_cap);    /* clear any stale FM2 pointers */
        for (ei = 0; ei < ex.n; ei++) {
            st = encode_map_extent(mp_base + (size_t)ei * 4,
                                   ex.ext[ei].lbn, ex.ext[ei].count, NULL);
            if (st != ODS2_OK)
                return st;
        }
        h[offsetof(ods2_fh2_t, fh2_map_inuse)] = (uint8_t)(ex.n * 2);
    }
    {
        size_t ra = offsetof(ods2_fh2_t, fh2_recattr);
        uint32_t hiblk = new_alloc, efblk = new_alloc;
        uint16_t ffbyte =
            (uint16_t)(new_valid - (size_t)(new_alloc - 1) * ODS2_BLOCK_SIZE);
        put16(h + ra + offsetof(ods2_recattr_t, fat_hiblk) + 0,
              (uint16_t)(hiblk >> 16));
        put16(h + ra + offsetof(ods2_recattr_t, fat_hiblk) + 2,
              (uint16_t)(hiblk & 0xFFFF));
        put16(h + ra + offsetof(ods2_recattr_t, fat_efblk) + 0,
              (uint16_t)(efblk >> 16));
        put16(h + ra + offsetof(ods2_recattr_t, fat_efblk) + 2,
              (uint16_t)(efblk & 0xFFFF));
        put16(h + ra + offsetof(ods2_recattr_t, fat_ffbyte), ffbyte);
        put32(h + offsetof(ods2_fh2_t, fh2_highwater), hiblk + 1);
    }
    put16(h + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(h));

    return wvol_commit(wvol, ODS2_OK);
}
