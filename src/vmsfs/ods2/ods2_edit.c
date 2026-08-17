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

/* ---- little-endian scalar access (endian-independent), as ods2_writer.c ---- */

static inline void ed_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
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
