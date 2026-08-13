/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_backend_netbsd.h - the NetBSD realization of the OVMX ODS-2 / vmsfs
 * kernel-backend shim (rd vms-308, epic vms-8e8; the ODS-2 filesystem analog of
 * the executive's exec_kbackend_netbsd.h, and the sibling of the Linux
 * realization src/kernel/vmsfs/vmsfs_backend_linux.h).
 *
 * DO NOT include this directly -- include "vmsfs_backend.h", which selects this
 * file on a NetBSD kernel build (OVMX_KBACKEND_NETBSD is defined by the NetBSD
 * kmodule build in src/kernel-netbsd/vmsfs/Makefile). See vmsfs_backend.h for
 * the seam-#1 vocabulary contract and vmsfs_bio.h for the seam-#2 block/inode
 * contract; this header realizes BOTH over PUBLIC, documented NetBSD kernel
 * APIs, so the SAME substrate-neutral ODS-2 core (the src/kernel-core/vmsfs
 * algorithm sources) compiles against it with no `#if' in the algorithm itself.
 *
 *   Seam #1 (vmsfs_backend.h vocabulary):
 *     uint8/16/32/64_t, size_t, bool       <- <sys/types.h> / <sys/stdint.h>
 *     strchr/strrchr/strlen/strncasecmp    <- libkern (via <sys/systm.h>)
 *       /memcpy/memcmp/memset
 *     strscpy                              -> strlcpy (truncating copy; the core
 *                                             uses only its copy effect, never
 *                                             the return value)
 *     isdigit/toupper                      <- libkern (via <sys/systm.h>)
 *     snprintf                             <- libkern (via <sys/systm.h>)
 *     VMSFS_EINVAL / ENAMETOOLONG /        == the NetBSD errno values so a core
 *       EIO / ENOSPC / ENOENT                 -VMSFS_E* return is the exact errno
 *                                             the NetBSD VFS glue expects.
 *
 *   Seam #2 (vmsfs_bio.h block/inode ops): the buffer/geometry/bitmap ops the
 *   storage allocator, directory-block scanner and file-header codec call, over
 *   NetBSD's buffer cache (bread(9)/brelse/bwrite/bdwrite), pure-C bit-array
 *   helpers, <sys/endian.h> byte order, and time_second.
 *
 * THE ONE STRUCTURAL DIFFERENCE FROM LINUX -- the deferred-disposition buffer
 * handle. Linux's mark_buffer_dirty() marks a held buffer dirty and leaves it
 * held, so its brelse() (vmsfs_bput) is a separate, unconditional release: the
 * core's contract is get -> data -> [dirty|dirty_sync] -> put, TWO independent
 * steps. NetBSD's write primitives are different: bwrite()/bdwrite() WRITE AND
 * RELEASE the buffer in one call, and there is no "mark dirty but keep busy,
 * release later" primitive -- in the BSD buffer model the write IS the release.
 * A naive mapping (vmsfs_bdirty_sync -> bwrite, then vmsfs_bput -> brelse) would
 * double-dispose the buffer. So `struct vmsfs_bh' here is NOT the bare buf: it
 * is a tiny wrapper that RECORDS the requested disposition (clean / delayed /
 * sync) at bdirty[_sync] time and performs the single correct terminal call
 * (brelse / bdwrite / bwrite) once, at bput time. The core is unchanged and
 * unaware; this is exactly the substrate difference the backend seam exists to
 * absorb (compare exec_kbackend_netbsd.h's size-tracking exec_alloc header and
 * its lazy mutex_init -- the executive's analogous BSD-vs-Linux glue). The
 * handle is heap-allocated per buffer (KM_SLEEP; these ops run only in VOP /
 * mount process context); it could later become a pool_cache with no core
 * change.
 *
 * Clean-room (CLAUDE.md Rule 8): these forwarders and helpers call only public,
 * documented NetBSD kernel APIs (buf(9), kmem(9), <sys/endian.h>, libkern) and
 * OVMX's own pure-C bit helpers. No NetBSD or VSI/HPE source is copied.
 */

#ifndef OVMX_VMSFS_BACKEND_NETBSD_H
#define OVMX_VMSFS_BACKEND_NETBSD_H

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stdint.h>    /* uint8/16/32/64_t, size_t */
#include <sys/systm.h>     /* memcpy, memcmp, memset, strlen, strchr, strrchr,
                            *  strncasecmp, strlcpy, snprintf, toupper, isdigit
                            *  (libkern, pulled in via <sys/systm.h>) */
#include <sys/endian.h>    /* le16toh/le32toh/le64toh, htole16/htole32/htole64 */
#include <sys/errno.h>     /* EINVAL, ENAMETOOLONG, EIO, ENOSPC, ENOENT */
#include <sys/time.h>      /* time_second */
#include <sys/kmem.h>      /* kmem_alloc / kmem_free, KM_SLEEP */
#include <sys/buf.h>       /* struct buf, bread, brelse, bwrite, bdwrite */
#include <sys/vnode.h>     /* struct vnode (the device vnode is vol->host) */

/* ---- normalized ODS-2-core error codes == the host errno values ---- */
#define VMSFS_EINVAL       EINVAL
#define VMSFS_ENAMETOOLONG ENAMETOOLONG
#define VMSFS_EIO          EIO
#define VMSFS_ENOSPC       ENOSPC
#define VMSFS_ENOENT       ENOENT

/*
 * strscpy: the core's bounded, always-NUL-terminating copy. NetBSD's strlcpy
 * has the identical truncating-copy effect (dst, src, size); the core discards
 * the return value (see vmsfs_header.c), so the differing return type is
 * irrelevant. Mapped as a statement-safe macro.
 */
#define strscpy(d, s, n) ((void)strlcpy((d), (s), (n)))

/* ================================================================
 * Seam #2 realization -- the block/inode (vmsfs_bio) vocabulary.
 * (contract in vmsfs_bio.h, design in src/kernel/vmsfs/README-backend.md §3.)
 * ================================================================ */

/*
 * The buffer handle. Unlike Linux (where struct vmsfs_bh IS struct buffer_head
 * and is never actually defined), NetBSD needs the deferred-disposition wrapper
 * described in the file header: it pairs the held buf with the disposition the
 * core requested, so vmsfs_bput() can issue the single correct terminal call.
 */
struct vmsfs_bh {
	struct buf *bp;
	int         disp;   /* 0 = clean, 1 = delayed write, 2 = synchronous write */
};

/* ODS-2 blocks are 512 bytes; the device is read a block at a time by LBN. */
#ifndef VMSFS_NB_BLOCK_SIZE
#define VMSFS_NB_BLOCK_SIZE 512
#endif

/*
 * vmsfs_bget - read the 512-byte volume block at @lbn into a held buffer and
 * wrap it. @host is struct vmsfs_volume's opaque handle; the vmsfs mount sets
 * it to the device vnode (devvp), so bread(9) reads directly from the volume.
 * Returns NULL on any I/O error (the core maps that to -VMSFS_EIO), exactly
 * like the Linux sb_bread() forwarder returning NULL.
 */
static __inline struct vmsfs_bh *
vmsfs_bget(void *host, uint32_t lbn)
{
	struct buf *bp = NULL;
	struct vmsfs_bh *w;
	int err;

	err = bread((struct vnode *)host, (daddr_t)lbn, VMSFS_NB_BLOCK_SIZE,
	    0, &bp);
	if (err != 0)
		return NULL;

	w = kmem_alloc(sizeof(*w), KM_SLEEP);
	w->bp = bp;
	w->disp = 0;
	return w;
}

static __inline void *
vmsfs_bdata(struct vmsfs_bh *w)
{
	return w->bp->b_data;
}

/* Mark for a delayed write-back (Linux: mark_buffer_dirty). */
static __inline void
vmsfs_bdirty(struct vmsfs_bh *w)
{
	w->disp = 1;
}

/* Mark for a synchronous write-back (Linux: mark_buffer_dirty + sync_dirty_buffer). */
static __inline void
vmsfs_bdirty_sync(struct vmsfs_bh *w)
{
	w->disp = 2;
}

/*
 * Release the buffer, honoring the recorded disposition with a SINGLE terminal
 * call (bwrite/bdwrite already release the buf; brelse releases a clean one),
 * then free the wrapper.
 */
static __inline void
vmsfs_bput(struct vmsfs_bh *w)
{
	if (w == NULL)
		return;
	switch (w->disp) {
	case 2:
		(void)bwrite(w->bp);   /* synchronous: write + release */
		break;
	case 1:
		bdwrite(w->bp);        /* delayed: write + release */
		break;
	default:
		brelse(w->bp, 0);      /* clean: release only */
		break;
	}
	kmem_free(w, sizeof(*w));
}

/* ================================================================
 * Storage-bitmap ops. The on-disk bitmap is a bit array cached in memory as an
 * `unsigned long' word array; the core passes only uint32_t bit indices. These
 * are OVMX's own pure-C helpers over the SAME LSB-first, little-endian-word bit
 * numbering the Linux bitops use, so a bitmap mastered by the Linux/mkimage
 * path is read identically here (amd64 is little-endian, matching the audit
 * backend's LE assumption). No NetBSD API and no NetBSD source involved.
 * ================================================================ */

#define VMSFS_NB_BPL ((uint32_t)(sizeof(unsigned long) * 8))

static __inline uint32_t
vmsfs_bit_find_zero(const unsigned long *bitmap, uint32_t nbits, uint32_t start)
{
	uint32_t b;

	for (b = start; b < nbits; b++) {
		if ((bitmap[b / VMSFS_NB_BPL] &
		    (1UL << (b % VMSFS_NB_BPL))) == 0)
			return b;
	}
	return nbits;
}

static __inline void
vmsfs_bit_set(unsigned long *bitmap, uint32_t bit)
{
	bitmap[bit / VMSFS_NB_BPL] |= (1UL << (bit % VMSFS_NB_BPL));
}

static __inline void
vmsfs_bit_clear(unsigned long *bitmap, uint32_t bit)
{
	bitmap[bit / VMSFS_NB_BPL] &= ~(1UL << (bit % VMSFS_NB_BPL));
}

static __inline int
vmsfs_bit_test(const unsigned long *bitmap, uint32_t bit)
{
	return (bitmap[bit / VMSFS_NB_BPL] & (1UL << (bit % VMSFS_NB_BPL))) != 0;
}

/* ---- little-endian on-disk field access (<sys/endian.h>) ---- */
static __inline uint16_t vmsfs_le16_to_cpu(uint16_t v) { return le16toh(v); }
static __inline uint32_t vmsfs_le32_to_cpu(uint32_t v) { return le32toh(v); }
static __inline uint64_t vmsfs_le64_to_cpu(uint64_t v) { return le64toh(v); }
static __inline uint16_t vmsfs_cpu_to_le16(uint16_t v) { return htole16(v); }
static __inline uint32_t vmsfs_cpu_to_le32(uint32_t v) { return htole32(v); }
static __inline uint64_t vmsfs_cpu_to_le64(uint64_t v) { return htole64(v); }

/*
 * Wall clock (Unix epoch seconds). WIDTH-CRITICAL: returns 64-bit (see
 * vmsfs_bio.h / the audit backend's note -- the value feeds vmsfs_cpu_to_le64
 * for the 64-bit on-disk timestamps). NetBSD's time_second is a time_t, 64-bit
 * on amd64.
 */
static __inline uint64_t
vmsfs_now_seconds(void)
{
	return (uint64_t)time_second;
}

#endif /* OVMX_VMSFS_BACKEND_NETBSD_H */
