/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_block.h - the GENUINE ODS-2 codec's BLOCK-ACCESS seam (rd vms-dcd,
 * epic vms-208). The one place a 512-byte volume block comes from, abstracted
 * so the identical codec source runs BOTH:
 *
 *   - USERSPACE: reads/writes a backing fd (a loop/disk image, /dev/vms, ...)
 *     via pread()/pwrite() -- realized in ods2_block_posix.c. This is the
 *     block access ods2_bdev.c and ods2_writer.c used inline before this seam.
 *
 *   - KERNEL-RESIDENT (-DOVMX_ODS2_KERNEL): reads/writes a real struct
 *     block_device THROUGH the shared FS engine's vmsfs_bio.h backend
 *     (vmsfs_bget / vmsfs_bdata / vmsfs_bdirty / vmsfs_bput -- on Linux
 *     trivial forwarders to sb_bread / mark_buffer_dirty / brelse) -- realized
 *     in ods2_block_kern.c. `dev` is then the opaque host handle vmsfs_bget
 *     takes (a struct super_block *), never a fd.
 *
 * Rule 8: this is pure I/O plumbing. It changes only WHERE a block's bytes
 * come from, never a single on-disk ODS-2 byte.
 */

#ifndef OVMX_VMSFS_ODS2_BLOCK_H
#define OVMX_VMSFS_ODS2_BLOCK_H

#include "vmsfs/ods2.h"   /* ods2_status_t, ODS2_BLOCK_SIZE, fixed-width types */

#ifdef OVMX_ODS2_KERNEL
/* Opaque backing device: the host handle vmsfs_bget() takes (super_block*). */
typedef void *ods2_blk_dev_t;
#define ODS2_BDEV_DEV(bv)   ((bv)->host)
#define ODS2_WVOL_DEV(wv)   ((wv)->host)
#else
/* Backing fd (borrowed), the userspace tools' block source. */
typedef int   ods2_blk_dev_t;
#define ODS2_BDEV_DEV(bv)   ((bv)->fd)
#define ODS2_WVOL_DEV(wv)   ((wv)->bdev_fd)
#endif

/*
 * Read / write exactly ONE 512-byte block at `lbn`. `nblocks` bounds the
 * device (0 == "unknown, skip the range check"); an lbn at/after it is
 * ODS2_ERR_RANGE. A short/failed transfer is a hard ODS2_ERR_IO (whole blocks
 * only) -- the same contract the inline pread/pwrite loops enforced.
 */
ods2_status_t ods2_blk_read(ods2_blk_dev_t dev, uint32_t nblocks,
                            uint32_t lbn, void *buf, size_t buf_len);
ods2_status_t ods2_blk_write(ods2_blk_dev_t dev, uint32_t nblocks,
                             uint32_t lbn, const void *buf, size_t buf_len);

/*
 * Backend-neutral finish of an ods2_bdev_t open: `bv->nblocks` and its backing
 * device field (fd / host) are already set; read + validate the home block at
 * LBN 1 into bv->home (the shared tail of ods2_bdev_open() and
 * ods2_bdev_open_host()). Defined in ods2_bdev.c.
 */
ods2_status_t ods2_bdev_finish_open(ods2_bdev_t *bv);

#endif /* OVMX_VMSFS_ODS2_BLOCK_H */
