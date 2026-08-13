/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_internal.h (NetBSD substrate twin) - the shared-struct header the
 * substrate-agnostic executive core includes on a NetBSD build (rd vms-4b4,
 * epic vms-8e8; docs/design-netbsd-executive-core.md §4).
 *
 * The Linux executive keeps its shared kernel structs in src/kernel/vms_internal.h,
 * a large header that pulls <linux/...> and carries the WHOLE executive (proc
 * table, ASTs, locks, mailboxes, ...). The relocated facility
 * src/kernel-core/vms_eflag.c includes "vms_internal.h" with NO substrate in its
 * name, and on Linux the build's -I resolves that to src/kernel's copy.
 *
 * On the NetBSD build there is NO -I into src/kernel (that header would not
 * compile in a NetBSD kernel TU), so the SAME `#include "vms_internal.h"'
 * resolves HERE via -I. (src/kernel-netbsd). This twin therefore provides EXACTLY
 * the slice the shared facilities built for NetBSD touch -- the event-flag,
 * AST and access-mode structs, the SS$ status codes they return, the arg structs
 * they copy, and the few Linux-kernel spellings they use (strscpy, READ_ONCE,
 * ERESTARTSYS) -- expressed in the substrate-agnostic exec_* and exec_list_*
 * vocabulary, so the SAME vms_eflag.c / vms_ast.c / vms_access.c sources compile
 * against it without a single `#if'. This is the per-substrate "glue" the design
 * record calls out (§4): the facilities are shared; the struct header and the
 * char-device/module lifecycle are backend-specific. As each further facility
 * joins the NetBSD build (mbx, proctab, locks -- P4-B onward) it extends this
 * twin with exactly the fields it touches, never the whole Linux PCB.
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

/* The facility argument structs the shared core copies in/out, plus the access
 * modes and privilege bits they gate on. Byte-identical to src/kernel/vms_ioctl.h;
 * shared with the userspace tool. Event flags (P2c), ASTs + access modes (P4-A). */
#include "vms_eflag_nb.h"
#include "vms_ast_nb.h"
#include "vms_access_nb.h"

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
/* Access-mode + AST subset (P4-A). Values match src/kernel/vms_internal.h
 * exactly (BADPARAM/NOPRIV public $SSDEF; EXASTLM AST-quota; NOTALLPRIV
 * oracle-pinned 1664, vms-2b8). */
#define SS__BADPARAM   0x00000014  /* SS$_BADPARAM */
#define SS__NOPRIV     0x00000024  /* SS$_NOPRIV */
#define SS__EXASTLM    0x00000038  /* SS$_EXASTLM (AST quota exceeded) */
#define SS__NOTALLPRIV 1664        /* SS$_NOTALLPRIV (not all requested privs authorized) */

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
 * AST structures (P4-A) -- the exact fields src/kernel-core/vms_ast.c uses, in
 * the exec_* / exec_list_* vocabulary. Compare src/kernel/vms_internal.h's
 * Linux-typed versions (struct list_head / spinlock_t); the facility names only
 * exec_* so it compiles unchanged on either substrate. Values match
 * src/kernel/vms_internal.h.
 * ================================================================ */

/* Per-process, per-mode AST quota (src/kernel/vms_internal.h VMS_AST_MAX_PER_MODE). */
#define VMS_AST_MAX_PER_MODE  64

/* A queued AST: routine address + parameter + the access mode it was declared at. */
struct vms_ast_entry {
	exec_list_node_t list;       /* link in vms_ast_state.pending */
	uint64_t         astadr;     /* userspace handler address */
	uint64_t         astprm;     /* parameter to handler */
	uint8_t          acmode;     /* access mode (0-3) */
};

/* Per-access-mode AST queue: the pending list, its count, an enable flag, guard. */
struct vms_ast_state {
	exec_list_head_t pending;    /* list of vms_ast_entry */
	int              count;      /* number of pending ASTs */
	int              enabled;    /* delivery enabled for this mode */
	exec_lock_t      lock;       /* guards pending + count + enabled */
};

/* ================================================================
 * Event-flag structures -- the exact fields src/kernel-core/vms_eflag.c uses,
 * in the exec_* and exec_list_* vocabulary. Compare src/kernel/vms_internal.h's
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
 * executive's per-process state; on the NetBSD substrate the twin carries just
 * the state the facilities built for NetBSD touch -- event flags (.ef, P2c),
 * ASTs (.ast + .hiber_*, P4-A) and access modes (.current_mode/.cur_privs/
 * .perm_privs/.mode_lock + .image_active/.pre_image_mode, P4-A) -- plus the
 * fields the NetBSD `vms' pseudo-device's own per-pid proc table needs (pid key
 * + intrusive link). The glue owns pid and next; the facilities own the rest.
 * Field names and types mirror src/kernel/vms_internal.h (exec_* in place of the
 * Linux spinlock_t/wait_queue_head_t) so the shared sources compile unchanged.
 */
struct vms_proc {
	pid_t               pid;   /* proc-table key (NetBSD glue; facility ignores) */
	struct vms_proc    *next;  /* proc-table link  (NetBSD glue; facility ignores) */
	struct vms_ef_state ef;

	/* Access-mode + privilege state (src/kernel-core/vms_access.c). */
	uint8_t             current_mode;     /* PSL_C_KERNEL..PSL_C_USER */
	uint8_t             image_active;     /* 1 while a controlled descent is open */
	uint8_t             pre_image_mode;   /* mode to restore on IMAGE_RUNDOWN */
	uint64_t            cur_privs;        /* current (temporary) privileges */
	uint64_t            perm_privs;       /* permanent (authorized) privileges */
	exec_lock_t         mode_lock;        /* guards current_mode/privs/image_* */

	/* AST queues + hibernate/wake (src/kernel-core/vms_ast.c). */
	struct vms_ast_state ast[4];          /* one queue per access mode 0..3 */
	exec_cv_t           hiber_wq;         /* $HIBER waiter cv; broadcast on AST arrival */
	exec_lock_t         hiber_lock;       /* paired guard for hiber_wq */
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

/* ----------------------------------------------------------------
 * AST facility (P4-A) -- DEFINED in src/kernel-core/vms_ast.c.
 * ---------------------------------------------------------------- */
long vms_ioctl_dclast(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setast(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_deliverast(struct vms_proc *proc, unsigned long arg);
void vms_proc_rundown_asts(struct vms_proc *proc, uint8_t min_acmode);
int  vms_ast_has_deliverable(struct vms_proc *proc, uint8_t cur_mode);
void vms_ast_notify_arrival(struct vms_proc *proc);

/* ----------------------------------------------------------------
 * Access-mode facility (P4-A) -- DEFINED in src/kernel-core/vms_access.c.
 * ---------------------------------------------------------------- */
long vms_ioctl_setmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setprv(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_chkpriv(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_enter_image(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_image_rundown(struct vms_proc *proc, unsigned long arg);

/* ----------------------------------------------------------------
 * Cross-facility image-rundown release helpers. vms_ioctl_image_rundown()
 * (vms_access.c) calls all three; only vms_proc_rundown_asts is DEFINED on the
 * NetBSD build today (vms_ast.c). vms_proc_rundown_locks (vms_lock.c, rd vms-ff7)
 * and vms_proc_rundown_channels (vms_mbx.c, rd vms-d7a) are not in this module's
 * SRCS yet, so vms_netbsd.c supplies WEAK no-op stubs that the real facility
 * definitions override once those facilities join the build (see vms_netbsd.c).
 * ---------------------------------------------------------------- */
void vms_proc_rundown_locks(struct vms_proc *proc, uint8_t min_acmode);
void vms_proc_rundown_channels(struct vms_proc *proc, uint8_t min_acmode);

#endif /* _VMS_INTERNAL_H */
