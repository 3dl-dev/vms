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
 * Clean-room (Rule 8): both realizations name only public, documented APIs
 * (libc, or the Linux kernel's own public headers). No VSI/HPE/Linux source is
 * copied.
 */

#ifndef OVMX_VMSFS_ODS2_KCOMPAT_H
#define OVMX_VMSFS_ODS2_KCOMPAT_H

#ifdef OVMX_ODS2_KERNEL

/* ---- kernel-resident vocabulary ---- */
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
