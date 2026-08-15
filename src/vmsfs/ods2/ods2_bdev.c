/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_bdev.c - Block-device-backed variant of the GENUINE ODS-2 reader.
 *
 * vms-6cb (R1 of the real-ODS-2-runtime epic vms-5eb): the genuine ODS-2
 * reader (ods2_reader.c) parses byte-real Files-11 level 2 structures, but
 * only over a whole-volume in-memory `const uint8_t *image`. This file adds a
 * block-BACKED twin -- the SAME parse/validate/walk logic, but blocks are
 * fetched on demand via pread() over a backing fd (a loop/disk image file, or
 * a block/character device such as /dev/vms) instead of a resident buffer.
 * This is the FOUNDATION rung that lets later rungs (R2..R6) wire the genuine
 * reader into the live RMS/DCL/MOUNT path over real storage.
 *
 * ADDITIVE: the in-memory ods2_volume_t path (ods2_volume_open/_block/
 * _read_header/_list_dir) is unchanged; both variants coexist. This file
 * reuses the pure, I/O-free reader primitives (ods2_home_parse,
 * ods2_fh2_parse, ods2_fh2_map_walk, ods2_dir_block_scan) verbatim -- it adds
 * NO new ODS-2 format facts beyond what vmsfs/ods2.h already cites from its
 * Files-11 sources (Rule 8); it only changes WHERE a 512-byte block comes
 * from.
 *
 * The raw-block pread shape deliberately mirrors src/vmsscs/scs_mscp_srv.c's
 * scs_mscp_srv_read_blocks(): whole-block reads, a short read is a hard
 * failure (not a partial success), and the byte offset is computed in 64
 * bits. Kept in a separate translation unit so ods2_reader.c stays free of
 * any POSIX-I/O dependency (it remains usable in an I/O-free context); the
 * ods2 static library only pulls this object in when a caller references an
 * ods2_bdev_* symbol.
 */

/* pread / lseek / off_t / SEEK_END -- same feature-test macro
 * src/vmsscs/scs_mscp_srv.c uses, so this TU is self-sufficient regardless of
 * the compiler's default -std extensions. */
#define _POSIX_C_SOURCE 200809L

#include "vmsfs/ods2.h"

#include <string.h>
#include <unistd.h>

/*
 * Read exactly one 512-byte block at `lbn` into `buf` via pread. Mirrors
 * scs_mscp_srv_read_blocks(): loops until the whole block is read, treats a
 * <=0 return as a hard failure, and computes the offset in 64 bits.
 */
static ods2_status_t bdev_pread_block(const ods2_bdev_t *bv, uint32_t lbn,
                                      void *buf, size_t buf_len)
{
    uint8_t *out = (uint8_t *)buf;
    off_t off;
    size_t want = ODS2_BLOCK_SIZE;
    size_t done = 0;

    if (!bv || bv->fd < 0 || !buf)
        return ODS2_ERR_ARGS;
    if (buf_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (lbn >= bv->nblocks)
        return ODS2_ERR_RANGE;

    off = (off_t)lbn * (off_t)ODS2_BLOCK_SIZE;
    while (done < want) {
        ssize_t n = pread(bv->fd, out + done, want - done, off + (off_t)done);
        if (n <= 0)
            return ODS2_ERR_IO;   /* short read == failure: whole blocks only */
        done += (size_t)n;
    }
    return ODS2_OK;
}

ods2_status_t ods2_bdev_read_block(const ods2_bdev_t *bv, uint32_t lbn,
                                   void *buf, size_t buf_len)
{
    return bdev_pread_block(bv, lbn, buf, buf_len);
}

ods2_status_t ods2_bdev_open(ods2_bdev_t *bv, int fd, uint64_t span_bytes)
{
    uint8_t home_blk[ODS2_BLOCK_SIZE];
    ods2_status_t st;

    if (!bv || fd < 0)
        return ODS2_ERR_ARGS;

    /* Auto-detect the volume size when the caller passes 0: seek to the end
     * (works for regular / loop image files). A device that cannot report its
     * size this way must pass span_bytes explicitly. */
    if (span_bytes == 0) {
        off_t end = lseek(fd, 0, SEEK_END);
        if (end < 0)
            return ODS2_ERR_IO;
        span_bytes = (uint64_t)end;
    }
    if (span_bytes < 2u * ODS2_BLOCK_SIZE)   /* need at least boot + home */
        return ODS2_ERR_SIZE;

    memset(bv, 0, sizeof(*bv));
    bv->fd      = fd;
    bv->nblocks = (uint32_t)(span_bytes / ODS2_BLOCK_SIZE);

    /*
     * Home block lives at LBN 1 (block 0 is the boot block), same as the
     * in-memory ods2_volume_open(). The parse validates BOTH additive
     * checksums + the "DECFILE11B  " format string + (strict) structure
     * level, so a non-ODS-2 fd is rejected rather than silently accepted.
     */
    st = bdev_pread_block(bv, 1, home_blk, sizeof(home_blk));
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

    st = bdev_pread_block(bv, hdr_lbn, header_out, out_len);
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

/* map-walk callback: pread each extent's blocks and scan them as directory. */
static int bdev_dir_extent_cb(const ods2_extent_t *ext, void *ctx)
{
    struct bdev_dir_ctx *c = (struct bdev_dir_ctx *)ctx;
    uint32_t k;

    for (k = 0; k < ext->count; k++) {
        uint8_t blk[ODS2_BLOCK_SIZE];
        ods2_status_t st = bdev_pread_block(c->bv, ext->lbn + k,
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
