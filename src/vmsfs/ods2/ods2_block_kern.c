// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * ods2_block_kern.c - KERNEL-RESIDENT realization of the ODS-2 block-access
 * seam (ods2_block.h), rd vms-dcd, epic vms-208 (Files-11 ODS-2 ACP in the
 * executive).
 *
 * Binds the genuine ODS-2 codec's block access to the shared FS engine's
 * vmsfs_bio.h backend -- vmsfs_bget / vmsfs_bdata / vmsfs_bdirty_sync /
 * vmsfs_bput -- so the codec reads/writes a real struct block_device with NO
 * libc and NO POSIX. On Linux those ops forward to sb_bread / mark_buffer_dirty
 * + sync_dirty_buffer / brelse (vmsfs_backend_linux.h); the ACP mount sets the
 * volume's block size to ODS2_BLOCK_SIZE (512), so one vmsfs_bget() is exactly
 * one Files-11 logical block. `dev` is the opaque host handle vmsfs_bget takes
 * (a struct super_block *), threaded through unchanged.
 *
 * Built ONLY in the kernel FS engine (the userspace codec compiles
 * ods2_block_posix.c instead). Rule 8: pure I/O plumbing -- it changes only
 * WHERE a block's bytes come from, never a single on-disk ODS-2 byte.
 */

#include "ods2_block.h"
#include "ods2_kcompat.h"   /* memset, memcpy */

#include "vmsfs_bio.h"   /* struct vmsfs_bh, vmsfs_bget/bdata/bdirty_sync/bput */

ods2_status_t ods2_blk_read(ods2_blk_dev_t dev, uint32_t nblocks,
                            uint32_t lbn, void *buf, size_t buf_len)
{
    struct vmsfs_bh *bh;

    if (!dev || !buf)
        return ODS2_ERR_ARGS;
    if (buf_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (nblocks != 0 && lbn >= nblocks)
        return ODS2_ERR_RANGE;

    bh = vmsfs_bget(dev, lbn);
    if (!bh)
        return ODS2_ERR_IO;
    memcpy(buf, vmsfs_bdata(bh), ODS2_BLOCK_SIZE);
    vmsfs_bput(bh);
    return ODS2_OK;
}

ods2_status_t ods2_blk_write(ods2_blk_dev_t dev, uint32_t nblocks,
                             uint32_t lbn, const void *buf, size_t buf_len)
{
    struct vmsfs_bh *bh;

    if (!dev || !buf)
        return ODS2_ERR_ARGS;
    if (buf_len < ODS2_BLOCK_SIZE)
        return ODS2_ERR_SIZE;
    if (nblocks != 0 && lbn >= nblocks)
        return ODS2_ERR_RANGE;

    bh = vmsfs_bget(dev, lbn);
    if (!bh)
        return ODS2_ERR_IO;
    memcpy(vmsfs_bdata(bh), buf, ODS2_BLOCK_SIZE);
    vmsfs_bdirty_sync(bh);
    vmsfs_bput(bh);
    return ODS2_OK;
}

/*
 * KERNEL-RESIDENT reader open: bind an ods2_bdev_t to a mounted volume. `host`
 * is the vmsfs_bio backend handle (super_block, block size already 512);
 * `nblocks` is the volume size in 512-byte blocks (no lseek in the kernel).
 * Shared tail (home-block read+validate) is ods2_bdev_finish_open().
 */
ods2_status_t ods2_bdev_open_host(ods2_bdev_t *bv, void *host, uint32_t nblocks)
{
    if (!bv || !host)
        return ODS2_ERR_ARGS;
    if (nblocks < 2u)
        return ODS2_ERR_SIZE;

    memset(bv, 0, sizeof(*bv));
    bv->host    = host;
    bv->nblocks = nblocks;

    return ods2_bdev_finish_open(bv);
}
