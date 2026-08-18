// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ods2_edit.c - PURE, block-I/O-free EDIT helpers for the GENUINE ODS-2 codec:
 * append an FM2 retrieval-pointer extent to a file header's map area, set a
 * header's end-of-file / highest-allocated position, reseal a header's
 * checksum, and test/allocate a bit in a storage-bitmap (BITMAP.SYS) block
 * (vms-c60, epic vms-208).
 *
 * WHY A SEPARATE PURE TU (the ods2_reader.c precedent, rd vms-dcd). The
 * Files-11 ODS-2 ACP's IO$_WRITEVBLK / implicit-extend path (src/kernel-core/
 * vmsfs_acp.c) must, in the EXECUTIVE, (a) allocate blocks from the volume's
 * BITMAP.SYS storage bitmap and (b) grow an accessed file's FH2 -- append a
 * retrieval pointer, bump its EOF/HIBLK, reseal its checksum. Those are on-disk
 * ODS-2 FORMAT facts, so they must come from the codec (CLAUDE.md Rule 8), NOT
 * be hand-duplicated in the kernel module. But the ACP reads and writes a RAW
 * block device through exec_blockdev_read_block / _write_block (there is no
 * mounted super_block behind an ACP volume -- see vmsfs_acp.c's top-of-file
 * note), so it CANNOT drive the full ods2_wvolume writer, whose kernel block
 * backend (ods2_block_kern.c) binds to vmsfs_bget()/super_block and whose
 * ~2 MB write cache is per-open. The clean fit -- exactly the shape #633's
 * IO$_ACCESS already uses -- is: the codec provides PURE parsers/encoders that
 * operate on caller-supplied 512-byte block buffers, and vms_acp.c SEQUENCES
 * the raw block reads/writes around them. ods2_reader.c is the pure PARSE
 * surface; this file is the pure EDIT surface (the inverse operations), with no
 * allocation, no I/O, no statics -- the same "drop-in kernel-resident" contract.
 *
 * Rule 8 provenance: every byte layout below is the SAME on-disk ODS-2 fact
 * already carried, with its citations, in vmsfs/ods2.h and reproduced by the
 * writer (ods2_writer.c): the FM2 format-1 retrieval pointer ([N], inverse of
 * ods2_fh2_map_walk's decode and identical to write_fh2_header_ext's
 * encode_map_extent), the RECATTR fat_hiblk/fat_efblk/fat_ffbyte hi/lo words
 * ([F16], see ods2_recattr_t), fh2_highwater, the 255-word additive FH2
 * checksum ([N]), and the storage-bitmap 32-bit-word packing with a SET bit ==
 * FREE ([N2], see ods2_writer.c's ods2_bitmap_set). No new format fact is
 * introduced; these are the write-side twins of parse operations the reader
 * already performs.
 */

#include "vmsfs/ods2.h"
#include "ods2_kcompat.h"   /* memset/memcpy/memmove/memcmp/strlen/snprintf (dual-world) */

/* ---- little-endian scalar access (endian-independent), as ods2_writer.c ---- */

static inline void ed_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline uint16_t ed_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void ed_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t ed_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * ods2_fh2_map_append - append ONE format-1 FM2 retrieval pointer covering
 * [lbn, lbn+count) to the END of the file header's map area, bumping
 * fh2_map_inuse by 2 words. The map area begins at (fh2_mpoffset * 2) bytes and
 * the currently-used words are fh2_map_inuse; the new 2-word entry is written at
 * (mpoffset*2 + map_inuse*2). The whole 255-word header is checksummed, so the
 * entry must land before the checksum word at byte 510 -- if it would not fit,
 * ODS2_ERR_NOSPACE (the file's extent map is full; the ACP surfaces this as an
 * honest device-full rather than a silent partial map). count in 1..256 and
 * lbn < 2^22 are the format-1 range (larger runs need FM2 format 2/3, which this
 * rung's small-volume corpus never needs -- ODS2_ERR_ARGS, fail-honest).
 *
 * `header_block` is the file's already-parsed FH2 primary header (>= 512 bytes);
 * the caller reseals the checksum with ods2_fh2_reseal() after all edits.
 */
ods2_status_t ods2_fh2_map_append(void *header_block, uint32_t lbn, uint32_t count)
{
    uint8_t *h = (uint8_t *)header_block;
    unsigned mpoffset, map_inuse, entry_byte;
    uint16_t w0, w1;
    uint32_t high6;

    if (!h)
        return ODS2_ERR_ARGS;
    if (count < 1 || count > 256 || lbn >= (1u << 22))
        return ODS2_ERR_ARGS;

    mpoffset  = h[offsetof(ods2_fh2_t, fh2_mpoffset)];
    map_inuse = h[offsetof(ods2_fh2_t, fh2_map_inuse)];

    entry_byte = mpoffset * 2u + map_inuse * 2u;
    /* Need 4 bytes (2 words) and must stay clear of the checksum word (510). */
    if (entry_byte + 4u > offsetof(ods2_fh2_t, fh2_checksum))
        return ODS2_ERR_NOSPACE;

    /* FM2 format 1: word0 = 0x4000 | (count-1) | (high-6 LBN bits << 8),
     * word1 = low 16 LBN bits. Inverse of ods2_fh2_map_walk()'s format-1
     * decode; identical to ods2_writer.c's encode_map_extent(). */
    high6 = (lbn >> 16) & 0x3F;
    w0 = (uint16_t)(0x4000u | ((count - 1u) & 0xFF) | (high6 << 8));
    w1 = (uint16_t)(lbn & 0xFFFF);
    ed_put16(h + entry_byte + 0, w0);
    ed_put16(h + entry_byte + 2, w1);

    h[offsetof(ods2_fh2_t, fh2_map_inuse)] = (uint8_t)(map_inuse + 2u);
    return ODS2_OK;
}

/*
 * ods2_fh2_set_eof - set the file header's RECATTR (FAT) size fields to a new
 * end-of-file/allocation position: fat_hiblk = `hiblk` (highest allocated VBN),
 * fat_efblk = `efblk` (end-of-file VBN), fat_ffbyte = `ffbyte` (first free byte
 * in the EOF block). Also updates fh2_highwater to hiblk+1 (the VBN of the first
 * never-written block), matching the writer's own invariant (see
 * write_fh2_header_ext()'s [F11] highwater note). The hi/lo word packing is the
 * [F16] convention: word[1] LOW, word[0] HIGH (ods2_recattr_hiblk/efblk decode
 * it the same way). Caller reseals with ods2_fh2_reseal().
 */
ods2_status_t ods2_fh2_set_eof(void *header_block, uint32_t hiblk,
                               uint32_t efblk, uint16_t ffbyte)
{
    uint8_t *h = (uint8_t *)header_block;
    uint8_t *fat;

    if (!h)
        return ODS2_ERR_ARGS;

    fat = h + offsetof(ods2_fh2_t, fh2_recattr);
    /* fat_hiblk[2] at +4, fat_efblk[2] at +8, fat_ffbyte at +12 (ods2_recattr_t) */
    ed_put16(fat + offsetof(ods2_recattr_t, fat_hiblk) + 2, (uint16_t)(hiblk & 0xFFFF));      /* [1] low  */
    ed_put16(fat + offsetof(ods2_recattr_t, fat_hiblk) + 0, (uint16_t)((hiblk >> 16) & 0xFFFF)); /* [0] high */
    ed_put16(fat + offsetof(ods2_recattr_t, fat_efblk) + 2, (uint16_t)(efblk & 0xFFFF));
    ed_put16(fat + offsetof(ods2_recattr_t, fat_efblk) + 0, (uint16_t)((efblk >> 16) & 0xFFFF));
    ed_put16(fat + offsetof(ods2_recattr_t, fat_ffbyte), ffbyte);

    ed_put32(h + offsetof(ods2_fh2_t, fh2_highwater), hiblk + 1u);
    return ODS2_OK;
}

/*
 * ods2_fh2_reseal - recompute and store the FH2's 16-bit additive checksum
 * (first 255 words, [N]) into fh2_checksum (byte 510), after any edit above.
 * ods2_checksum()/ods2_block_checksum() are the same pure summation the reader's
 * ods2_fh2_parse() validates against, so a resealed header re-parses clean.
 */
void ods2_fh2_reseal(void *header_block)
{
    uint8_t *h = (uint8_t *)header_block;
    if (!h)
        return;
    ed_put16(h + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(h));
}

/* ================================================================
 * Storage bitmap (BITMAP.SYS VBN2..) bit accounting. [N2] (ods2.h /
 * ods2_writer.c): 32-bit little-endian words, 4096 bits per 512-byte block, a
 * SET bit == a FREE block, a CLEAR bit == an ALLOCATED block (cluster factor 1,
 * so bit N of the whole bitmap == LBN N). These helpers act on ONE 512-byte
 * bitmap DATA block; the ACP chooses which block (bit / 4096) and which bit
 * within it (bit % 4096), reading/writing the block through exec_blockdev_*.
 * ================================================================ */

#define ODS2_SBM_BITS_PER_WORD   32u
/* ODS2_SBM_BITS_PER_BLOCK (== 4096) is the public constant from vmsfs/ods2.h. */

/* 1 if the bit `bit_in_block` (0..4095) is FREE (set) in this bitmap block. */
int ods2_sbm_block_bit_free(const void *bitmap_block, unsigned bit_in_block)
{
    const uint8_t *b = (const uint8_t *)bitmap_block;
    unsigned word_idx = (bit_in_block % ODS2_SBM_BITS_PER_BLOCK) / ODS2_SBM_BITS_PER_WORD;
    unsigned bit_idx  = bit_in_block % ODS2_SBM_BITS_PER_WORD;
    uint32_t w = ed_rd32(b + (size_t)word_idx * 4);
    return (w >> bit_idx) & 1u ? 1 : 0;
}

/* Mark the bit ALLOCATED (clear -> 0) in this bitmap block. */
void ods2_sbm_block_alloc(void *bitmap_block, unsigned bit_in_block)
{
    uint8_t *b = (uint8_t *)bitmap_block;
    unsigned word_idx = (bit_in_block % ODS2_SBM_BITS_PER_BLOCK) / ODS2_SBM_BITS_PER_WORD;
    unsigned bit_idx  = bit_in_block % ODS2_SBM_BITS_PER_WORD;
    uint8_t *wp = b + (size_t)word_idx * 4;
    uint32_t w = ed_rd32(wp);
    w &= ~(1u << bit_idx);
    ed_put32(wp, w);
}

/* Mark the bit FREE (set -> 1) -- the inverse, for rollback on a failed
 * multi-block extend so a partially-applied allocation never leaks blocks. */
void ods2_sbm_block_free(void *bitmap_block, unsigned bit_in_block)
{
    uint8_t *b = (uint8_t *)bitmap_block;
    unsigned word_idx = (bit_in_block % ODS2_SBM_BITS_PER_BLOCK) / ODS2_SBM_BITS_PER_WORD;
    unsigned bit_idx  = bit_in_block % ODS2_SBM_BITS_PER_WORD;
    uint8_t *wp = b + (size_t)word_idx * 4;
    uint32_t w = ed_rd32(wp);
    w |= (1u << bit_idx);
    ed_put32(wp, w);
}

/* ================================================================
 * INDEX-FILE bitmap (INDEXF.SYS's index bitmap, at hm2_ibmaplbn..) bit
 * accounting -- the FILE-HEADER allocation counterpart of the storage bitmap
 * above (vms-5303, epic vms-208). SAME 32-bit-little-endian, 4096-bit-per-block
 * packing [N2], but the OPPOSITE bit SENSE: a SET bit == a FID IN USE, a CLEAR
 * bit == a FREE FID (this is the sense ods2_writer.c's ifile_bitmap_mark_used()
 * writes: `ods2_bitmap_set(..., fidnum - 1, value=1)` to mark FID in use, and
 * format_common()'s ods2_bitmap_set(..., i - 1, 1) marks reserved FIDs IN USE). Bit N
 * of the whole index bitmap corresponds to file NUMBER N+1 (FID numbers are
 * 1-based). These helpers act on ONE 512-byte index-bitmap DATA block; the ACP
 * (src/kernel-core/vmsfs_acp.c) chooses which block ((fidnum-1)/4096) and which
 * bit within it ((fidnum-1)%4096), reading/writing the block through
 * exec_blockdev_*. Write-side twins of the reader's implicit "bit set == used"
 * read, no new format fact.
 * ================================================================ */

/* 1 if file number `bit_in_block+base` is IN USE (bit set) in this bitmap block. */
int ods2_ifbm_block_fid_used(const void *bitmap_block, unsigned bit_in_block)
{
    const uint8_t *b = (const uint8_t *)bitmap_block;
    unsigned word_idx = (bit_in_block % ODS2_SBM_BITS_PER_BLOCK) / ODS2_SBM_BITS_PER_WORD;
    unsigned bit_idx  = bit_in_block % ODS2_SBM_BITS_PER_WORD;
    uint32_t w = ed_rd32(b + (size_t)word_idx * 4);
    return (w >> bit_idx) & 1u ? 1 : 0;
}

/* Mark the FID IN USE (set -> 1) in this index-bitmap block. */
void ods2_ifbm_block_alloc(void *bitmap_block, unsigned bit_in_block)
{
    uint8_t *b = (uint8_t *)bitmap_block;
    unsigned word_idx = (bit_in_block % ODS2_SBM_BITS_PER_BLOCK) / ODS2_SBM_BITS_PER_WORD;
    unsigned bit_idx  = bit_in_block % ODS2_SBM_BITS_PER_WORD;
    uint8_t *wp = b + (size_t)word_idx * 4;
    uint32_t w = ed_rd32(wp);
    w |= (1u << bit_idx);
    ed_put32(wp, w);
}

/* Mark the FID FREE (clear -> 0) in this index-bitmap block -- IO$_DELETE's
 * header deallocation, the inverse of _alloc. */
void ods2_ifbm_block_free(void *bitmap_block, unsigned bit_in_block)
{
    uint8_t *b = (uint8_t *)bitmap_block;
    unsigned word_idx = (bit_in_block % ODS2_SBM_BITS_PER_BLOCK) / ODS2_SBM_BITS_PER_WORD;
    unsigned bit_idx  = bit_in_block % ODS2_SBM_BITS_PER_WORD;
    uint8_t *wp = b + (size_t)word_idx * 4;
    uint32_t w = ed_rd32(wp);
    w &= ~(1u << bit_idx);
    ed_put32(wp, w);
}

/* ================================================================
 * FH2 file-header CONSTRUCTION (vms-5303). PURE write-side twin of
 * ods2_writer.c's write_fh2_header_ext() -- the SAME validated on-disk FH2
 * format (Rule 8), transcribed to operate on a caller-supplied 512-byte block
 * with no wvolume, no wblk(), no I/O, no allocation, so the executive ACP
 * sequences the raw exec_blockdev_write_block around it exactly as it does for
 * the map/EOF edits above. Every field, offset, preset and the acoffset==255
 * / reserved1 / fileowner / highwater provenance is write_fh2_header_ext()'s;
 * see that function (and ods2.h's WRITER provenance [F2]/[F4]/[F11]/[F15]/[F16])
 * for the per-field real-VAX-MOUNT bisection trail. The only shape difference
 * is that owner UIC and protection are PARAMETERS here (the ACP supplies the
 * creating process's UIC per INV-6, or the ATR-list value) rather than the
 * writer's uniform SYSTEM [1,4]; pass owner={0,0}+fileprot=0 to take the
 * kind default (SYSTEM owner, 0xFA00/0xBA00 prot) the writer uses.
 * ================================================================ */

#define ED_ID_OFF_WORDS 54     /* ident area at byte 108 (write_fh2_header_ext) */
#define ED_MP_OFF_WORDS 114    /* map area at byte 228 */

ods2_status_t ods2_fh2_build(void *header_block, uint32_t fidnum, uint16_t seq,
                             const char *name, uint16_t version, uint32_t filechar,
                             unsigned kind, const ods2_extent_t *extents,
                             unsigned n_extents, size_t data_len,
                             ods2_fid_t backlink, ods2_uic_t owner,
                             uint16_t fileprot, uint32_t maxfiles)
{
    uint8_t *h = (uint8_t *)header_block;
    char idbuf[20 + 66 + 1];
    size_t base_len, n;
    ods2_status_t st;
    uint8_t rtype, rattrib;
    uint16_t rsize, maxrec, ffbyte;
    uint32_t hiblk, efblk, total_count;
    unsigned ei;
    ods2_uic_t eff_owner = owner;
    uint16_t eff_prot = fileprot;

    if (!h || !name)
        return ODS2_ERR_ARGS;
    if (fidnum < 1 || (maxfiles && fidnum > maxfiles))
        return ODS2_ERR_ARGS;
    if (n_extents > 0 && !extents)
        return ODS2_ERR_ARGS;

    total_count = 0;
    for (ei = 0; ei < n_extents; ei++)
        total_count += extents[ei].count;

    base_len = strlen(name);
    if (base_len == 0)
        return ODS2_ERR_ARGS;
    n = (size_t)snprintf(idbuf, sizeof(idbuf), "%s;%u", name, (unsigned)version);
    if (n >= sizeof(idbuf))
        return ODS2_ERR_ARGS;

    memset(h, 0, ODS2_BLOCK_SIZE);

    h[offsetof(ods2_fh2_t, fh2_idoffset)] = ED_ID_OFF_WORDS;
    h[offsetof(ods2_fh2_t, fh2_mpoffset)] = ED_MP_OFF_WORDS;
    h[offsetof(ods2_fh2_t, fh2_acoffset)] = 255;    /* [F4] no-ACL sentinel */
    h[offsetof(ods2_fh2_t, fh2_rsoffset)] = 255;

    ed_put16(h + offsetof(ods2_fh2_t, fh2_struclev), ODS2_STRUCLEV_V2);

    ed_put16(h + offsetof(ods2_fh2_t, fh2_fid) + 0, (uint16_t)(fidnum & 0xFFFF));
    ed_put16(h + offsetof(ods2_fh2_t, fh2_fid) + 2, seq);
    h[offsetof(ods2_fh2_t, fh2_fid) + 4] = 0;                      /* rvn */
    h[offsetof(ods2_fh2_t, fh2_fid) + 5] = (uint8_t)(fidnum >> 16);/* nmx */

    ed_put16(h + offsetof(ods2_fh2_t, fh2_backlink) + 0, backlink.fid_num);
    ed_put16(h + offsetof(ods2_fh2_t, fh2_backlink) + 2, backlink.fid_seq);
    h[offsetof(ods2_fh2_t, fh2_backlink) + 4] = backlink.fid_rvn;
    h[offsetof(ods2_fh2_t, fh2_backlink) + 5] = backlink.fid_nmx;

    ed_put32(h + offsetof(ods2_fh2_t, fh2_filechar), filechar);

    /* ---- RECATTR (FAT) preset by kind (write_fh2_header_ext's switch) ---- */
    switch (kind) {
    case ODS2_FK_DIR:
        rtype = 2; rattrib = 0x08; rsize = 512; maxrec = 512;
        break;
    case ODS2_FK_DATA:
        rtype = ODS2_RTYPE_VAR; rattrib = ODS2_RAT_CR; rsize = 0; maxrec = 0;
        break;
    case ODS2_FK_DATA_FIX:
        rtype = ODS2_RTYPE_FIX; rattrib = 0x00; rsize = 512; maxrec = 512;
        break;
    default: /* ODS2_FK_SYSTEM */
        rtype = 1; rattrib = 0x00; rsize = 512; maxrec = 512;
        break;
    }
    if (total_count == 0) {
        hiblk = 0; efblk = 1; ffbyte = 0;
    } else {
        hiblk = total_count;
        if (kind == ODS2_FK_DIR) {
            efblk = total_count + 1;
            ffbyte = 0;
        } else {
            efblk = total_count;
            if ((kind == ODS2_FK_DATA || kind == ODS2_FK_DATA_FIX) && data_len > 0) {
                size_t last = data_len - (size_t)(total_count - 1) * ODS2_BLOCK_SIZE;
                ffbyte = (uint16_t)last;
            } else {
                ffbyte = 0;
            }
        }
    }
    h[offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_rtype)]   = rtype;
    h[offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_rattrib)] = rattrib;
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_rsize), rsize);
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_hiblk) + 0,
             (uint16_t)(hiblk >> 16));
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_hiblk) + 2,
             (uint16_t)(hiblk & 0xFFFF));
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_efblk) + 0,
             (uint16_t)(efblk >> 16));
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_efblk) + 2,
             (uint16_t)(efblk & 0xFFFF));
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_ffbyte), ffbyte);
    ed_put16(h + offsetof(ods2_fh2_t, fh2_recattr) + offsetof(ods2_recattr_t, fat_maxrec), maxrec);

    /* [F11] owner / prot / reserved1 / highwater. Owner+prot from the caller
     * (INV-6: the creating process's UIC), falling to the writer's SYSTEM/kind
     * default when the caller passes zero. */
    if (eff_owner.uic_group == 0 && eff_owner.uic_member == 0) {
        eff_owner.uic_member = 4; eff_owner.uic_group = 1;         /* SYSTEM [1,4] */
    }
    if (eff_prot == 0)
        eff_prot = (kind == ODS2_FK_DIR) ? 0xBA00u : 0xFA00u;
    ed_put16(h + offsetof(ods2_fh2_t, fh2_reserved1),
             (fidnum <= ODS2_RESFILES) ? 0xFE00u : 0u);
    ed_put16(h + offsetof(ods2_fh2_t, fh2_fileowner) + 0, eff_owner.uic_member);
    ed_put16(h + offsetof(ods2_fh2_t, fh2_fileowner) + 2, eff_owner.uic_group);
    ed_put16(h + offsetof(ods2_fh2_t, fh2_fileprot), eff_prot);
    ed_put32(h + offsetof(ods2_fh2_t, fh2_highwater), hiblk + 1u);

    /* ident area: "NAME.TYPE;VERSION", space-padded (write_fh2_header_ext). */
    {
        uint8_t *id = h + (size_t)ED_ID_OFF_WORDS * 2;
        size_t first_len = (n < 20) ? n : 20;
        size_t ext_len = (n > 20) ? (n - 20) : 0;

        memset(id, ' ', 20);
        memcpy(id, idbuf, first_len);
        ed_put16(id + 20, version);                               /* fi2_revision */
        if (ext_len > 0) {
            uint8_t *ext = id + offsetof(ods2_ident_t, fi2_filenamext);
            memset(ext, ' ', sizeof(((ods2_ident_t *)0)->fi2_filenamext));
            memcpy(ext, idbuf + 20, ext_len);
        }
    }

    /* map area: one FM2 format-1 retrieval pointer per extent. */
    for (ei = 0; ei < n_extents; ei++) {
        st = ods2_fh2_map_append(h, extents[ei].lbn, extents[ei].count);
        if (st != ODS2_OK)
            return st;
    }

    ed_put16(h + offsetof(ods2_fh2_t, fh2_checksum), ods2_block_checksum(h));
    return ODS2_OK;
}

/*
 * ods2_fh2_rename - rewrite an EXISTING file header's identification area (file
 * name + version), and -- when `new_backlink` is non-NULL -- its directory
 * back-link FID, for an IO$_MODIFY!IO$M_MOVE (rename/move) of the file
 * (vms-de7, epic vms-208). PURE: edits the caller's 512-byte header block in
 * place, touching ONLY the ident name / revision word / filename-extension and
 * (optionally) fh2_backlink -- every other FH2 field (FID, RECATTR/EOF, map,
 * owner/prot, create/revise dates) is left byte-for-byte unchanged, so the file
 * KEEPS its identity and allocation and only its NAME (and parent) moves. The
 * caller reseals with ods2_fh2_reseal() and writes the block back, exactly as
 * for ods2_fh2_map_append()/ods2_fh2_set_eof().
 *
 * Every byte written is ods2_fh2_build()'s own ident-area / backlink layout
 * (see its [F2]/[F11] provenance and lines 397-411): fi2_filename[20] holds the
 * space-padded "NAME.TYPE;VERSION" head, fi2_revision the version word, and
 * fi2_filenamext[66] the >20-char overflow. fh2_idoffset is read from the header
 * itself (not the build-time constant) so a header laid down with a different
 * ident offset still renames correctly. ODS2_ERR_ARGS on a bad block/name,
 * ODS2_ERR_FORMAT if the header's ident offset does not fit a 512-byte block.
 */
ods2_status_t ods2_fh2_rename(void *header_block, const char *name,
                              uint16_t version, const ods2_fid_t *new_backlink)
{
    uint8_t *h = (uint8_t *)header_block;
    unsigned idoff_words;
    uint8_t *id;
    char idbuf[20 + 66 + 1];
    size_t n, first_len, ext_len;

    if (!h || !name || name[0] == '\0')
        return ODS2_ERR_ARGS;

    n = (size_t)snprintf(idbuf, sizeof(idbuf), "%s;%u", name, (unsigned)version);
    if (n >= sizeof(idbuf))
        return ODS2_ERR_ARGS;

    idoff_words = h[offsetof(ods2_fh2_t, fh2_idoffset)];
    if (idoff_words == 0 ||
        (size_t)idoff_words * 2u + sizeof(ods2_ident_t) > ODS2_BLOCK_SIZE)
        return ODS2_ERR_FORMAT;
    id = h + (size_t)idoff_words * 2u;

    /* ident name: "NAME.TYPE;VERSION", space-padded (ods2_fh2_build lines
     * 397-411). Touch only fi2_filename[20] / fi2_revision / fi2_filenamext[66];
     * fi2_credate / fi2_revdate etc. (bytes 22..53) are left as they are. */
    first_len = (n < 20) ? n : 20;
    ext_len   = (n > 20) ? (n - 20) : 0;
    memset(id, ' ', 20);
    memcpy(id, idbuf, first_len);
    ed_put16(id + offsetof(ods2_ident_t, fi2_revision), version);
    {
        uint8_t *ext = id + offsetof(ods2_ident_t, fi2_filenamext);
        memset(ext, ' ', sizeof(((ods2_ident_t *)0)->fi2_filenamext));
        if (ext_len > 0)
            memcpy(ext, idbuf + 20, ext_len);
    }

    /* fh2_backlink: the parent-directory FID ([F2]). Rewritten only on a MOVE
     * across directories; a same-directory rename passes NULL and keeps it. */
    if (new_backlink) {
        ed_put16(h + offsetof(ods2_fh2_t, fh2_backlink) + 0, new_backlink->fid_num);
        ed_put16(h + offsetof(ods2_fh2_t, fh2_backlink) + 2, new_backlink->fid_seq);
        h[offsetof(ods2_fh2_t, fh2_backlink) + 4] = new_backlink->fid_rvn;
        h[offsetof(ods2_fh2_t, fh2_backlink) + 5] = new_backlink->fid_nmx;
    }
    return ODS2_OK;
}

/* ================================================================
 * DIRECTORY-RECORD build + rebuild (vms-5303). PURE write-side twins of
 * ods2_writer.c's ods2_wvolume_dir_insert() / merge_dir_record(): the SAME
 * validated on-disk directory format ([F13] name-sorted, [F14] verlimit,
 * [F17] one-record-never-crosses-a-block, value entries descending by
 * version), transcribed to operate on caller-supplied CONTIGUOUS block buffers
 * (in_blocks = the directory's current data blocks, out_blocks = the repacked
 * result) with a caller-supplied `flat` scratch buffer -- no wvolume, no
 * wblk(), no I/O, no block allocation. The ACP reads the directory's data
 * blocks (via its window), calls these to compute the new block bytes, then
 * allocates any growth (acp_bitmap_alloc) and writes the blocks + the header
 * map back. Every byte layout is ods2_wvolume_dir_insert()'s; see that
 * function's [F13]/[F17]/[vms-9794] provenance.
 * ================================================================ */

/* [F13] byte-wise ascending name order (ods2_writer.c dir_name_cmp twin). */
static int ed_dir_name_cmp(const char *a, unsigned alen, const char *b, unsigned blen)
{
    unsigned m = alen < blen ? alen : blen;
    int c = m ? memcmp(a, b, m) : 0;
    if (c != 0)
        return c;
    if (alen != blen)
        return alen < blen ? -1 : 1;
    return 0;
}

/* Merge a new {version, entry_fid} into an existing SAME-name record's
 * descending value-entry array -- ods2_writer.c merge_dir_record() twin.
 * `src` points at the record's dir_size word; `src_len` == 2 + dir_size. */
static ods2_status_t ed_merge_dir_record(const uint8_t *src, unsigned src_len,
                                         unsigned namecount, uint16_t version,
                                         ods2_fid_t entry_fid,
                                         uint8_t *out, unsigned *out_len)
{
    unsigned val_off = 6u + namecount;
    unsigned n_old, i, new_idx, new_len;

    if (val_off & 1)
        val_off++;
    if (val_off > src_len || ((src_len - val_off) % 8u) != 0)
        return ODS2_ERR_FORMAT;
    n_old = (src_len - val_off) / 8u;

    new_idx = n_old;
    for (i = 0; i < n_old; i++) {
        uint16_t v = ed_rd16(src + val_off + i * 8u);
        if (v == version)
            return ODS2_ERR_ARGS;               /* duplicate version */
        if (v < version) { new_idx = i; break; }
    }

    new_len = val_off + (n_old + 1u) * 8u;
    if (new_len > ODS2_BLOCK_SIZE - 2u)
        return ODS2_ERR_NOSPACE;

    memset(out, 0xFF, new_len);
    ed_put16(out + 0, (uint16_t)(new_len - 2));
    memcpy(out + 2, src + 2, 4);                 /* verlimit + flags + namecount */
    memcpy(out + 6, src + 6, namecount);
    for (i = 0; i < new_idx; i++)
        memcpy(out + val_off + i * 8u, src + val_off + i * 8u, 8u);
    ed_put16(out + val_off + new_idx * 8u + 0, version);
    ed_put16(out + val_off + new_idx * 8u + 2, entry_fid.fid_num);
    ed_put16(out + val_off + new_idx * 8u + 4, entry_fid.fid_seq);
    out[val_off + new_idx * 8u + 6] = entry_fid.fid_rvn;
    out[val_off + new_idx * 8u + 7] = entry_fid.fid_nmx;
    for (i = new_idx; i < n_old; i++)
        memcpy(out + val_off + (i + 1u) * 8u, src + val_off + i * 8u, 8u);

    *out_len = new_len;
    return ODS2_OK;
}

/* Greedy-pack a flat sorted record stream into 512-byte blocks (each 0xFF-
 * filled so trailing space reads back as ODS2_DIR_END), reserving 2 trailing
 * bytes per block for the terminator. [F17] rule 2. Returns block count via
 * *nblk_out; ODS2_ERR_NOSPACE if it exceeds out_nblk_cap. */
static ods2_status_t ed_dir_pack(const uint8_t *flat, size_t flat_used,
                                 uint8_t *out_blocks, unsigned out_nblk_cap,
                                 unsigned *nblk_out)
{
    unsigned nblk = 1, cur = 0, bi;
    size_t foff;

    for (foff = 0; foff < flat_used; ) {
        unsigned reclen = 2u + ed_rd16(flat + foff);
        if (cur + reclen + 2u > ODS2_BLOCK_SIZE) { nblk++; cur = 0; }
        cur += reclen;
        foff += reclen;
    }
    if (nblk > out_nblk_cap)
        return ODS2_ERR_NOSPACE;

    for (bi = 0; bi < nblk; bi++)
        memset(out_blocks + (size_t)bi * ODS2_BLOCK_SIZE, 0xFF, ODS2_BLOCK_SIZE);
    bi = 0;
    cur = 0;
    for (foff = 0; foff < flat_used; ) {
        unsigned reclen = 2u + ed_rd16(flat + foff);
        if (cur + reclen + 2u > ODS2_BLOCK_SIZE) { bi++; cur = 0; }
        memcpy(out_blocks + (size_t)bi * ODS2_BLOCK_SIZE + cur, flat + foff, reclen);
        cur += reclen;
        foff += reclen;
    }
    *nblk_out = nblk;
    return ODS2_OK;
}

ods2_status_t ods2_dir_insert_blocks(const uint8_t *in_blocks, unsigned in_nblk,
                                     const char *name, unsigned namecount,
                                     uint16_t version, ods2_fid_t entry_fid,
                                     int is_resfile,
                                     uint8_t *flat, size_t flat_cap,
                                     uint8_t *out_blocks, unsigned out_nblk_cap,
                                     unsigned *out_nblk)
{
    uint8_t newrec[ODS2_BLOCK_SIZE];
    unsigned new_valoff, newrec_len, b;
    size_t flat_used = 0, insert_off = 0, need;
    int have_insert = 0, found_name = 0;
    ods2_status_t st;

    if (!in_blocks || !name || !flat || !out_blocks || !out_nblk)
        return ODS2_ERR_ARGS;
    if (namecount == 0 || namecount > 255)
        return ODS2_ERR_ARGS;

    need = (size_t)in_nblk * ODS2_BLOCK_SIZE + ODS2_BLOCK_SIZE + 16;
    if (flat_cap < need)
        return ODS2_ERR_NOSPACE;

    /* Build the new record (ods2_wvolume_dir_insert lines 2100-2116). */
    new_valoff = 6u + namecount;
    if (new_valoff & 1)
        new_valoff++;
    newrec_len = new_valoff + 8u;               /* one value entry */
    memset(newrec, 0xFF, sizeof(newrec));
    ed_put16(newrec + 0, (uint16_t)(newrec_len - 2));
    ed_put16(newrec + 2, is_resfile ? version : ODS2_DIR_VERLIMIT_DEFAULT);
    newrec[4] = 0;                               /* dir_flags */
    newrec[5] = (uint8_t)namecount;
    memcpy(newrec + 6, name, namecount);
    ed_put16(newrec + new_valoff + 0, version);
    ed_put16(newrec + new_valoff + 2, entry_fid.fid_num);
    ed_put16(newrec + new_valoff + 4, entry_fid.fid_seq);
    newrec[new_valoff + 6] = entry_fid.fid_rvn;
    newrec[new_valoff + 7] = entry_fid.fid_nmx;

    /* Flatten existing records; locate the sorted insertion point / merge. */
    for (b = 0; b < in_nblk; b++) {
        const uint8_t *blk = in_blocks + (size_t)b * ODS2_BLOCK_SIZE;
        unsigned off = 0;
        for (;;) {
            uint16_t rec_size;
            unsigned reclen, nc;
            if (off + 6 > ODS2_BLOCK_SIZE)
                break;
            rec_size = ed_rd16(blk + off);
            if (rec_size == ODS2_DIR_END)
                break;
            reclen = 2u + rec_size;
            if (off + reclen > ODS2_BLOCK_SIZE)
                return ODS2_ERR_FORMAT;
            nc = blk[off + 5];
            if (6u + nc > reclen)
                return ODS2_ERR_FORMAT;
            if (!found_name && !have_insert) {
                int cmp = ed_dir_name_cmp(name, namecount,
                                          (const char *)blk + off + 6, nc);
                if (cmp == 0) {
                    unsigned merged_len;
                    st = ed_merge_dir_record(blk + off, reclen, nc, version,
                                             entry_fid, flat + flat_used, &merged_len);
                    if (st != ODS2_OK)
                        return st;
                    flat_used += merged_len;
                    found_name = 1;
                    off += reclen;
                    continue;
                }
                if (cmp < 0) { insert_off = flat_used; have_insert = 1; }
            }
            memcpy(flat + flat_used, blk + off, reclen);
            flat_used += reclen;
            off += reclen;
        }
    }

    if (!found_name) {
        if (!have_insert)
            insert_off = flat_used;
        if (insert_off < flat_used)
            memmove(flat + insert_off + newrec_len, flat + insert_off,
                    flat_used - insert_off);
        memcpy(flat + insert_off, newrec, newrec_len);
        flat_used += newrec_len;
    }

    return ed_dir_pack(flat, flat_used, out_blocks, out_nblk_cap, out_nblk);
}

ods2_status_t ods2_dir_remove_blocks(const uint8_t *in_blocks, unsigned in_nblk,
                                     const char *name, unsigned namecount,
                                     uint16_t version,
                                     uint8_t *flat, size_t flat_cap,
                                     uint8_t *out_blocks, unsigned out_nblk_cap,
                                     unsigned *out_nblk, int *removed)
{
    unsigned b;
    size_t flat_used = 0, need;

    if (!in_blocks || !name || !flat || !out_blocks || !out_nblk || !removed)
        return ODS2_ERR_ARGS;
    if (namecount == 0 || namecount > 255)
        return ODS2_ERR_ARGS;
    *removed = 0;

    need = (size_t)in_nblk * ODS2_BLOCK_SIZE + 16;
    if (flat_cap < need)
        return ODS2_ERR_NOSPACE;

    for (b = 0; b < in_nblk; b++) {
        const uint8_t *blk = in_blocks + (size_t)b * ODS2_BLOCK_SIZE;
        unsigned off = 0;
        for (;;) {
            uint16_t rec_size;
            unsigned reclen, nc, val_off, n_ent, i, keep;
            if (off + 6 > ODS2_BLOCK_SIZE)
                break;
            rec_size = ed_rd16(blk + off);
            if (rec_size == ODS2_DIR_END)
                break;
            reclen = 2u + rec_size;
            if (off + reclen > ODS2_BLOCK_SIZE)
                return ODS2_ERR_FORMAT;
            nc = blk[off + 5];
            if (6u + nc > reclen)
                return ODS2_ERR_FORMAT;

            if (!ed_dir_name_cmp(name, namecount, (const char *)blk + off + 6, nc)) {
                /* Matching name: drop the requested version(s). */
                val_off = 6u + nc;
                if (val_off & 1) val_off++;
                if (val_off > reclen || ((reclen - val_off) % 8u) != 0)
                    return ODS2_ERR_FORMAT;
                n_ent = (reclen - val_off) / 8u;
                if (version == 0) {
                    *removed = 1;               /* whole name gone */
                    off += reclen;
                    continue;
                }
                /* Rebuild keeping every value entry except `version`. */
                keep = 0;
                {
                    uint8_t *dst = flat + flat_used;
                    unsigned dst_val = val_off, wrote = 0;
                    memset(dst, 0xFF, val_off);
                    memcpy(dst + 2, blk + off + 2, 4);       /* verlimit/flags/nc */
                    memcpy(dst + 6, blk + off + 6, nc);
                    for (i = 0; i < n_ent; i++) {
                        const uint8_t *ent = blk + off + val_off + i * 8u;
                        if (ed_rd16(ent) == version) { *removed = 1; continue; }
                        memcpy(dst + dst_val + wrote * 8u, ent, 8u);
                        wrote++;
                    }
                    keep = wrote;
                    if (keep > 0) {
                        unsigned nl = val_off + keep * 8u;
                        ed_put16(dst + 0, (uint16_t)(nl - 2));
                        flat_used += nl;
                    }
                    /* keep==0 -> the whole record is dropped (last version). */
                }
                off += reclen;
                continue;
            }
            memcpy(flat + flat_used, blk + off, reclen);
            flat_used += reclen;
            off += reclen;
        }
    }

    /* Never grow / never deallocate directory blocks on remove -- repack, then
     * PAD OUT to the original block count so the file keeps its allocation
     * (trailing blocks become empty ODS2_DIR_END); the ACP therefore never
     * rewrites the directory's FH2 map on a delete. */
    {
        unsigned packed = 0, bi;
        ods2_status_t st = ed_dir_pack(flat, flat_used, out_blocks,
                                       in_nblk > out_nblk_cap ? out_nblk_cap : in_nblk,
                                       &packed);
        if (st != ODS2_OK)
            return st;
        for (bi = packed; bi < in_nblk && bi < out_nblk_cap; bi++)
            memset(out_blocks + (size_t)bi * ODS2_BLOCK_SIZE, 0xFF, ODS2_BLOCK_SIZE);
        *out_nblk = (in_nblk <= out_nblk_cap) ? in_nblk : out_nblk_cap;
    }
    return ODS2_OK;
}
