/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ods2_kcompat.h - dual-world (userspace / kernel-resident) compatibility shim
 * for the GENUINE ODS-2 codec (src/vmsfs/ods2/), rd vms-dcd, epic vms-208
 * (Files-11 ODS-2 ACP in the executive).
 *
 * The codec is a SINGLE source of truth compiled BOTH ways:
 *
 *   - USERSPACE (default): the INITIALIZE.EXE / vmsfs_master --ods2 tools and
 *     the codec's own host ctest. Resolves to libc <string.h>/<stdlib.h>/
 *     <ctype.h>/<stdio.h> and plain malloc/free.
 *
 *   - KERNEL-RESIDENT (-DOVMX_ODS2_KERNEL): compiled into the shared kernel FS
 *     engine (the Linux vmsfs.ko), where the ODS-2 ACP reads/writes a genuine
 *     Files-11 volume off a real struct block_device with NO libc and NO POSIX.
 *     Resolves the same vocabulary to the kernel's freestanding string/ctype/
 *     printf and to the kernel allocator (kvmalloc/kvfree, which transparently
 *     handle both the small and the ~2 MB write-cache allocations the writer
 *     makes without a contiguous-page cap).
 *
 * This header changes only WHERE a byte comes from and WHERE memory is
 * allocated -- never the on-disk ODS-2 layout (CLAUDE.md Rule 8). The
 * block-access seam (pread/pwrite vs vmsfs_bio bget/bput/bdirty) is a separate
 * header, ods2_block.h.
 *
 * SUBSTRATE DISPATCH (rd vms-02b). Kernel-resident has TWO realizations,
 * selected the SAME way src/kernel-core/vmsfs/vmsfs_backend.h dispatches its
 * sibling shim (Linux vmsfs_backend_linux.h vs NetBSD vmsfs_backend_netbsd.h):
 *
 *   - Linux (default, or OVMX_KBACKEND_LINUX/__linux__/__KERNEL__): the shared
 *     kernel FS engine (vmsfs.ko). The linux headers below plus kvmalloc/
 *     kvfree, as above.
 *
 *   - NetBSD (OVMX_KBACKEND_NETBSD, e.g. the OVMX/NetBSD-VAX executive's ACP
 *     compile leg, tools/cross-vax/build-vms-module-vax.sh): the SAME
 *     vocabulary via <sys/systm.h> libkern (memcpy/memset/memmove/memcmp/
 *     strlen/strchr/strcmp/toupper/isdigit/snprintf), exactly as
 *     src/kernel-netbsd/vmsfs/vmsfs_backend_netbsd.h already realizes for the
 *     substrate-neutral vmsfs core. The allocator seam maps to the NetBSD
 *     kernel allocator malloc(9)/free(9) (<sys/malloc.h>'s kern_malloc/
 *     kern_free) -- NOT kmem_alloc/kmem_free, whose kmem_free(p, size) needs
 *     the allocation size back at free time; every ods2_kfree call site in the
 *     codec (ods2_path.c, ods2_writer.c) is single-argument, so the NetBSD
 *     realization must free from the pointer alone exactly as Linux kvfree()
 *     and userspace free() do. kern_free(void *) does that (the size is
 *     tracked internally by the allocator, BSD malloc(9)-style); kern_malloc's
 *     M_ZERO flag gives kzalloc's zeroing without a separate memset.
 *
 * Clean-room (Rule 8): all three realizations name only public, documented
 * APIs (libc, or the Linux/NetBSD kernel's own public headers). No VSI/HPE/
 * Linux/NetBSD source is copied.
 */

#ifndef OVMX_VMSFS_ODS2_KCOMPAT_H
#define OVMX_VMSFS_ODS2_KCOMPAT_H

#ifdef OVMX_ODS2_KERNEL

#if defined(OVMX_KBACKEND_NETBSD)

/* ---- kernel-resident vocabulary: NetBSD ---- */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stdint.h>     /* uint8/16/32/64_t, size_t */
#include <sys/systm.h>      /* memcpy, memset, memmove, memcmp, strlen, strchr,
                             *  strcmp, toupper, isdigit, snprintf (libkern) */
#include <sys/malloc.h>     /* kern_malloc / kern_free (malloc(9)/free(9)) */

/*
 * The codec's heap seam (NetBSD realization). kern_malloc/kern_free (the
 * malloc(9)/free(9) macros' underlying KPI) free from the pointer alone, like
 * Linux kvfree() and userspace free() -- see the file-header note on why this
 * is kmem_alloc/kmem_free-incompatible for this seam. M_ZERO gives the
 * zeroing ods2_kzalloc needs without a separate memset.
 */
static inline void *ods2_kalloc(size_t n)        { return kern_malloc((unsigned long)n, M_WAITOK); }
static inline void *ods2_kzalloc(size_t n)       { return kern_malloc((unsigned long)n, M_WAITOK | M_ZERO); }
static inline void  ods2_kfree(void *p)          { kern_free(p); }

#else  /* !OVMX_KBACKEND_NETBSD -- Linux (default kernel realization) */

/* ---- kernel-resident vocabulary: Linux ---- */
#include <linux/types.h>    /* uint8/16/32/64_t, size_t, bool */
#include <linux/string.h>   /* memcpy, memset, memmove, memcmp, strlen, strchr, strcmp */
#include <linux/ctype.h>    /* toupper, isdigit */
#include <linux/kernel.h>   /* snprintf */
#include <linux/slab.h>     /* kvmalloc, kvfree */

/*
 * The codec's heap seam. Userspace malloc/calloc/free map to these three so
 * the codec source names no allocator directly. In the kernel kvmalloc()/
 * kvzalloc() fall back from kmalloc to vmalloc automatically, so both the tiny
 * cache-header allocation and the writer's ~2 MB block cache / directory
 * flatten buffer succeed without a physically-contiguous requirement.
 */
static inline void *ods2_kalloc(size_t n)        { return kvmalloc(n, GFP_KERNEL); }
static inline void *ods2_kzalloc(size_t n)       { return kvzalloc(n, GFP_KERNEL); }
static inline void  ods2_kfree(void *p)          { kvfree(p); }

#endif /* OVMX_KBACKEND_NETBSD */

#else  /* !OVMX_ODS2_KERNEL -- userspace */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static inline void *ods2_kalloc(size_t n)        { return malloc(n); }
static inline void *ods2_kzalloc(size_t n)       { return calloc(1, n); }
static inline void  ods2_kfree(void *p)          { free(p); }

#endif /* OVMX_ODS2_KERNEL */

#endif /* OVMX_VMSFS_ODS2_KCOMPAT_H */
