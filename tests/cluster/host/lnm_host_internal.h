/* SPDX-License-Identifier: GPL-2.0 */
/*
 * lnm_host_internal.h - the HOST stand-in for src/kernel/vms_internal.h,
 * scoped to exactly what src/kernel-core/vms_lnm.c needs to compile (rd
 * vms-94a). Reached only through lnm_shim/vms_internal.h, which is FIRST on the
 * host LNM target's include path; the mechanism and the reasoning are
 * lock_host_internal.h's (the same one vms_lock.c's host bed uses).
 *
 * vms_lnm.c is compiled UNCHANGED. Every type below has the field names and
 * widths src/kernel/vms_internal.h gives it; the portable pieces (the arena,
 * the ioctl argument structs, PSL_C_*, VMS_PRV_M_*) come from the real
 * src/kernel/vms_ioctl.h, which already builds in plain userspace.
 */
#ifndef OVMX_LNM_HOST_INTERNAL_H
#define OVMX_LNM_HOST_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "vms_ioctl.h"            /* vms_lnm.h, PSL_C_*, VMS_PRV_M_* */
#include "exec_kbackend.h"        /* -> lnm_shim -> exec_kbackend_host.h */

#define SS__NORMAL      0x00000001
#define SS__BADPARAM    0x00000014
#define SS__NOPRIV      36
#define SS__IVLOGNAM    340
#define SS__SUPERSEDE   1585
#define SS__NOLOGNAM    444
#define SS__NOLOGTAB    8852
#define SS__EXLNMQUOTA  8780
#define SS__UNSUPPORTED 3658

/* The PCB fields vms_lnm.c reads (src/kernel/vms_internal.h names/widths). */
struct vms_proc {
	uint32_t vms_pid;
	uint32_t uic;
	uint32_t job_id;
	uint8_t  current_mode;
	uint64_t cur_privs;
};

#define pr_info(...) ((void)0)

/* sec 9/10 of exec_kbackend.h, which the shared host backend does not carry. */
static inline void exec_membar_producer(void) { __sync_synchronize(); }
static inline void *exec_arena_alloc(size_t n) { return calloc(1, n ? n : 1); }
static inline void exec_arena_free(void *p) { free(p); }

int  vms_lnm_init(void);
long vms_ioctl_lnm_define(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_lnm_delete(struct vms_proc *proc, unsigned long arg);
void vms_lnm_cleanup(void);
void *vms_lnm_arena_base(void);
int vms_lnm_cluster_init(void);
uint32_t vms_lnm_cluster_other_members(void);   /* vms_devtab.c */
uint32_t vms_lnm_cluster_crelnt(const char *tab, const char *parent,
                                uint32_t attr, uint8_t acmode, uint32_t promsk);
uint32_t vms_lnm_cluster_crelnm(const char *tab, const char *name,
                                uint32_t attr, uint8_t acmode, uint8_t nequiv,
                                const struct vms_lnm_equiv *eq);
uint32_t vms_lnm_cluster_dellnm(const char *tab, const char *name,
                                uint8_t acmode);

#endif
