/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_path.c - block-backed ODS-2 PATH RESOLUTION + FILE-CONTENT read.
 *
 * vms-5eb read-path foundation (the piece R2/R3/R5 all need but the library
 * did not yet offer over a block device). ods2_reader.c and its block-backed
 * twin ods2_bdev.c expose the Files-11 PRIMITIVES:
 *
 *   - ods2_bdev_read_header(fid_num)  -> a validated 512-byte file header
 *   - ods2_bdev_list_dir(dir_header)  -> {name, version, fid} per dir record
 *   - ods2_fh2_map_walk(header)       -> the file's FM2 retrieval-pointer runs
 *   - ods2_recattr_data_bytes()       -> a file's true valid byte count
 *   - ods2_var_records_decode()       -> RFM=VAR record stream -> text
 *
 * This file COMPOSES those into the two operations the live userspace
 * RMS/DCL/MOUNT path needs:
 *
 *   1. resolve an on-volume path -- a list of directory components plus an
 *      optional file component -- to a file/directory header + FID, by
 *      walking from the MFD (FID 4, [000000]); and
 *   2. read a file's CONTENT (raw bytes, or newline-joined VAR text) by
 *      pread-ing its retrieval-pointer extents off the block device.
 *
 * Rule 8: this adds NO on-disk format facts. It only SEQUENCES directory
 * traversal + extent reads the reader already implements from its cited
 * Files-11 sources. The name-matching convention it applies -- a directory
 * component "SYS0" is the on-disk entry "SYS0.DIR"; a file spelled
 * "LOGIN.COM;3" is the on-disk entry "LOGIN.COM" at version 3 -- is VMS
 * filespec SEMANTICS, not a byte-layout fact. Names are matched
 * case-insensitively (VMS filespecs are case-blind).
 *
 * Like ods2_bdev.c, the fd inside the ods2_bdev_t is borrowed; this file
 * only reads. The write side (create/put/erase over a block device) is a
 * separate, not-yet-built foundation -- see docs/design-ods2-runtime-flip.md.
 */

#include "vmsfs/ods2.h"

#include "ods2_kcompat.h"   /* string/ctype/snprintf + ods2_kalloc/ods2_kfree */

/* Case-insensitive compare of a length-counted on-disk name against a C
 * string. Returns 1 on equal, 0 otherwise. */
static int name_eq_ci(const char *name, unsigned name_len, const char *want)
{
    size_t wl = strlen(want);
    unsigned i;
    if ((size_t)name_len != wl)
        return 0;
    for (i = 0; i < name_len; i++) {
        if (toupper((unsigned char)name[i]) != toupper((unsigned char)want[i]))
            return 0;
    }
    return 1;
}

/* ---- directory entry lookup ---- */

struct dir_find_ctx {
    const char *want;       /* name to match, INCLUDING type (".DIR"/".EXT") */
    uint16_t    want_ver;   /* 0 => highest version wins                     */
    int         matched;
    uint16_t    best_ver;
    ods2_fid_t  best_fid;
};

static int dir_find_cb(const char *name, unsigned name_len,
                       uint16_t version, const ods2_fid_t *fid, void *ctx)
{
    struct dir_find_ctx *c = (struct dir_find_ctx *)ctx;
    if (!name_eq_ci(name, name_len, c->want))
        return 0;   /* keep scanning */

    if (c->want_ver != 0) {
        if (version == c->want_ver) {
            c->matched  = 1;
            c->best_ver = version;
            c->best_fid = *fid;
            return 1;   /* exact version -- stop */
        }
        return 0;
    }

    /* Highest version wins. Directory records are name-then-descending-version
     * ordered on disk, but do not rely on that -- track the max explicitly. */
    if (!c->matched || version >= c->best_ver) {
        c->matched  = 1;
        c->best_ver = version;
        c->best_fid = *fid;
    }
    return 0;
}

ods2_status_t ods2_bdev_dir_find(const ods2_bdev_t *bv,
                                 const void *dir_header_block,
                                 const char *name, uint16_t want_version,
                                 ods2_fid_t *fid_out, uint16_t *version_out)
{
    struct dir_find_ctx c;
    ods2_status_t st;

    if (!bv || !dir_header_block || !name)
        return ODS2_ERR_ARGS;

    c.want     = name;
    c.want_ver = want_version;
    c.matched  = 0;
    c.best_ver = 0;
    memset(&c.best_fid, 0, sizeof(c.best_fid));

    st = ods2_bdev_list_dir(bv, dir_header_block, dir_find_cb, &c);
    if (st != ODS2_OK)
        return st;
    if (!c.matched)
        return ODS2_ERR_NOTFOUND;

    if (fid_out)
        *fid_out = c.best_fid;
    if (version_out)
        *version_out = c.best_ver;
    return ODS2_OK;
}

/* ---- path resolution (walk from the MFD) ---- */

ods2_status_t ods2_bdev_resolve_dir(const ods2_bdev_t *bv,
                                    const char *const *comps, unsigned ndirs,
                                    ods2_fid_t *fid_out,
                                    void *dir_header_out, size_t out_len)
{
    uint8_t hdr[ODS2_BLOCK_SIZE];
    ods2_fid_t cur;
    ods2_status_t st;
    unsigned i;

    if (!bv || (ndirs > 0 && !comps) || !dir_header_out)
        return ODS2_ERR_ARGS;
    if (out_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;

    /* Start at the MFD ([000000], FID 4). */
    st = ods2_bdev_read_header(bv, ODS2_FID_MFD, hdr, sizeof(hdr));
    if (st != ODS2_OK)
        return st;
    memset(&cur, 0, sizeof(cur));
    cur.fid_num = ODS2_FID_MFD;
    cur.fid_seq = ODS2_FID_MFD;

    for (i = 0; i < ndirs; i++) {
        char want[64];
        ods2_fid_t child;

        if (!comps[i] || comps[i][0] == '\0')
            return ODS2_ERR_ARGS;
        /* A directory component "SYS0" is the on-disk entry "SYS0.DIR". */
        if (strlen(comps[i]) + 5 > sizeof(want))
            return ODS2_ERR_SIZE;
        snprintf(want, sizeof(want), "%s.DIR", comps[i]);

        st = ods2_bdev_dir_find(bv, hdr, want, /*highest*/0, &child, NULL);
        if (st != ODS2_OK)
            return st;

        st = ods2_bdev_read_header(bv, ods2_fid_number(&child), hdr, sizeof(hdr));
        if (st != ODS2_OK)
            return st;
        cur = child;
    }

    memcpy(dir_header_out, hdr, ODS2_BLOCK_SIZE);
    if (fid_out)
        *fid_out = cur;
    return ODS2_OK;
}

ods2_status_t ods2_bdev_resolve_file(const ods2_bdev_t *bv,
                                     const char *const *comps, unsigned ndirs,
                                     const char *filename, uint16_t version,
                                     ods2_fid_t *fid_out,
                                     void *file_header_out, size_t out_len)
{
    uint8_t dirhdr[ODS2_BLOCK_SIZE];
    ods2_fid_t fid;
    ods2_status_t st;

    if (!bv || !filename || !file_header_out)
        return ODS2_ERR_ARGS;
    if (out_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;

    st = ods2_bdev_resolve_dir(bv, comps, ndirs, NULL, dirhdr, sizeof(dirhdr));
    if (st != ODS2_OK)
        return st;

    st = ods2_bdev_dir_find(bv, dirhdr, filename, version, &fid, NULL);
    if (st != ODS2_OK)
        return st;

    st = ods2_bdev_read_header(bv, ods2_fid_number(&fid),
                               file_header_out, out_len);
    if (st != ODS2_OK)
        return st;
    if (fid_out)
        *fid_out = fid;
    return ODS2_OK;
}

/* ---- file content read (block-backed) ---- */

struct file_read_ctx {
    const ods2_bdev_t *bv;
    uint8_t          *out;
    size_t            cap;
    size_t            written;
    size_t            remaining;   /* valid bytes still to copy            */
    ods2_status_t     st;
};

static int file_read_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    struct file_read_ctx *c = (struct file_read_ctx *)ctx;
    uint32_t k;

    for (k = 0; k < ext->count && c->remaining > 0; k++) {
        uint8_t blk[ODS2_BLOCK_SIZE];
        size_t take;
        ods2_status_t st = ods2_bdev_read_block(c->bv, ext->lbn + k,
                                                blk, sizeof(blk));
        if (st != ODS2_OK) {
            c->st = st;
            return 1;   /* stop the walk */
        }
        take = c->remaining < ODS2_BLOCK_SIZE ? c->remaining : ODS2_BLOCK_SIZE;
        if (c->written + take > c->cap) {
            c->st = ODS2_ERR_SIZE;
            return 1;
        }
        memcpy(c->out + c->written, blk, take);
        c->written   += take;
        c->remaining -= take;
    }
    return 0;
}

ods2_status_t ods2_bdev_read_file(const ods2_bdev_t *bv,
                                  const void *file_header_block,
                                  void *out, size_t out_cap, size_t *out_len)
{
    ods2_fh2_t hdr;
    struct file_read_ctx c;
    ods2_status_t st;
    size_t data_bytes;

    if (!bv || !file_header_block || (!out && out_cap > 0))
        return ODS2_ERR_ARGS;

    st = ods2_fh2_parse(file_header_block, ODS2_BLOCK_SIZE, &hdr);
    if (st != ODS2_OK)
        return st;

    data_bytes = ods2_recattr_data_bytes(&hdr.fh2_recattr);
    if (data_bytes > out_cap)
        return ODS2_ERR_SIZE;

    c.bv        = bv;
    c.out       = (uint8_t *)out;
    c.cap       = out_cap;
    c.written   = 0;
    c.remaining = data_bytes;
    c.st        = ODS2_OK;

    st = ods2_fh2_map_walk(file_header_block, file_read_extent_cb, &c, NULL);
    if (st != ODS2_OK)
        return st;
    if (c.st != ODS2_OK)
        return c.st;
    /* The retrieval map must have covered the whole valid-byte span. */
    if (c.remaining != 0)
        return ODS2_ERR_FORMAT;

    if (out_len)
        *out_len = c.written;
    return ODS2_OK;
}

ods2_status_t ods2_bdev_read_file_text(const ods2_bdev_t *bv,
                                       const void *file_header_block,
                                       char *out, size_t out_cap,
                                       size_t *out_len)
{
    ods2_fh2_t hdr;
    ods2_status_t st;
    size_t data_bytes, raw_len = 0;
    uint8_t *raw;

    if (!bv || !file_header_block || (!out && out_cap > 0))
        return ODS2_ERR_ARGS;

    st = ods2_fh2_parse(file_header_block, ODS2_BLOCK_SIZE, &hdr);
    if (st != ODS2_OK)
        return st;

    data_bytes = ods2_recattr_data_bytes(&hdr.fh2_recattr);
    if (data_bytes == 0) {
        if (out_len)
            *out_len = 0;
        return ODS2_OK;
    }

    raw = (uint8_t *)ods2_kalloc(data_bytes);
    if (!raw)
        return ODS2_ERR_IO;

    st = ods2_bdev_read_file(bv, file_header_block, raw, data_bytes, &raw_len);
    if (st != ODS2_OK) {
        ods2_kfree(raw);
        return st;
    }

    st = ods2_var_records_decode(raw, raw_len, out, out_cap, out_len);
    ods2_kfree(raw);
    return st;
}
