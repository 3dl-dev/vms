/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_reader.c - Reader for GENUINE ODS-2 (Files-11 level 2) volumes.
 *
 * Increment 1 of the genuine-ODS-2 effort. Parses and validates the home
 * block, walks INDEXF.SYS file headers, decodes FM2 retrieval pointers, and
 * lists directories. See src/vmsfs/include/vmsfs/ods2.h for full clean-room
 * provenance ([N] Nankervis simh/simtools ods2, [S] public Files-11 docs).
 *
 * The reader is READ-ONLY and operates over an in-memory volume image; it
 * does not perform I/O and links against nothing but the C runtime, so it is
 * equally usable from the kernel-side MSCP server path later.
 *
 * ENDIANNESS: ODS-2 on-disk fields are little-endian (VAX/Alpha). All scalar
 * reads go through the le16/le32 helpers below, so the reader is correct on a
 * big-endian host too; the checksum sums LE 16-bit words explicitly.
 */

#include "vmsfs/ods2.h"

#include <string.h>

/* ---- little-endian scalar reads (endian-independent) ---- */

static inline uint16_t le16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

static inline uint32_t le32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

const char *ods2_strerror(ods2_status_t st)
{
    switch (st) {
    case ODS2_OK:           return "ok";
    case ODS2_ERR_ARGS:     return "bad arguments";
    case ODS2_ERR_SIZE:     return "buffer too small";
    case ODS2_ERR_CHECKSUM: return "checksum mismatch";
    case ODS2_ERR_FORMAT:   return "bad format / structure level";
    case ODS2_ERR_RANGE:    return "block out of range";
    case ODS2_ERR_NOTFOUND: return "not found";
    case ODS2_ERR_NOSPACE:  return "no space";
    case ODS2_ERR_IO:       return "backing-store I/O error";
    default:                return "unknown";
    }
}

/*
 * 16-bit additive checksum over `count` LE 16-bit words. [N] access.c
 * checksum(): result += *ptr++ for count words, truncated to 16 bits.
 */
uint16_t ods2_checksum(const void *block, unsigned count)
{
    const uint8_t *b = (const uint8_t *)block;
    unsigned result = 0;
    unsigned i;

    for (i = 0; i < count; i++)
        result += le16(b + (size_t)i * 2);

    return (uint16_t)result;
}

ods2_status_t ods2_home_parse(const void *block, size_t block_len,
                              ods2_home_t *out, int strict_level)
{
    const uint8_t *b = (const uint8_t *)block;
    uint16_t stored1, stored2, calc1, calc2;

    if (!block || !out)
        return ODS2_ERR_ARGS;
    if (block_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;

    /* checksum1 covers the first 29 words (bytes 0..57); it sits at word 29. */
    stored1 = le16(b + offsetof(ods2_home_t, hm2_checksum1));
    calc1   = ods2_checksum(b, 29);
    if (stored1 != calc1)
        return ODS2_ERR_CHECKSUM;

    /* checksum2 covers all 255 words (bytes 0..509); it sits at word 255. */
    stored2 = le16(b + offsetof(ods2_home_t, hm2_checksum2));
    calc2   = ods2_block_checksum(b);
    if (stored2 != calc2)
        return ODS2_ERR_CHECKSUM;

    /* Format string must be "DECFILE11B  ". [N] */
    if (memcmp(b + offsetof(ods2_home_t, hm2_format),
               ODS2_FORMAT_STRING, ODS2_FORMAT_LEN) != 0)
        return ODS2_ERR_FORMAT;

    if (strict_level &&
        le16(b + offsetof(ods2_home_t, hm2_struclev)) != ODS2_STRUCLEV_V2)
        return ODS2_ERR_FORMAT;

    /* Byte-for-byte copy: on-disk layout == struct layout by construction. */
    memcpy(out, b, sizeof(*out));
    return ODS2_OK;
}

ods2_status_t ods2_fh2_parse(const void *block, size_t block_len,
                             ods2_fh2_t *out)
{
    const uint8_t *b = (const uint8_t *)block;
    uint16_t stored, calc;

    if (!block || !out)
        return ODS2_ERR_ARGS;
    if (block_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;

    stored = le16(b + offsetof(ods2_fh2_t, fh2_checksum));
    calc   = ods2_block_checksum(b);
    if (stored != calc)
        return ODS2_ERR_CHECKSUM;

    memcpy(out, b, sizeof(*out));
    return ODS2_OK;
}

/*
 * Parse and validate the Storage Control Block (BITMAP.SYS VBN1). Same
 * checksum+struclev convention as the home block / file headers. See
 * ods2_scb_t in vmsfs/ods2.h for the increment-2 real-image findings on the
 * fields this validates and exposes.
 */
ods2_status_t ods2_scb_parse(const void *block, size_t block_len,
                             ods2_scb_t *out)
{
    const uint8_t *b = (const uint8_t *)block;
    uint16_t stored, calc;

    if (!block || !out)
        return ODS2_ERR_ARGS;
    if (block_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;

    stored = le16(b + offsetof(ods2_scb_t, scb_checksum));
    calc   = ods2_block_checksum(b);
    if (stored != calc)
        return ODS2_ERR_CHECKSUM;

    if (le16(b + offsetof(ods2_scb_t, scb_struclev)) != ODS2_STRUCLEV_V2)
        return ODS2_ERR_FORMAT;

    memcpy(out, b, sizeof(*out));
    return ODS2_OK;
}

/*
 * Recompute and validate the [F6] checksum of a SECURITY.SYS VBN1 data
 * block (see ods2.h's WRITER provenance comment and ods2_writer.c's
 * ods2_security_checksum() -- the SAME byte-lane XOR fold, reimplemented
 * here so the reader has no link-time dependency on the writer).
 */
ods2_status_t ods2_security_parse(const void *block, size_t block_len,
                                  char *label_out, size_t label_out_size,
                                  ods2_uic_t *owner_out)
{
    const uint8_t *b = (const uint8_t *)block;
    uint32_t stored, calc;
    uint16_t lenfield;
    size_t strlen_, total, n, i;
    uint8_t lanes[4] = { 0, 0, 0, 0 };

    if (!block)
        return ODS2_ERR_ARGS;
    if (block_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;

    lenfield = le16(b + 0x50);
    if (lenfield < 5 || lenfield > 16)   /* strlen 1..12, +4 */
        return ODS2_ERR_FORMAT;
    strlen_ = (size_t)lenfield - 4;

    total = 78u + strlen_;
    n = (total / 4u) * 4u;
    for (i = 0; i < n; i++)
        lanes[i % 4] ^= b[4 + i];
    calc = (uint32_t)lanes[0] | ((uint32_t)lanes[1] << 8) |
           ((uint32_t)lanes[2] << 16) | ((uint32_t)lanes[3] << 24);

    stored = le32(b + 0);
    if (stored != calc)
        return ODS2_ERR_CHECKSUM;

    if (label_out && label_out_size > 0) {
        size_t copy_len = strlen_;
        if (copy_len > label_out_size - 1)
            copy_len = label_out_size - 1;
        memcpy(label_out, b + 0x52, copy_len);
        label_out[copy_len] = '\0';
    }
    if (owner_out) {
        owner_out->uic_member = le16(b + 0x1C + 0);
        owner_out->uic_group  = le16(b + 0x1C + 2);
    }

    return ODS2_OK;
}

const ods2_ident_t *ods2_fh2_ident(const void *header_block)
{
    const uint8_t *b = (const uint8_t *)header_block;
    unsigned id_off_words;

    if (!header_block)
        return NULL;

    id_off_words = b[offsetof(ods2_fh2_t, fh2_idoffset)];
    /* ident area must fit inside the 512-byte header. */
    if ((size_t)id_off_words * 2 + sizeof(ods2_ident_t) > ODS2_BLOCK_SIZE)
        return NULL;

    return (const ods2_ident_t *)(b + (size_t)id_off_words * 2);
}

/*
 * Decode the FM2 retrieval pointers in the map area. [N] access.c getwindow():
 *   switch (word0 >> 14):
 *     0: 1 word, empty placeholder
 *     1: 2 words, count=(w0 & 0377)+1,   lbn=((w0 & 037400)<<8)+w1
 *     2: 3 words, count=(w0 & 037777)+1, lbn=(w2<<16)+w1
 *     3: 4 words, count=((w0 & 037777)<<16)+w1+1, lbn=(w3<<16)+w2
 */
ods2_status_t ods2_fh2_map_walk(const void *header_block,
                                ods2_map_cb cb, void *ctx, unsigned *n_out)
{
    const uint8_t *b = (const uint8_t *)header_block;
    unsigned mp_off_words, map_inuse, i;
    unsigned n = 0;

    if (!header_block || !cb)
        return ODS2_ERR_ARGS;

    mp_off_words = b[offsetof(ods2_fh2_t, fh2_mpoffset)];
    map_inuse    = b[offsetof(ods2_fh2_t, fh2_map_inuse)];

    /* Map area must lie within the header. */
    if ((size_t)(mp_off_words + map_inuse) * 2 > ODS2_BLOCK_SIZE)
        return ODS2_ERR_RANGE;

    i = 0;
    while (i < map_inuse) {
        const uint8_t *mp = b + (size_t)(mp_off_words + i) * 2;
        uint16_t w0 = le16(mp);
        unsigned fmt = (w0 >> 14) & 0x3;
        ods2_extent_t ext = { 0, 0 };
        unsigned words;

        switch (fmt) {
        case 0:
            words = 1;
            /* placeholder / no space: skip (count 0). */
            i += words;
            continue;
        case 1:
            words = 2;
            if (i + words > map_inuse) return ODS2_ERR_RANGE;
            ext.count = (unsigned)(w0 & 0x00FF) + 1;
            ext.lbn   = ((unsigned)(w0 & 0x3F00) << 8) + le16(mp + 2);
            break;
        case 2:
            words = 3;
            if (i + words > map_inuse) return ODS2_ERR_RANGE;
            ext.count = (unsigned)(w0 & 0x3FFF) + 1;
            ext.lbn   = ((unsigned)le16(mp + 4) << 16) + le16(mp + 2);
            break;
        default: /* 3 */
            words = 4;
            if (i + words > map_inuse) return ODS2_ERR_RANGE;
            ext.count = ((unsigned)(w0 & 0x3FFF) << 16) + le16(mp + 2) + 1;
            ext.lbn   = ((unsigned)le16(mp + 6) << 16) + le16(mp + 4);
            break;
        }

        n++;
        if (cb(&ext, ctx)) {
            i += words;
            break;
        }
        i += words;
    }

    if (n_out)
        *n_out = n;
    return ODS2_OK;
}

const uint8_t *ods2_volume_block(const ods2_volume_t *vol, uint32_t lbn)
{
    if (!vol || lbn >= vol->nblocks)
        return NULL;
    return vol->image + (size_t)lbn * ODS2_BLOCK_SIZE;
}

ods2_status_t ods2_volume_open(ods2_volume_t *vol,
                               const void *image, size_t image_len)
{
    ods2_status_t st;
    const uint8_t *home_blk;

    if (!vol || !image)
        return ODS2_ERR_ARGS;
    if (image_len < 2u * ODS2_BLOCK_SIZE)   /* need at least boot + home */
        return ODS2_ERR_SIZE;

    memset(vol, 0, sizeof(*vol));
    vol->image     = (const uint8_t *)image;
    vol->image_len = image_len;
    vol->nblocks   = (uint32_t)(image_len / ODS2_BLOCK_SIZE);

    /*
     * The home block conventionally lives at LBN 1 (block 0 is the boot
     * block). Parse it there; the parse validates checksums + format so a
     * wrong guess is rejected rather than silently accepted.
     */
    home_blk = ods2_volume_block(vol, 1);
    if (!home_blk)
        return ODS2_ERR_RANGE;

    st = ods2_home_parse(home_blk, ODS2_BLOCK_SIZE, &vol->home, /*strict*/1);
    if (st != ODS2_OK)
        return st;

    return ODS2_OK;
}

ods2_status_t ods2_volume_read_header(const ods2_volume_t *vol,
                                      uint32_t fid_num,
                                      void *header_out, size_t out_len)
{
    uint32_t idx_lbn, hdr_lbn;
    const uint8_t *hdr;
    ods2_fh2_t tmp;
    ods2_status_t st;

    if (!vol || !header_out)
        return ODS2_ERR_ARGS;
    if (out_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (fid_num < 1)
        return ODS2_ERR_ARGS;
    if (fid_num > vol->home.hm2_maxfiles)
        return ODS2_ERR_RANGE;

    /*
     * File headers begin immediately after the index-file bitmap:
     *   idx_lbn = hm2_ibmaplbn + hm2_ibmapsize
     * and file N's primary header is at idx_lbn + (N-1). [N] (Nankervis
     * computes the identical base) [S].
     */
    idx_lbn = vol->home.hm2_ibmaplbn + vol->home.hm2_ibmapsize;
    hdr_lbn = idx_lbn + (fid_num - 1);

    hdr = ods2_volume_block(vol, hdr_lbn);
    if (!hdr)
        return ODS2_ERR_RANGE;

    st = ods2_fh2_parse(hdr, ODS2_BLOCK_SIZE, &tmp);
    if (st != ODS2_OK)
        return st;

    memcpy(header_out, hdr, ODS2_BLOCK_SIZE);
    return ODS2_OK;
}

/* ---- directory listing ---- */

/* Context threading the volume's data blocks through the map walk. */
struct dir_walk_ctx {
    const ods2_volume_t *vol;
    ods2_dir_cb          cb;
    void                *user;
    ods2_status_t        st;
    int                  stop;
    uint32_t             vbn;   /* running VBN (1-based) — unused but honest */
};

/*
 * Decode one directory data block's records, invoking `cb` per {name,
 * version, fid}. Shared by the in-memory walker (dir_scan_block below) and
 * the block-backed one (ods2_bdev_list_dir); returns 1 if `cb` asked to stop.
 */
int ods2_dir_block_scan(const void *dir_block, ods2_dir_cb cb, void *ctx)
{
    const uint8_t *blk = (const uint8_t *)dir_block;
    unsigned off = 0;

    if (!dir_block || !cb)
        return 0;

    while (off + 6 <= ODS2_BLOCK_SIZE) {
        uint16_t rec_size = le16(blk + off);
        unsigned namecount, name_off, val_off, rec_end;

        if (rec_size == ODS2_DIR_END)
            break;                          /* end of records in this block */

        rec_end = off + 2 + rec_size;       /* rec_size excludes the size word */
        if (rec_end > ODS2_BLOCK_SIZE)
            break;                          /* malformed / truncated */

        namecount = blk[off + 5];
        name_off  = off + 6;
        if (name_off + namecount > rec_end)
            break;                          /* malformed */

        /* Value entries follow the name, word-aligned to an even offset. */
        val_off = name_off + namecount;
        if (val_off & 1)
            val_off++;

        while (val_off + (unsigned)sizeof(ods2_dir_ent_t) <= rec_end) {
            uint16_t version = le16(blk + val_off);
            ods2_fid_t fid;

            fid.fid_num = le16(blk + val_off + 2);
            fid.fid_seq = le16(blk + val_off + 4);
            fid.fid_rvn = blk[val_off + 6];
            fid.fid_nmx = blk[val_off + 7];

            if (cb((const char *)(blk + name_off), namecount,
                   version, &fid, ctx))
                return 1;
            val_off += (unsigned)sizeof(ods2_dir_ent_t);
        }

        off = rec_end;
    }
    return 0;
}

/* Decode one directory data block's records (in-memory walk thread). */
static int dir_scan_block(struct dir_walk_ctx *c, const uint8_t *blk)
{
    if (ods2_dir_block_scan(blk, c->cb, c->user)) {
        c->stop = 1;
        return 1;
    }
    return 0;
}

/* map-walk callback: read each extent's blocks and scan them as directory. */
static int dir_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    struct dir_walk_ctx *c = (struct dir_walk_ctx *)ctx;
    uint32_t k;

    for (k = 0; k < ext->count; k++) {
        const uint8_t *blk = ods2_volume_block(c->vol, ext->lbn + k);
        if (!blk) {
            c->st = ODS2_ERR_RANGE;
            return 1;
        }
        c->vbn++;
        if (dir_scan_block(c, blk))
            return 1;   /* user asked to stop */
    }
    return 0;
}

ods2_status_t ods2_volume_list_dir(const ods2_volume_t *vol,
                                   const void *dir_header_block,
                                   ods2_dir_cb cb, void *ctx)
{
    struct dir_walk_ctx c;
    ods2_fh2_t tmp;
    ods2_status_t st;

    if (!vol || !dir_header_block || !cb)
        return ODS2_ERR_ARGS;

    /* Validate the directory's header before trusting its map area. */
    st = ods2_fh2_parse(dir_header_block, ODS2_BLOCK_SIZE, &tmp);
    if (st != ODS2_OK)
        return st;

    c.vol  = vol;
    c.cb   = cb;
    c.user = ctx;
    c.st   = ODS2_OK;
    c.stop = 0;
    c.vbn  = 0;

    st = ods2_fh2_map_walk(dir_header_block, dir_extent_cb, &c, NULL);
    if (st != ODS2_OK)
        return st;
    return c.st;
}

/* ---- RMS variable-length (RFM=VAR) record decode [F16], increment 9 ---- */

ods2_status_t ods2_var_records_decode(const void *data, size_t data_bytes,
                                      char *out, size_t out_cap,
                                      size_t *out_len)
{
    const uint8_t *b = (const uint8_t *)data;
    size_t off = 0, w = 0;

    if (!data || (!out && out_cap > 0))
        return ODS2_ERR_ARGS;

    while (off < data_bytes) {
        uint16_t rlen;
        size_t content_end, next;

        if (off + 2 > data_bytes)
            return ODS2_ERR_FORMAT; /* truncated length word */
        rlen = le16(b + off);
        content_end = off + 2 + rlen;
        if (content_end > data_bytes)
            return ODS2_ERR_FORMAT; /* record body runs past EOF */

        if (w + (size_t)rlen + 1 > out_cap) /* +1 for the '\n' this writes */
            return ODS2_ERR_SIZE;
        if (rlen > 0)
            memcpy(out + w, b + off + 2, rlen);
        w += rlen;
        out[w++] = '\n';

        next = content_end;
        if (rlen & 1)
            next++;             /* word-alignment pad byte -- see [F16] */
        off = next;
    }

    if (out_len)
        *out_len = w;
    return ODS2_OK;
}

/* Context + map-walk callback: capture the file's single retrieval-pointer
 * extent (ods2_wvolume_create_file() never produces more than one). */
struct single_extent_ctx {
    ods2_extent_t ext;
    unsigned      n;
};

static int single_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    struct single_extent_ctx *c = (struct single_extent_ctx *)ctx;
    if (c->n == 0)
        c->ext = *ext;
    c->n++;
    return 0;
}

ods2_status_t ods2_file_read_text(const ods2_volume_t *vol,
                                  const void *file_header_block,
                                  char *out, size_t out_cap, size_t *out_len)
{
    ods2_fh2_t hdr;
    struct single_extent_ctx ec = { { 0, 0 }, 0 };
    ods2_status_t st;
    const uint8_t *blk;
    size_t data_bytes;

    if (!vol || !file_header_block || (!out && out_cap > 0))
        return ODS2_ERR_ARGS;

    st = ods2_fh2_parse(file_header_block, ODS2_BLOCK_SIZE, &hdr);
    if (st != ODS2_OK)
        return st;

    st = ods2_fh2_map_walk(file_header_block, single_extent_cb, &ec, NULL);
    if (st != ODS2_OK)
        return st;
    if (ec.n != 1)
        return ODS2_ERR_RANGE; /* multi-extent data files unsupported here */

    blk = ods2_volume_block(vol, ec.ext.lbn);
    if (!blk)
        return ODS2_ERR_RANGE;

    data_bytes = ods2_recattr_data_bytes(&hdr.fh2_recattr);
    if (data_bytes > (size_t)ec.ext.count * ODS2_BLOCK_SIZE)
        return ODS2_ERR_FORMAT; /* recattr claims more data than allocated */

    return ods2_var_records_decode(blk, data_bytes, out, out_cap, out_len);
}
