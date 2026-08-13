/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_backend_linux.h - the LINUX realization of the OVMX ODS-2 / vmsfs
 * kernel-backend shim (rd vms-544, epic vms-8e8).
 *
 * DO NOT include this directly -- include "vmsfs_backend.h", which selects this
 * file on a Linux kernel build. See that header for the vocabulary contract.
 *
 * Every name a core ODS-2 file uses resolves here to the EXACT primitive the
 * filesystem already used, so an algorithm moved onto the shim compiles to
 * behaviour-identical code:
 *
 *   uint8/16/32/64_t, size_t, bool    <- <linux/types.h>
 *   strchr/strrchr/strlen/strncasecmp <- <linux/string.h>
 *     /memcpy/memcmp/strscpy
 *   isdigit/toupper                   <- <linux/ctype.h>
 *   snprintf                          <- <linux/kernel.h>
 *   VMSFS_EINVAL / ENAMETOOLONG /     == the Linux errno values EINVAL /
 *     EIO / ENOSPC                       ENAMETOOLONG / EIO / ENOSPC
 *
 * Because each VMSFS_E* is defined to the host errno, a core function that
 * returns -VMSFS_EINVAL returns exactly the -EINVAL the Linux VFS glue in
 * src/kernel/vmsfs/ expects -- the extraction is byte-value-identical on the
 * return path.
 *
 * Clean-room (CLAUDE.md Rule 8): these mappings name only public, documented
 * Linux kernel APIs. No code is copied from the Linux source.
 */

#ifndef OVMX_VMSFS_BACKEND_LINUX_H
#define OVMX_VMSFS_BACKEND_LINUX_H

#include <linux/types.h>    /* uint8/16/32/64_t, size_t, bool */
#include <linux/string.h>   /* strchr, strrchr, strlen, strncasecmp, memcpy, memcmp, strscpy */
#include <linux/ctype.h>    /* isdigit, toupper */
#include <linux/kernel.h>   /* snprintf */
#include <linux/errno.h>    /* EINVAL, ENAMETOOLONG, EIO, ENOSPC, ENOENT */
/* ---- seam #2 (block/inode) backing headers (rd vms-d69) ---- */
#include <linux/fs.h>          /* struct super_block, byteorder (le*_to_cpu) */
#include <linux/buffer_head.h> /* struct buffer_head, sb_bread, mark_buffer_dirty,
                                *  sync_dirty_buffer, brelse */
#include <linux/bitops.h>      /* find_next_zero_bit, set_bit, clear_bit, test_bit */
#include <linux/ktime.h>       /* ktime_get_real_seconds */

/* ---- normalized ODS-2-core error codes == the host errno values ---- */
#define VMSFS_EINVAL       EINVAL
#define VMSFS_ENAMETOOLONG ENAMETOOLONG
#define VMSFS_EIO          EIO
#define VMSFS_ENOSPC       ENOSPC
#define VMSFS_ENOENT       ENOENT

/* ================================================================
 * Seam #2 realization — the block/inode (vmsfs_bio) vocabulary
 * (rd vms-d69, epic vms-8e8; contract in vmsfs_backend.h, design in
 * src/kernel/vmsfs/README-backend.md §3).
 *
 * Each op is a TRIVIAL FORWARDER to the exact Linux primitive the
 * block-device ODS-2 code already used, so the storage allocator, the
 * directory-block scanner and the file-header decode/encode compile to
 * behaviour-identical code once promoted into the substrate-neutral core:
 *
 *   struct vmsfs_bh          == struct buffer_head   (opaque to the core)
 *   vmsfs_bget(host,lbn)     -> sb_bread((struct super_block *)host, lbn)
 *   vmsfs_bdata(bh)          -> bh->b_data
 *   vmsfs_bdirty(bh)         -> mark_buffer_dirty(bh)
 *   vmsfs_bdirty_sync(bh)    -> mark_buffer_dirty(bh) + sync_dirty_buffer(bh)
 *   vmsfs_bput(bh)           -> brelse(bh)
 *   vmsfs_bit_find_zero/set/clear/test -> find_next_zero_bit / set_bit /
 *                                         clear_bit / test_bit
 *   vmsfs_le16/32/64_to_cpu / vmsfs_cpu_to_le16/32/64 -> le*_to_cpu / cpu_to_le*
 *   vmsfs_now_seconds()      -> ktime_get_real_seconds()
 *
 * The buffer handle is opaque: the core only ever holds a `struct vmsfs_bh *`
 * and passes it back to these ops (the exec-core `exec_task_ref_t` pattern).
 * The volume descriptor `struct vmsfs_volume` (defined in the core header
 * vmsfs_bio.h) is host-neutral; its `host` field carries the super_block, so
 * the ops take a plain `void *host` and never name `struct vmsfs_volume`.
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders call only public, documented
 * Linux kernel APIs. No code is copied from the Linux source.
 * ================================================================ */

struct vmsfs_bh;   /* opaque buffer handle (Linux: struct buffer_head) */

static inline struct vmsfs_bh *vmsfs_bget(void *host, uint32_t lbn)
{
	return (struct vmsfs_bh *)sb_bread((struct super_block *)host, lbn);
}

static inline void *vmsfs_bdata(struct vmsfs_bh *bh)
{
	return ((struct buffer_head *)bh)->b_data;
}

static inline void vmsfs_bdirty(struct vmsfs_bh *bh)
{
	mark_buffer_dirty((struct buffer_head *)bh);
}

static inline void vmsfs_bdirty_sync(struct vmsfs_bh *bh)
{
	struct buffer_head *b = (struct buffer_head *)bh;

	mark_buffer_dirty(b);
	sync_dirty_buffer(b);
}

static inline void vmsfs_bput(struct vmsfs_bh *bh)
{
	brelse((struct buffer_head *)bh);
}

/* ---- storage-bitmap ops (one bit per volume block) ---- */
static inline uint32_t vmsfs_bit_find_zero(const unsigned long *bitmap,
					   uint32_t nbits, uint32_t start)
{
	return (uint32_t)find_next_zero_bit(bitmap, nbits, start);
}

static inline void vmsfs_bit_set(unsigned long *bitmap, uint32_t bit)
{
	set_bit(bit, bitmap);
}

static inline void vmsfs_bit_clear(unsigned long *bitmap, uint32_t bit)
{
	clear_bit(bit, bitmap);
}

static inline int vmsfs_bit_test(const unsigned long *bitmap, uint32_t bit)
{
	return test_bit(bit, bitmap);
}

/* ---- little-endian on-disk field access ---- */
static inline uint16_t vmsfs_le16_to_cpu(uint16_t v) { return le16_to_cpu(v); }
static inline uint32_t vmsfs_le32_to_cpu(uint32_t v) { return le32_to_cpu(v); }
static inline uint64_t vmsfs_le64_to_cpu(uint64_t v) { return le64_to_cpu(v); }
static inline uint16_t vmsfs_cpu_to_le16(uint16_t v) { return cpu_to_le16(v); }
static inline uint32_t vmsfs_cpu_to_le32(uint32_t v) { return cpu_to_le32(v); }
static inline uint64_t vmsfs_cpu_to_le64(uint64_t v) { return cpu_to_le64(v); }

/* ---- wall clock (Unix epoch seconds) ---- */
static inline uint64_t vmsfs_now_seconds(void)
{
	return (uint64_t)ktime_get_real_seconds();
}

#endif /* OVMX_VMSFS_BACKEND_LINUX_H */
