/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_bdev.c - Block-BACKED variant of the GENUINE ODS-2 reader.
 *
 * vms-6cb (R1 of the real-ODS-2-runtime work): the genuine ODS-2 reader
 * (ods2_reader.c) parses byte-real Files-11 level 2 structures over a
 * whole-volume in-memory `const uint8_t *image`. This file is the block-BACKED
 * twin -- the SAME parse/validate/walk logic, but blocks are fetched on demand
 * one at a time from a backing device instead of a resident buffer.
 *
 * rd vms-dcd (epic vms-208): the raw block access no longer hard-codes POSIX
 * pread() here. It goes through the ods2_block.h SEAM, so this SAME file runs
 * BOTH userspace (fd + pread, ods2_block_posix.c) AND kernel-resident (a real
 * struct block_device through vmsfs_bio.h, ods2_block_kern.c). This file itself
 * is backend-neutral: it names no fd, no pread, no libc I/O.
 *
 * ADDITIVE: the in-memory ods2_volume_t path (ods2_reader.c) is unchanged; both
 * variants coexist. This file reuses the pure, I/O-free reader primitives
 * (ods2_home_parse, ods2_fh2_parse, ods2_fh2_map_walk, ods2_dir_block_scan)
 * verbatim -- it adds NO new ODS-2 format facts (Rule 8); it only changes WHERE
 * a 512-byte block comes from.
 */

#include "vmsfs/ods2.h"
#include "ods2_kcompat.h"   /* memset */
#include "ods2_block.h"     /* the block-access seam */

/* Read exactly one 512-byte block at `lbn` through the seam. */
static ods2_status_t bdev_read_block(const ods2_bdev_t *bv, uint32_t lbn,
                                     void *buf, size_t buf_len)
{
    if (!bv || !buf)
        return ODS2_ERR_ARGS;
    return ods2_blk_read(ODS2_BDEV_DEV(bv), bv->nblocks, lbn, buf, buf_len);
}

ods2_status_t ods2_bdev_read_block(const ods2_bdev_t *bv, uint32_t lbn,
                                   void *buf, size_t buf_len)
{
    return bdev_read_block(bv, lbn, buf, buf_len);
}

/*
 * Shared open tail: bv->nblocks + backing-device field are set by the
 * per-backend constructor (ods2_bdev_open / ods2_bdev_open_host); validate the
 * home block at LBN 1 (BOTH additive checksums + "DECFILE11B  " + strict
 * structure level), so a non-ODS-2 volume is rejected rather than accepted.
 */
ods2_status_t ods2_bdev_finish_open(ods2_bdev_t *bv)
{
    uint8_t home_blk[ODS2_BLOCK_SIZE];
    ods2_status_t st;

    if (!bv)
        return ODS2_ERR_ARGS;
    if (bv->nblocks < 2u)   /* need at least boot + home */
        return ODS2_ERR_SIZE;

    st = bdev_read_block(bv, 1, home_blk, sizeof(home_blk));
    if (st != ODS2_OK)
        return st;

    return ods2_home_parse(home_blk, ODS2_BLOCK_SIZE, &bv->home, /*strict*/1);
}

ods2_status_t ods2_bdev_read_header(const ods2_bdev_t *bv, uint32_t fid_num,
                                    void *header_out, size_t out_len)
{
    uint32_t idx_lbn, hdr_lbn;
    ods2_fh2_t tmp;
    ods2_status_t st;

    if (!bv || !header_out)
        return ODS2_ERR_ARGS;
    if (out_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (fid_num < 1)
        return ODS2_ERR_ARGS;
    if (fid_num > bv->home.hm2_maxfiles)
        return ODS2_ERR_RANGE;

    /* Identical INDEXF.SYS arithmetic to ods2_volume_read_header(). [N][S] */
    idx_lbn = bv->home.hm2_ibmaplbn + bv->home.hm2_ibmapsize;
    hdr_lbn = idx_lbn + (fid_num - 1);

    st = bdev_read_block(bv, hdr_lbn, header_out, out_len);
    if (st != ODS2_OK)
        return st;

    /* Validate the header's additive checksum before the caller trusts it. */
    return ods2_fh2_parse(header_out, ODS2_BLOCK_SIZE, &tmp);
}

/* ---- directory listing (block-backed) ---- */

/* Threads the block-backed volume + user callback through the map walk. */
struct bdev_dir_ctx {
    const ods2_bdev_t *bv;
    ods2_dir_cb        cb;
    void              *user;
    ods2_status_t      st;
    int                stop;
};

/* map-walk callback: read each extent's blocks and scan them as directory. */
static int bdev_dir_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    struct bdev_dir_ctx *c = (struct bdev_dir_ctx *)ctx;
    uint32_t k;

    for (k = 0; k < ext->count; k++) {
        uint8_t blk[ODS2_BLOCK_SIZE];
        ods2_status_t st = bdev_read_block(c->bv, ext->lbn + k,
                                           blk, sizeof(blk));
        if (st != ODS2_OK) {
            c->st = st;
            return 1;   /* stop the walk */
        }
        if (ods2_dir_block_scan(blk, c->cb, c->user)) {
            c->stop = 1;
            return 1;   /* user asked to stop */
        }
    }
    return 0;
}

ods2_status_t ods2_bdev_list_dir(const ods2_bdev_t *bv,
                                 const void *dir_header_block,
                                 ods2_dir_cb cb, void *ctx)
{
    struct bdev_dir_ctx c;
    ods2_fh2_t tmp;
    ods2_status_t st;

    if (!bv || !dir_header_block || !cb)
        return ODS2_ERR_ARGS;

    /* Validate the directory's header before trusting its map area. */
    st = ods2_fh2_parse(dir_header_block, ODS2_BLOCK_SIZE, &tmp);
    if (st != ODS2_OK)
        return st;

    c.bv   = bv;
    c.cb   = cb;
    c.user = ctx;
    c.st   = ODS2_OK;
    c.stop = 0;

    st = ods2_fh2_map_walk(dir_header_block, bdev_dir_extent_cb, &c, NULL);
    if (st != ODS2_OK)
        return st;
    return c.st;
}
