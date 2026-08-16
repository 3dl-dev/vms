/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_block_posix.c - USERSPACE realization of the ODS-2 block-access seam
 * (ods2_block.h). Whole-block pread()/pwrite() over a borrowed fd, in 64-bit
 * offsets, treating a short transfer as a hard failure -- the exact shape
 * ods2_bdev.c's bdev_pread_block() and ods2_writer.c's wcache pread/pwrite
 * used inline before the seam existed (which itself mirrors
 * src/vmsscs/scs_mscp_srv.c's raw-block server).
 *
 * Built ONLY in the userspace codec library (the kernel build compiles
 * ods2_block_kern.c instead). Rule 8: pure I/O plumbing, no on-disk fact.
 */

#define _POSIX_C_SOURCE 200809L

#include "ods2_block.h"
#include "ods2_kcompat.h"   /* memset */

#include <unistd.h>

ods2_status_t ods2_blk_read(ods2_blk_dev_t dev, uint32_t nblocks,
                            uint32_t lbn, void *buf, size_t buf_len)
{
    uint8_t *out = (uint8_t *)buf;
    off_t off;
    size_t want = ODS2_BLOCK_SIZE;
    size_t done = 0;

    if (dev < 0 || !buf)
        return ODS2_ERR_ARGS;
    if (buf_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (nblocks != 0 && lbn >= nblocks)
        return ODS2_ERR_RANGE;

    off = (off_t)lbn * (off_t)ODS2_BLOCK_SIZE;
    while (done < want) {
        ssize_t n = pread(dev, out + done, want - done, off + (off_t)done);
        if (n <= 0)
            return ODS2_ERR_IO;   /* short read == failure: whole blocks only */
        done += (size_t)n;
    }
    return ODS2_OK;
}

ods2_status_t ods2_blk_write(ods2_blk_dev_t dev, uint32_t nblocks,
                             uint32_t lbn, const void *buf, size_t buf_len)
{
    const uint8_t *in = (const uint8_t *)buf;
    off_t off;
    size_t want = ODS2_BLOCK_SIZE;
    size_t done = 0;

    if (dev < 0 || !buf)
        return ODS2_ERR_ARGS;
    if (buf_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (nblocks != 0 && lbn >= nblocks)
        return ODS2_ERR_RANGE;

    off = (off_t)lbn * (off_t)ODS2_BLOCK_SIZE;
    while (done < want) {
        ssize_t n = pwrite(dev, in + done, want - done, off + (off_t)done);
        if (n <= 0)
            return ODS2_ERR_IO;   /* short write == failure: whole blocks only */
        done += (size_t)n;
    }
    return ODS2_OK;
}

/*
 * fd-based reader open (the userspace tools' constructor). Auto-detects the
 * volume size when `span_bytes` is 0 via lseek(SEEK_END) (loop/image files); a
 * device that cannot report its size that way must pass span_bytes. The shared
 * tail (home-block read+validate) is ods2_bdev_finish_open().
 */
ods2_status_t ods2_bdev_open(ods2_bdev_t *bv, int fd, uint64_t span_bytes)
{
    if (!bv || fd < 0)
        return ODS2_ERR_ARGS;

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

    return ods2_bdev_finish_open(bv);
}
