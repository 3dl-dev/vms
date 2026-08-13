/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vmsfs_backend.h - the OVMX ODS-2 / vmsfs filesystem kernel-backend shim
 * (rd vms-544, epic vms-8e8; design record docs/design-netbsd-executive-core.md
 * -- this is the ODS-2 analog of the executive's exec_kbackend.h).
 *
 * This is the ONE header a substrate-agnostic ODS-2 core file includes for the
 * small vocabulary of host primitives the pure ODS-2 ALGORITHMS need -- the
 * fixed-width type vocabulary, the freestanding string/ctype/memory helpers,
 * and the normalized error codes the format-layer functions return. Its
 * concrete realization is selected at build time from a per-substrate backend
 * header, exactly as exec_kbackend.h does for the executive:
 *
 *   Linux   (src/kernel/vmsfs/vmsfs_backend_linux.h)   -> <linux/types.h>,
 *                                                         <linux/string.h>,
 *                                                         <linux/ctype.h>,
 *                                                         <linux/kernel.h>,
 *                                                         host errno values.
 *   NetBSD  (src/kernel-netbsd/vmsfs/vmsfs_backend_netbsd.h, later phase)
 *                                                      -> <sys/systm.h> libkern
 *                                                         string/ctype + errno.
 *
 * The point of the shim is that on Linux every name resolves to EXACTLY the
 * primitive the filesystem uses today, so promoting an ODS-2 algorithm onto it
 * is a behaviour-preserving refactor; the same algorithm source then compiles
 * against the NetBSD backend without a single `#if` in the algorithm itself.
 *
 * PHASE V1 SCOPE (this landing). vmsfs.ko's ON-DISK FORMAT (vmsfs_ondisk.h) was
 * already substrate-neutral, and the first pure ODS-2 ALGORITHMS extracted onto
 * this shim are:
 *
 *   - version resolution   (src/kernel-core/vmsfs/vmsfs_version.c): parse a VMS
 *     "NAME.TYPE;VERSION" filespec, build a versioned name, match a base name.
 *     Needs only string/ctype ops + the two name-parse error codes.
 *   - retrieval-map math   (src/kernel-core/vmsfs/vmsfs_map.c): VBN->LBN
 *     translation and contiguous map extension over a file's retrieval-pointer
 *     array. Needs only fixed-width types + two error codes (no host primitive
 *     at all -- pure arithmetic over the on-disk vmsfs_retrieval_ptr layout).
 *
 * The block-buffer + volume-geometry seam that the bitmap/cluster allocator and
 * the directory-block scanner need (the sb_bread/brelse/mark_buffer_dirty
 * equivalent and the storage-bitmap ops, today reached through struct
 * super_block / struct vmsfs_sb_info / struct buffer_head) is deliberately NOT
 * introduced here: those algorithms are still entangled with the Linux VFS
 * object model and are the subject of the next phase (V2 -- reduce the Linux
 * VFS backend to a thin inode/block shim), mirroring how the executive core
 * left its block-coupled facility (vms_devtab) in src/kernel until its seam
 * existed. Adding an op here before a core file needs it would be dead surface.
 *
 * Clean-room (CLAUDE.md Rule 8): this contract and the ODS-2 algorithms are
 * OVMX's own code; each backend maps it to PUBLIC, documented host kernel APIs
 * only. No Linux, NetBSD, or VSI/HPE source or binary is copied.
 *
 * ================================================================
 * THE VOCABULARY (contract; the backend header provides the concrete impl)
 * ================================================================
 *
 * Types (concrete per substrate; the backend pulls the host header that
 * defines them):
 *   uint8_t / uint16_t / uint32_t / uint64_t   fixed-width on-disk field types.
 *   size_t                                      buffer sizes.
 *   bool / true / false                         boolean results.
 *
 * String / ctype / memory (standard freestanding names; the backend guarantees
 * they are declared -- Linux via <linux/string.h>/<linux/ctype.h>, NetBSD via
 * libkern). A core ODS-2 file calls them by their ordinary names:
 *   strchr, strlen, strncasecmp, memcpy, snprintf, isdigit
 *
 * Error codes (normalized ODS-2-core spelling; the backend defines each to the
 * host's own errno value so the returned integer is identical on that host --
 * e.g. on Linux VMSFS_EINVAL == EINVAL, so a core function that returns
 * -VMSFS_EINVAL returns exactly the -EINVAL the Linux VFS caller expects):
 *   VMSFS_EINVAL        invalid argument / unparseable filespec
 *   VMSFS_ENAMETOOLONG  name does not fit the caller's buffer
 *   VMSFS_EIO           VBN not mapped by any retrieval pointer
 *   VMSFS_ENOSPC        retrieval-pointer array is full
 */

#ifndef OVMX_VMSFS_BACKEND_H
#define OVMX_VMSFS_BACKEND_H

/*
 * Backend selection -- identical scheme to exec_kbackend.h. Each substrate's
 * build defines its own macro (OVMX_KBACKEND_LINUX via src/kernel/vmsfs/Makefile
 * ccflags; OVMX_KBACKEND_NETBSD via the NetBSD kmodule build in a later phase).
 * __linux__/__KERNEL__ are accepted as a fallback so a stock
 * `make -C src/kernel/vmsfs` still resolves the Linux backend.
 */
#if defined(OVMX_KBACKEND_NETBSD)
#  include "vmsfs_backend_netbsd.h"
#elif defined(OVMX_KBACKEND_LINUX) || defined(__linux__) || defined(__KERNEL__)
#  include "vmsfs_backend_linux.h"
#else
#  error "vmsfs_backend.h: no kernel backend selected (define OVMX_KBACKEND_LINUX or OVMX_KBACKEND_NETBSD)"
#endif

#endif /* OVMX_VMSFS_BACKEND_H */
