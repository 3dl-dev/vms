/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_internal.h (NetBSD substrate twin) - the shared-struct header the
 * substrate-agnostic executive core includes on a NetBSD build (rd vms-4b4,
 * epic vms-8e8; docs/design-netbsd-executive-core.md §4).
 *
 * The Linux executive keeps its shared kernel structs in src/kernel/vms_internal.h,
 * a large header that pulls <linux/*> and carries the WHOLE executive (proc
 * table, ASTs, locks, mailboxes, ...). The relocated facility
 * src/kernel-core/vms_eflag.c includes "vms_internal.h" with NO substrate in its
 * name, and on Linux the build's -I resolves that to src/kernel's copy.
 *
 * On the NetBSD build there is NO -I into src/kernel (that header would not
 * compile in a NetBSD kernel TU), so the SAME `#include "vms_internal.h"'
 * resolves HERE via -I. (src/kernel-netbsd). This twin therefore provides EXACTLY
 * the slice the event-flag facility touches -- the event-flag structs, the SS$
 * status codes it returns, the arg structs it copies, and the few Linux-kernel
 * spellings it uses (strscpy, READ_ONCE, ERESTARTSYS) -- expressed in the
 * substrate-agnostic exec_*/exec_list_* vocabulary, so the SAME vms_eflag.c
 * source compiles against it without a single `#if'. This is the per-substrate
 * "glue" the design record calls out (§4): the facility is shared; the struct
 * header and the char-device/module lifecycle are backend-specific.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own structs and constants; the constants
 * are grounded to the in-tree source of truth (src/kernel/vms_internal.h /
 * src/libvms/include/ssdef.h), not to any VSI header.
 */

#ifndef _VMS_INTERNAL_H
#define _VMS_INTERNAL_H

#include <sys/types.h>
#include <sys/systm.h>     /* memset, strncmp, strlcpy, printf */
#include <sys/errno.h>     /* EFAULT */

/* The shim contracts. Selected to their NetBSD backends by OVMX_KBACKEND_NETBSD
 * (set by the kmodule build). These give exec_lock_t / exec_cv_t and the
 * exec_list_* container types the structs below embed. */
#include "exec_kbackend.h"
#include "exec_list.h"

/* The event-flag argument structs the facility copies in/out. Byte-identical to
 * src/kernel/vms_ioctl.h; shared with the userspace tool. */
#include "vms_eflag_nb.h"

/* ================================================================
 * VMS status codes -- the subset the event-flag facility returns. Values match
 * src/kernel/vms_internal.h exactly (ORACLE-PINNED there, vms-2a8/vms-8019).
 * ================================================================ */
#define SS__NORMAL   0x00000001    /* SS$_NORMAL */
#define SS__WASCLR   1             /* SS$_WASCLR (== SS$_NORMAL on VMS) */
#define SS__WASSET   9             /* SS$_WASSET */
#define SS__INSFMEM  292           /* SS$_INSFMEM */
#define SS__ILLEFC   236           /* SS$_ILLEFC */
#define SS__UNASEFC  564           /* SS$_UNASEFC */

/* ================================================================
 * Linux-kernel spellings the shared facility uses verbatim, provided for the
 * NetBSD kernel environment so vms_eflag.c needs no #if.
 * ================================================================ */

/* The facility returns -ERESTARTSYS on the (unreachable-by-design) interrupted
 * wait, writing NO status; the NetBSD driver maps that return to ERESTART so the
 * ioctl syscall restarts and re-enters the wait (the same "re-enter" effect
 * libvmssys produces on Linux). The value is a private sentinel, distinct from
 * every NetBSD errno, so the driver can recognise it. */
#ifndef ERESTARTSYS
#define ERESTARTSYS  512
#endif

/* strscpy (Linux) -> strlcpy (NetBSD/libkern). Both size-bounded, both NUL-
 * terminate; the facility ignores the return value. */
#define strscpy(dst, src, n)  strlcpy((dst), (src), (n))

/* READ_ONCE (Linux) -- a single volatile-qualified load, so the compiler does
 * not hoist/duplicate the predicate read in a wait loop. */
#ifndef READ_ONCE
#define READ_ONCE(x)  (*(volatile __typeof__(x) *)&(x))
#endif

/* ================================================================
 * Event-flag structures -- the exact fields src/kernel-core/vms_eflag.c uses,
 * in the exec_*/exec_list_* vocabulary. Compare src/kernel/vms_internal.h's
 * Linux-typed versions (spinlock_t/wait_queue_head_t/struct list_head); the
 * facility names only exec_* / exec_list_* so both compile it unchanged.
 * ================================================================ */

/* Common event flag cluster (shared between processes). Lives in the module's
 * global vms_common_ef_list, in kernel memory -- the one copy every associated
 * process shares (the INV-6-decisive object). */
struct vms_common_ef_cluster {
	exec_list_node_t list;       /* link in vms_common_ef_list */
	char             name[32];
	uint32_t         flags;      /* 32-bit event flag state */
	uint32_t         prot;       /* protection mask */
	uint32_t         perm;       /* permanent flag */
	int              refcount;
	exec_cv_t        waitq;      /* shared cv: set-side broadcasts, wait-side sleeps */
	exec_lock_t      lock;       /* shared guard for flags+waitq (cv contract) */
};

/* Per-process event flag state. */
struct vms_ef_state {
	uint32_t                      local[2];  /* clusters 0 (0-31) and 1 (32-63) */
	struct vms_common_ef_cluster *common[2]; /* clusters 2 (64-95) and 3 (96-127) */
	exec_cv_t                     waitq;     /* wait queue for local flags */
	exec_lock_t                   lock;      /* guards local[], common[], waitq */
};

/*
 * Per-process control block. On Linux this is a large struct with the whole
 * executive's per-process state; on the NetBSD substrate the event-flag facility
 * is the only one built (P2c), so the twin carries just its event-flag state
 * plus the fields the NetBSD `vms' pseudo-device's own per-pid proc table needs
 * (pid key + intrusive link). The facility touches ONLY .ef; the glue owns pid
 * and next.
 */
struct vms_proc {
	pid_t               pid;   /* proc-table key (NetBSD glue; facility ignores) */
	struct vms_proc    *next;  /* proc-table link  (NetBSD glue; facility ignores) */
	struct vms_ef_state ef;
};

/* ================================================================
 * Facility entry points -- DEFINED in src/kernel-core/vms_eflag.c, called by the
 * NetBSD `vms' pseudo-device dispatch (src/kernel-netbsd/vms_netbsd.c).
 * ================================================================ */
void vms_eflag_init(void);
void vms_eflag_cleanup(void);
void vms_proc_release_common_ef(struct vms_proc *proc);

long vms_ioctl_setef(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_clref(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_waitfr(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wflor(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wfland(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_readef(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_ascefc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dacefc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dlcefc(struct vms_proc *proc, unsigned long arg);

#endif /* _VMS_INTERNAL_H */
