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
 * Like ods2_reader.c, this operates purely over an in-memory image buffer
 * (caller-owned) and does no I/O of its own.
 */

#include "vmsfs/ods2.h"

#include <stdio.h>
#include <string.h>

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

static uint8_t *wblk(ods2_wvolume_t *wvol, uint32_t lbn)
{
    return wvol->image + (size_t)lbn * ODS2_BLOCK_SIZE;
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
static void bitmap_set(ods2_wvolume_t *wvol, uint32_t base_lbn,
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
static void bitmap_init_all_set(ods2_wvolume_t *wvol, uint32_t base_lbn,
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
        bitmap_set(wvol, wvol->bitmap_scb_lbn + 1, lbn + i, 0);
}

/* Mark FID `fidnum` (1-based) in-use (bit -> 1) in the index file bitmap. */
static void ifile_bitmap_mark_used(ods2_wvolume_t *wvol, uint32_t fidnum)
{
    bitmap_set(wvol, wvol->ibmap_lbn, fidnum - 1, 1);
}

/* ================================================================
 * FM2 retrieval-pointer encoder (format 1: <= 256 blocks, <= 22-bit LBN).
 * Inverse of ods2_fh2_map_walk()'s format-1 decode in ods2_reader.c. [N]
 * ================================================================ */
static ods2_status_t encode_map_extent(uint8_t *mp, uint32_t lbn, uint32_t count)
{
    uint16_t w0, w1;
    uint32_t high6;

    if (count < 1 || count > 256 || lbn >= (1u << 22))
        return ODS2_ERR_ARGS;

    high6 = (lbn >> 16) & 0x3F;
    w0 = (uint16_t)(0x4000u | ((count - 1) & 0xFF) | (high6 << 8));
    w1 = (uint16_t)(lbn & 0xFFFF);

    put16(mp + 0, w0);
    put16(mp + 2, w1);
    return ODS2_OK;
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
 * both plain data files (HELLO.TXT/WORLD.TXT) show rtype=2/rattrib=0x02.
 * rsize/maxrec: system files use 512/512; data files use 0/0 (variable,
 * no max); directories use 512/512 to match 000000.DIR/OVMXDIR.DIR.
 */
enum fh2_kind { FH2_KIND_SYSTEM = 0, FH2_KIND_DIR, FH2_KIND_DATA };

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
static ods2_status_t write_fh2_header(ods2_wvolume_t *wvol, uint32_t fidnum,
                                      uint16_t seq, const char *name,
                                      uint16_t version, uint32_t filechar,
                                      enum fh2_kind kind,
                                      uint32_t map_lbn, uint32_t map_count,
                                      size_t data_len, ods2_fid_t backlink)
{
    uint32_t lbn = wvol->hdr_base_lbn + (fidnum - 1);
    uint8_t *h;
    char idbuf[21];
    size_t base_len, n;
    ods2_status_t st;
    uint8_t rtype, rattrib;
    uint16_t rsize, maxrec, ffbyte;
    uint32_t hiblk, efblk;

    if (fidnum < 1 || fidnum > wvol->maxfiles)
        return ODS2_ERR_ARGS;

    base_len = strlen(name);
    if (base_len == 0)
        return ODS2_ERR_ARGS;
    /* "NAME;VERSION", space-padded to 20 -- must fit. */
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
    default: /* FH2_KIND_SYSTEM */
        rtype = 1; rattrib = 0x00; rsize = 512; maxrec = 512;
        break;
    }
    if (map_count == 0) {
        /* Matches every zero-length reserved stub in the real fixture
         * (BADBLK/CORIMG/VOLSET/CONTIN/BACKUP/BADLOG): hiblk=0, efblk=1. */
        hiblk = 0;
        efblk = 1;
        ffbyte = 0;
    } else {
        /* Matches HELLO.TXT/WORLD.TXT/000000.DIR exactly (hiblk==efblk==
         * block count); INDEXF.SYS/BITMAP.SYS/SECURITY.SYS showed a +1 (or,
         * for SECURITY.SYS, a smaller) efblk in the real fixture -- this
         * writer does not reproduce that quirk (see ods2.h WRITER
         * simplifications), using the simpler hiblk==efblk convention for
         * every file it creates or stubs, [OVMX-inferred] for those three. */
        hiblk = map_count;
        efblk = map_count;
        if (kind == FH2_KIND_DATA && data_len > 0) {
            size_t last_block_bytes = data_len - (size_t)(map_count - 1) * ODS2_BLOCK_SIZE;
            ffbyte = (uint16_t)last_block_bytes; /* 1..512 */
        } else {
            ffbyte = 0;
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

    /* ident area: fi2_filename, space-padded, "NAME.TYPE;VERSION". */
    {
        uint8_t *id = h + (size_t)ID_OFF_WORDS * 2;
        memset(id, ' ', 20);
        memcpy(id, idbuf, n);
        put16(id + 20, version); /* fi2_revision */
        /* fi2_credate .. fi2_bakdate and fi2_filenamext left zero:
         * [OVMX-inferred] timestamps not modeled, see ods2.h. */
    }

    /* map area: one FM2 format-1 retrieval pointer, if this file has data. */
    if (map_count > 0) {
        uint8_t *mp = h + (size_t)MP_OFF_WORDS * 2;
        st = encode_map_extent(mp, map_lbn, map_count);
        if (st != ODS2_OK)
            return st;
        h[offsetof(ods2_fh2_t, fh2_map_inuse)] = 2;
    } else {
        h[offsetof(ods2_fh2_t, fh2_map_inuse)] = 0;
    }

    /* checksum over first 255 words -> offset 510 */
    put16(h + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(h));

    return ODS2_OK;
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

    /* INDEXF.SYS's own map is one contiguous extent starting at LBN 0 (the
     * BOOT BLOCK -- see ods2.h [F3]: a real VAX MOUNT rejected an earlier
     * draft whose extent started at LBN 1, and the real fixture's own
     * INDEXF.SYS map begins at LBN 0 too), so VBN == LBN + 1 within it. */
    put16(hb + offsetof(ods2_home_t, hm2_homevbn),   2);
    put16(hb + offsetof(ods2_home_t, hm2_alhomevbn), 3);
    put16(hb + offsetof(ods2_home_t, hm2_altidxvbn), (uint16_t)(altidx_lbn + 1));
    put16(hb + offsetof(ods2_home_t, hm2_ibmapvbn),  (uint16_t)(wvol->ibmap_lbn + 1));

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
 * ods2_volume_format()
 * ================================================================ */

ods2_status_t ods2_volume_format(uint8_t *image, size_t image_len,
                                 const ods2_format_params_t *params,
                                 ods2_wvolume_t *wvol)
{
    uint32_t total_blocks, maxfiles;
    uint32_t ibmap_size, hdr_base, altidx_lbn;
    uint32_t bitmap_scb_lbn, bitmap_bits_blocks, bitmap_total;
    uint32_t mfd_lbn, security_lbn, reserved_end;
    ods2_status_t st;
    ods2_uic_t system_uic;

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

    /* ---- fixed layout ----
     * LBN 0              : boot block (left zero)
     * LBN 1              : home block (primary)   [VBN1 of INDEXF.SYS]
     * LBN 2              : home block (alternate) [VBN2 of INDEXF.SYS]
     * LBN 3..3+ibsz-1    : index file bitmap       [VBN3.. of INDEXF.SYS]
     * LBN hdr_base..+mf-1: FID headers 1..maxfiles -- the FULL header area,
     *                      not just the 10 reserved ones: the reader (and
     *                      any real ODS-2 driver) computes a file's header
     *                      LBN as hdr_base + (fid_num - 1) for ANY valid
     *                      fid_num up to maxfiles, so nothing else may be
     *                      placed inside this range or user-file headers
     *                      (fid > ODS2_RESFILES) collide with it.
     * LBN altidx_lbn     : alternate index header (copy of FID1's header)
     * LBN bitmap_scb_lbn : BITMAP.SYS's SCB (its own VBN1)
     * LBN +1..+bb        : BITMAP.SYS's storage bitmap bits (its VBN2..)
     * LBN mfd_lbn        : MFD ([000000]) data block
     * LBN security_lbn..+5: SECURITY.SYS's 6-block CONTIG data extent
     *                      (only VBN1/security_lbn carries content)
     *
     * SECURITY.SYS (FID 10): the SPECIFIC increment-3 failure mode
     * ("%MOUNT-F-BADSECSYS -SYSTEM-E-BADCHECKSUM") -- real MOUNT enforcing
     * a genuine checksum over VBN1's content -- is RESOLVED in increment 4;
     * see ods2.h's WRITER [F6] provenance comment for the clean-room
     * (differential lab-2 observation, not disassembly or verbatim byte
     * copying) derivation of the checksum algorithm and data-block layout.
     * A full real-VAX MOUNT of this writer's output STILL does not reach
     * completion, now via a DIFFERENT failure ("-SYSTEM-W-FILENUMCHK")
     * bisected to be outside SECURITY.SYS entirely -- see [F9] in ods2.h
     * for the honest current status and the leading follow-up candidate.
     */
    ibmap_size = (maxfiles + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    if (ibmap_size < 1) ibmap_size = 1;
    hdr_base   = 3 + ibmap_size;
    altidx_lbn = hdr_base + maxfiles;   /* right after the FULL header area */

    bitmap_bits_blocks = (total_blocks + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
    if (bitmap_bits_blocks < 1) bitmap_bits_blocks = 1;
    bitmap_scb_lbn = altidx_lbn + 1;
    bitmap_total   = 1 + bitmap_bits_blocks; /* SCB + bit blocks */

    mfd_lbn      = bitmap_scb_lbn + bitmap_total;
    security_lbn = mfd_lbn + 1;
    reserved_end = security_lbn + ODS2_SECURITY_DATA_BLOCKS; /* first LBN
                                        available to callers */

    if (reserved_end >= total_blocks)
        return ODS2_ERR_SIZE;   /* volume too small for the fixed layout */

    wvol->ibmap_lbn          = 3;
    wvol->ibmap_size         = ibmap_size;
    wvol->hdr_base_lbn       = hdr_base;
    wvol->bitmap_scb_lbn     = bitmap_scb_lbn;
    wvol->bitmap_data_blocks = bitmap_bits_blocks;
    wvol->next_free_lbn      = reserved_end;
    wvol->next_free_fid      = ODS2_RESFILES + 1;

    /* ---- index file bitmap: FIDs 1..RESFILES in use, rest free ---- */
    bitmap_init_all_set(wvol, wvol->ibmap_lbn, wvol->ibmap_size);
    {
        uint32_t i;
        for (i = 1; i <= ODS2_RESFILES; i++)
            bitmap_set(wvol, wvol->ibmap_lbn, i - 1, 1); /* mark IN USE */
        for (i = ODS2_RESFILES + 1; i <= maxfiles; i++)
            bitmap_set(wvol, wvol->ibmap_lbn, i - 1, 0); /* mark free */
    }

    /* ---- storage bitmap: init all free, then mark the fixed layout used --- */
    bitmap_init_all_set(wvol, wvol->bitmap_scb_lbn + 1, wvol->bitmap_data_blocks);
    storage_bitmap_mark_used(wvol, 0, reserved_end); /* LBN 0..reserved_end-1 */
    /* also mark padding bits beyond total_blocks (inside the last bitmap
     * block) as allocated, so they never get handed out. */
    {
        uint32_t nbits = wvol->bitmap_data_blocks * BITS_PER_BLOCK;
        uint32_t i;
        for (i = total_blocks; i < nbits; i++)
            bitmap_set(wvol, wvol->bitmap_scb_lbn + 1, i, 0);
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

        st = write_fh2_header(wvol, ODS2_FID_INDEXF, ODS2_FID_INDEXF,
                              "INDEXF.SYS", 1, 0, FH2_KIND_SYSTEM,
                              0, altidx_lbn + 1, 0, mfd_bl); /* LBN 0..altidx_lbn */
        if (st != ODS2_OK) return st;
        st = write_fh2_header(wvol, ODS2_FID_BITMAP, ODS2_FID_BITMAP,
                              "BITMAP.SYS", 1, ODS2_FH2_M_CONTIG, FH2_KIND_SYSTEM,
                              wvol->bitmap_scb_lbn, bitmap_total, 0, mfd_bl);
        if (st != ODS2_OK) return st;
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

    wvol->mfd_fid.fid_num = ODS2_FID_MFD;
    wvol->mfd_fid.fid_seq = 1;
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

    return ODS2_OK;
}

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
    storage_bitmap_mark_used(wvol, lbn, count);
    wvol->next_free_lbn += count;

    *lbn_out = lbn;
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

ods2_status_t ods2_wvolume_create_file(ods2_wvolume_t *wvol,
                                       const char *name, uint16_t version,
                                       const uint8_t *data, size_t data_len,
                                       ods2_fid_t parent_dir,
                                       ods2_fid_t *fid_out)
{
    uint32_t fidnum, lbn, nblocks;
    ods2_status_t st;

    if (!wvol || !name || (!data && data_len > 0) || !fid_out)
        return ODS2_ERR_ARGS;

    nblocks = (uint32_t)((data_len + ODS2_BLOCK_SIZE - 1) / ODS2_BLOCK_SIZE);
    if (nblocks == 0)
        nblocks = 1;   /* even a zero-length file gets one data block */

    st = ods2_wvolume_alloc_blocks(wvol, nblocks, &lbn);
    if (st != ODS2_OK)
        return st;

    if (data_len > 0)
        memcpy(wblk(wvol, lbn), data, data_len);
    /* tail padding beyond data_len is already zero from volume-format's
     * initial memset (never reused, since the bump allocator never
     * revisits blocks). */

    st = alloc_fid(wvol, &fidnum);
    if (st != ODS2_OK)
        return st;

    /* seq == 1 (first generation) for a freshly created file; backlink ==
     * parent_dir. [F2] see ods2.h. */
    st = write_fh2_header(wvol, fidnum, 1, name, version, 0, FH2_KIND_DATA,
                          lbn, nblocks, data_len, parent_dir);
    if (st != ODS2_OK)
        return st;

    fid_from_num(fidnum, fid_out);
    return ODS2_OK;
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
    return ODS2_OK;
}

/* ================================================================
 * Directory-record insertion.
 * ================================================================ */

struct dir_single_extent_ctx {
    ods2_extent_t ext;
    unsigned      n;
};

static int dir_single_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    struct dir_single_extent_ctx *c = (struct dir_single_extent_ctx *)ctx;
    if (c->n == 0)
        c->ext = *ext;
    c->n++;
    return 0;
}

ods2_status_t ods2_wvolume_dir_insert(ods2_wvolume_t *wvol,
                                      ods2_fid_t dir_fid,
                                      const char *name, uint16_t version,
                                      ods2_fid_t entry_fid)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_volume_t view;
    ods2_status_t st;
    struct dir_single_extent_ctx ec = { { 0, 0 }, 0 };
    uint8_t *blk;
    unsigned off, namecount, name_off, val_off, rec_end, needed;

    if (!wvol || !name)
        return ODS2_ERR_ARGS;

    namecount = (unsigned)strlen(name);
    if (namecount == 0 || namecount > 255)
        return ODS2_ERR_ARGS;

    /* Re-open a read view of this same image (home block + all headers
     * written so far are already finalized/checksummed) to reuse the
     * increment-1 reader's header-read + map-walk logic rather than
     * duplicating it. */
    st = ods2_volume_open(&view, wvol->image, wvol->image_len);
    if (st != ODS2_OK)
        return st;
    st = ods2_volume_read_header(&view, ods2_fid_number(&dir_fid),
                                 hdr, sizeof(hdr));
    if (st != ODS2_OK)
        return st;

    st = ods2_fh2_map_walk(hdr, dir_single_extent_cb, &ec, NULL);
    if (st != ODS2_OK)
        return st;
    if (ec.n != 1)
        return ODS2_ERR_NOSPACE; /* directory growth beyond one extent unsupported */

    blk = wblk(wvol, ec.ext.lbn);

    /* Walk existing records the same way ods2_reader.c's dir_scan_block()
     * does, to find the first ODS2_DIR_END slot to insert at. */
    off = 0;
    for (;;) {
        uint16_t rec_size;
        if (off + 6 > ODS2_BLOCK_SIZE)
            return ODS2_ERR_NOSPACE;
        rec_size = rd16(blk + off);
        if (rec_size == ODS2_DIR_END)
            break;
        rec_end = off + 2 + rec_size;
        if (rec_end > ODS2_BLOCK_SIZE)
            return ODS2_ERR_NOSPACE; /* malformed existing block */
        off = rec_end;
    }

    name_off = off + 6;
    val_off  = name_off + namecount;
    if (val_off & 1)
        val_off++;                                 /* word-align */
    rec_end  = val_off + (unsigned)sizeof(ods2_dir_ent_t);
    needed   = rec_end - off;

    if (off + needed + 2 > ODS2_BLOCK_SIZE)
        return ODS2_ERR_NOSPACE; /* leave room for a trailing END marker */

    put16(blk + off + 0, (uint16_t)(rec_end - off - 2)); /* dir_size */
    put16(blk + off + 2, version);                        /* dir_verlimit */
    blk[off + 4] = 0;                                     /* dir_flags */
    blk[off + 5] = (uint8_t)namecount;                    /* dir_namecount */
    memcpy(blk + name_off, name, namecount);
    /* Any word-alignment pad byte between the name and val_off is already
     * 0xFF from the block's initial empty-directory fill; nothing to write. */

    put16(blk + val_off + 0, version);
    put16(blk + val_off + 2, entry_fid.fid_num);
    put16(blk + val_off + 4, entry_fid.fid_seq);
    blk[val_off + 6] = entry_fid.fid_rvn;
    blk[val_off + 7] = entry_fid.fid_nmx;

    /* Everything from rec_end onward was already 0xFF (either the initial
     * empty-directory fill, or a prior insert's untouched tail), so it
     * already reads back as ODS2_DIR_END -- no separate terminator write
     * needed. */

    return ODS2_OK;
}
