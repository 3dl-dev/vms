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
#include <sys/stdbool.h>   /* bool -- vms_proctab.c's predicates (P4-A) */

/* The shim contracts. Selected to their NetBSD backends by OVMX_KBACKEND_NETBSD
 * (set by the kmodule build). These give exec_lock_t / exec_cv_t, the
 * exec_list_* container types the structs below embed, and (P4-A proctab, rd
 * vms-ca7) exec_hash_node_t + the host-task/RCU-lite/sleepable-mutex ops the
 * process table binds. */
#include "exec_kbackend.h"
#include "exec_list.h"
#include "exec_hash.h"
/* The lock manager's lock-ID database is an intrusive red-black tree; its NetBSD
 * backend (exec_rbtree_netbsd.{h,c}) is selected by OVMX_KBACKEND_NETBSD. Needed
 * for exec_rbtree_node_t embedded in struct vms_lock_entry below (P4-A locks, rd
 * vms-ff7). */
#include "exec_rbtree.h"

/* The facility argument structs the shared core copies in/out, plus the access
 * modes and privilege bits they gate on. Byte-identical to src/kernel/vms_ioctl.h;
 * shared with the userspace tool. Event flags (P2c), ASTs + access modes (P4-A). */
#include "vms_eflag_nb.h"
#include "vms_ast_nb.h"
#include "vms_access_nb.h"
/* Mailbox (MBAn:) wire contract (P4-A, rd vms-d7a): the mailbox arg structs the
 * facility copies, VMS_DEVNAM_SIZE, the VMS_MBX_* sizing and the VMS_MBX_READ_NOW
 * modifier. Byte-identical to src/kernel/vms_mbx.h. */
#include "vms_mbx_nb.h"
/* Process-table wire contract (P4-A, rd vms-ca7): the $GETJPI/$SETPRN/
 * $PROCESS_SCAN/$SETIDENT/$HIBER/$WAKE arg structs the facility copies, the
 * VMS_PRCNAM/USERNAME sizes, the VMS_JPI_SEL and VMS_PI_V codes, the SS$_DUPLNAM/
 * NONEXPR/IVLOGNAM statuses, VMS_PRV_M_WORLD and the SYSTEM identity constants.
 * Byte-identical to src/kernel/vms_ioctl.h. */
#include "vms_proctab_nb.h"
/* Lock-manager (DLM) wire contract (P4-A, rd vms-ff7): the $ENQ/$DEQ/$CONVERT/
 * $GETLKI/GET_RESMASTER arg structs the facility copies, the LCK_K_* modes,
 * LCK_M_* flags and LCK_VALBLK_SIZE it tests. Byte-identical to
 * src/kernel/vms_ioctl.h. */
#include "vms_lock_nb.h"

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
/* Mailbox subset (P4-A, rd vms-d7a). Values match src/kernel/vms_internal.h. */
#define SS__EXQUOTA   28           /* SS$_EXQUOTA (mailbox buffer quota) */
#define SS__ENDOFFILE 2160         /* SS$_ENDOFFILE (IO$M_NOW read of an empty mailbox) */
#define SS__IVCHAN    602          /* SS$_IVCHAN -- invalid I/O channel */
#define SS__IVDEVNAM  608          /* SS$_IVDEVNAM -- invalid device name */
#define SS__NOSUCHDEV 2680         /* SS$_NOSUCHDEV -- no such device available */
/* Lock-manager subset (P4-A, rd vms-ff7). Values match src/kernel/vms_internal.h
 * exactly -- each is the value src/libvms/include/ssdef.h already ships for that
 * name, produced IN the executive (the lock manager yields VMS condition values,
 * vms-82a). SS__UNSUPPORTED is the honest "remote directory/master -- not built
 * yet" answer for a non-local DLM path (0.4), never a fabricated remote grant. */
#define SS__NOTQUEUED   2488       /* SS$_NOTQUEUED (LCK_M_NOQUEUE, not granted) */
#define SS__DEADLOCK    3594       /* SS$_DEADLOCK (wait-for cycle detected) */
#define SS__IVLOCKID    8484       /* SS$_IVLOCKID (invalid lock ID) */
#define SS__CANCELGRANT 8508       /* SS$_CVTUNGRANT (conversion could not be granted) */
#define SS__UNSUPPORTED 2296       /* SS$_UNSUPPORTED (remote DLM path -- 0.4) */

/* ================================================================
 * Mailbox privilege bits (P4-A, rd vms-d7a). PSL_C_* and CMKRNL/CMEXEC/SETPRV
 * come from vms_access_nb.h; the two mailbox-creation privileges the mailbox
 * facility gates $CREMBX on are added here. Bit positions ORACLE-PINNED in
 * src/kernel/vms_ioctl.h (PRV$V_PRMMBX 11, PRV$V_TMPMBX 15).
 * ================================================================ */
#ifndef VMS_PRV_M_PRMMBX
#define VMS_PRV_M_PRMMBX  (1ULL << 11)   /* create permanent mailbox  (PRV$V_PRMMBX) */
#endif
#ifndef VMS_PRV_M_TMPMBX
#define VMS_PRV_M_TMPMBX  (1ULL << 15)   /* create temporary mailbox  (PRV$V_TMPMBX) */
#endif
#ifndef VMS_PRV_M_NETMBX
#define VMS_PRV_M_NETMBX  (1ULL << 20)   /* create network device     (PRV$V_NETMBX) */
#endif
/* The privileges EVERY VMS process holds by default (TMPMBX + NETMBX), matching
 * src/kernel/vms_internal.h's VMS_DEFAULT_PRIVS. A fresh OVMX process must be
 * seeded with these on BOTH substrates or a default-privilege operation the
 * oracle allows -- e.g. $CREMBX of a TEMPORARY mailbox, which gates on TMPMBX
 * (vms_mbx.c mbx_priv_check) -- wrongly returns SS$_NOPRIV (rd vms-f8a: the
 * NetBSD proc seed omitted them, so the P4-A mailbox cross-process proof's
 * $CREMBX failed once proctab stopped masking it). */
#ifndef VMS_DEFAULT_PRIVS
#define VMS_DEFAULT_PRIVS  (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX)
#endif

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

/* pr_info (Linux) -> the NetBSD kernel printf (<sys/systm.h>). Only the mailbox
 * facility's one-line vms_mbx_init() load message uses it (P4-A). GNU
 * ##__VA_ARGS__ is fine under the build's -std=gnu99. */
#ifndef pr_info
#define pr_info(fmt, ...)  printf(fmt, ##__VA_ARGS__)
#endif

/*
 * kstrtouint (Linux) -- parse a NUL-terminated string as an unsigned int in the
 * given base, rejecting trailing garbage (a single trailing newline aside),
 * returning 0 on success or a negative errno. NetBSD's libkern has no kstrtouint,
 * so this shim is built on its strtoul (<lib/libkern/libkern.h>, pulled by
 * <sys/systm.h>). The only caller, vms_mbx.c's mbx_normalize_devnam(), passes a
 * string already validated to be alphanumeric (no sign, no whitespace), so this
 * exactly reproduces the Linux behaviour that path relies on: "MBA12" -> 12,
 * "MBA1a" -> -EINVAL. (P4-A, rd vms-d7a.)
 */
#ifndef kstrtouint
static __inline int
vms_nb_kstrtouint(const char *s, unsigned int base, unsigned int *res)
{
	unsigned long v;
	char *end;

	if (s == NULL || *s == '\0')
		return -EINVAL;
	v = strtoul(s, &end, (int)base);
	if (*end == '\n')          /* Linux kstrtouint tolerates one trailing '\n' */
		end++;
	if (*end != '\0')
		return -EINVAL;
	if (v > 0xffffffffUL)
		return -ERANGE;
	*res = (unsigned int)v;
	return 0;
}
#define kstrtouint(s, base, res)  vms_nb_kstrtouint((s), (base), (res))
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

/* ================================================================
 * Lock-manager (DLM) structures (P4-A, rd vms-ff7) -- the exact fields
 * src/kernel-core/vms_lock.c uses, in the exec_* / exec_list_* / exec_rbtree_*
 * vocabulary. Compare src/kernel/vms_internal.h's Linux-typed versions (struct
 * list_head / struct rb_node / struct hlist_node / wait_queue_head_t /
 * spinlock_t); the facility names only exec_* so it compiles unchanged on either
 * substrate. These are kernel-internal (never cross the /dev/vms boundary), so
 * only field PRESENCE and type must match the Linux twin -- not byte layout.
 * ================================================================ */

/* Lock entry -- a process's view of one granted or waiting lock. */
struct vms_lock_entry {
	exec_list_node_t    proc_list;      /* link in the process's lock list */
	exec_list_node_t    res_granted;    /* link in the resource's granted list */
	exec_list_node_t    res_waiting;    /* link in the resource's waiting list */
	exec_rbtree_node_t  rb_node;        /* link in the global lock-ID tree */
	uint32_t            lkid;
	uint32_t            granted_mode;   /* current granted mode (0-5) */
	uint32_t            requested_mode; /* requested mode if waiting */
	uint32_t            flags;          /* LCK_M_* */
	uint64_t            astadr;         /* completion AST */
	uint64_t            astprm;
	uint64_t            blkastadr;      /* blocking AST */
	uint8_t             valblk[LCK_VALBLK_SIZE];
	struct vms_lock_resource *resource;
	struct vms_proc     *proc;
	int                 waiting;        /* 1 if on the waiting list */
	int                 refcount;       /* reference count for safe lookup */
	exec_cv_t           wait_wq;        /* sync ENQ ($ENQW): blocker sleeps here */
	int                 grant_state;    /* sync wake: 0=pending, SS__NORMAL=granted,
	                                     *            SS__DEADLOCK=cycle detected */
	uint8_t             acmode;         /* access mode $ENQ was issued from (0-3),
	                                     * for image rundown -- NOT a lock mode */
};

/* Lock resource -- a named resource in the lock database. */
struct vms_lock_resource {
	exec_hash_node_t    hash_node;      /* link in the global resource hash */
	char                name[32];
	exec_list_head_t    granted;        /* granted lock list */
	exec_list_head_t    waiting;        /* waiting lock list (FIFO) */
	uint8_t             valblk[LCK_VALBLK_SIZE]; /* resource value block */
	exec_lock_t         lock;
	int                 refcount;
	struct vms_lock_resource *parent;
	/* DLM directory + mastering (vms-ci.5 DB, LOCAL scaffolding): both resolve
	 * to vms_local_csid on a stub-of-one membership; a non-local directory or
	 * master returns SS__UNSUPPORTED (0.4), never a fabricated remote grant. */
	uint32_t            dir_csid;       /* directory node CSID for `name` */
	uint32_t            master_csid;    /* mastering node CSID; 0 = unmastered */
};

/* The global resource-database hash. vms_lock.c lays down the bucket array with
 * EXEC_DEFINE_HASHTABLE(vms_res_hash, VMS_RES_HASH_BITS); this bit count matches
 * src/kernel/vms_internal.h (1024 buckets). Unlike the RCU process hash, the
 * resource hash has no lockless readers -- every walk runs under
 * vms_res_hash_lock (exec_hash_add/del/for_each_possible, non-RCU). */
#define VMS_RES_HASH_BITS   10

/*
 * This node's cluster system ID. DEFINED by the module glue
 * (src/kernel-netbsd/vms_netbsd.c) exactly as the Linux vms.ko defines it in
 * vms_module.c -- the CSID and its module-lifecycle setter are a per-substrate
 * rind concern, not portable executive logic (design record §4). The shared lock
 * manager reads it through this extern for its DLM directory/mastering
 * resolution. A stub-of-one membership makes every resource resolve to this
 * CSID (case (1): self is directory and master). NOT a claim of a VMS-authentic
 * CSID value/layout (CLAUDE.md Rule 8); the connection manager assigns the real
 * one at cluster join (0.4).
 */
extern uint32_t vms_local_csid;

/*
 * Per-process control block. On Linux this is a large struct with the whole
 * executive's per-process state; on the NetBSD substrate the twin carries just
 * the state the facilities built for NetBSD touch -- event flags (ef, P2c),
 * ASTs (ast + hiber_*, P4-A), access modes (current_mode/cur_privs/perm_privs/
 * mode_lock + image_active/pre_image_mode, P4-A) and the process table
 * (hash_node/pid_ref/uic/prcnam/username/terminal/p0+p1 extents/wake_pending,
 * P4-A rd vms-ca7) -- plus the glue's proc-table key (pid).
 *
 * THE PROCESS TABLE IS ONE TABLE (rd vms-ca7). Before proctab joined the NetBSD
 * SRCS the glue kept a private singly-linked proc list (a "next" pointer); now
 * the SHARED facility src/kernel-core/vms_proctab.c walks the SAME table the glue
 * populates, so the two are unified onto the intrusive "hash_node" the facility
 * expects (vms_proc_hash, below). $GETJPI must find the very proc that $ASCEFC'd
 * a cluster or queued an AST, so there can be exactly one per-pid struct, shared
 * by every facility. The glue owns pid, hash_node, pid_ref and rcu; the
 * facilities own the rest. Field names and types mirror src/kernel/vms_internal.h
 * (exec_* in place of the Linux spinlock_t/wait_queue_head_t/hlist_node/struct
 * pid pointer) so the shared sources compile unchanged.
 */
struct vms_proc {
	pid_t               pid;   /* proc-table key (NetBSD glue; facility ignores) */
	exec_hash_node_t    hash_node;  /* link in vms_proc_hash (glue-owned; the
	                                 * facility walks it via exec_hash_for_each) */
	struct vms_ef_state ef;

	/* Lock manager (src/kernel-core/vms_lock.c, P4-A rd vms-ff7). The list of
	 * every lock this process holds, its count, and their guard -- the glue
	 * brings them up at proc creation (exec_list_head_init/exec_lock_init) and
	 * vms_proc_release_locks() drains them at process death, exactly as the
	 * Linux vms.ko does in vms_module.c. */
	exec_list_head_t    locks;            /* struct vms_lock_entry, via proc_list */
	int                 lock_count;
	exec_lock_t         lock_list_lock;   /* guards locks + lock_count */

	/* Access-mode + privilege state (src/kernel-core/vms_access.c). */
	uint8_t             current_mode;     /* PSL_C_KERNEL..PSL_C_USER */
	uint8_t             image_active;     /* 1 while a controlled descent is open */
	uint8_t             pre_image_mode;   /* mode to restore on IMAGE_RUNDOWN */
	uint64_t            cur_privs;        /* current (temporary) privileges */
	uint64_t            perm_privs;       /* permanent (authorized) privileges */
	exec_lock_t         mode_lock;        /* guards current_mode/privs/image_* */

	/* AST queues + hibernate/wake (src/kernel-core/vms_ast.c + vms_proctab.c). */
	struct vms_ast_state ast[4];          /* one queue per access mode 0..3 */
	exec_cv_t           hiber_wq;         /* $HIBER waiter cv; broadcast on AST arrival */
	exec_lock_t         hiber_lock;       /* paired guard for hiber_wq */
	uint8_t             wake_pending;     /* sticky $WAKE bit (PCB$V_WAKEPEN), $HIBER/$WAKE */

	/* Identity (glue-populated; the mailbox facility stamps a created mailbox's
	 * owner from these -- informational only, not consulted by $ASSIGN's lookup). */
	pid_t               linux_pid;
	uint32_t            vms_pid;

	/*
	 * Executive-resident process identity (src/kernel-core/vms_proctab.c). The
	 * NAME and UIC live here, in the one table every process shares, which is
	 * what makes a $SETPRN name resolvable by another process's $GETJPI. uic is
	 * DERIVED by the glue from the calling task's real credentials (never a
	 * value a process supplies); prcnam/username/terminal start "" and are
	 * stamped by $SETPRN / $SETIDENT (terminal only once the device table joins
	 * SRCS -- it stays "" until then, honestly empty).
	 */
	uint32_t            uic;                       /* (group << 16) | member */
	char                prcnam[VMS_PRCNAM_SIZE];   /* process name ("" if unnamed) */
	char                username[VMS_USERNAME_SIZE]; /* "" until $SETIDENT stamps it */
	char                terminal[VMS_DEVNAM_SIZE]; /* "" (no device table in SRCS yet) */

	/* P0 program / P1 control region extents (structural; $GETJPI reports them,
	 * VMS_IOCTL_P0_MAP/P1_MAP would record them -- not in this module's SRCS, so
	 * both pairs stay zero). Carried so proc_fill_info compiles and reports them
	 * absent, exactly as on Linux before an image is mapped. */
	uint64_t            p0_base;
	uint64_t            p0_limit;
	uint64_t            p1_base;
	uint64_t            p1_limit;

	/*
	 * Host-task liveness handle (P4-A, rd vms-ca7). The facility tests
	 * proc->pid_ref for whole-process liveness (exec_task_alive) and pins it to
	 * read accounting (exec_task_pin), never dereferencing it -- it is opaque.
	 * On Linux this is a refcounted `struct pid *' (get_pid/put_pid); on NetBSD
	 * exec_task_ref_t just carries the pid, so the glue embeds the storage in the
	 * PCB (pid_ref_store) and points pid_ref at it, set once at creation. No ref
	 * to drop at teardown (proc_find(9) takes none), so freeing the PCB frees it.
	 */
	exec_task_ref_t     pid_ref_store;    /* backing storage (NetBSD glue) */
	exec_task_ref_t    *pid_ref;          /* -> pid_ref_store; the facility's handle */

	/* Mailbox channels + the channel-number allocator (src/kernel-core/vms_mbx.c,
	 * P4-A). On real VMS a process's channels are one number space regardless of
	 * device kind; on this build only mailbox channels are built, so next_chan
	 * feeds them. */
	uint32_t            next_chan;
	exec_lock_t         chan_lock;
	exec_list_head_t    mbx_channels;     /* struct vms_mbx_chan (defined in vms_mbx.c) */

	/* Deferred-free head (P4-A, rd vms-ca7). The reaper unlinks a dead PCB under
	 * vms_proc_hash_lock (exec_hash_del_rcu) and reclaims it through
	 * exec_free_deferred(&proc->rcu, ...) -- immediate on NetBSD (no lockless
	 * readers), the blessed grace-period fallback. Mirrors Linux's kfree_rcu. */
	exec_rcu_head_t     rcu;
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
 * MAILBOX facility (MBAn:, P4-A, rd vms-d7a) -- DEFINED in
 * src/kernel-core/vms_mbx.c, called by the NetBSD `vms' pseudo-device dispatch.
 * vms_mbx_init/cleanup bracket the module lifetime; vms_mbx_release_all runs
 * per-process at proc teardown; vms_mbx_dassgn is the $DASSGN fallback (wired
 * once the device table joins this module, rd vms-31b).
 * ---------------------------------------------------------------- */
void vms_mbx_init(void);
void vms_mbx_cleanup(void);
long vms_ioctl_mbx_create(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_assign(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_delmbx(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_write(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_read(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_set_wrtattn(struct vms_proc *proc, unsigned long arg);
int  vms_mbx_dassgn(struct vms_proc *proc, uint32_t chan);
void vms_mbx_release_all(struct vms_proc *proc);

/* ----------------------------------------------------------------
 * LOCK MANAGER facility (DLM, P4-A, rd vms-ff7) -- DEFINED in
 * src/kernel-core/vms_lock.c, the LAST executive facility promoted onto the
 * shared core and the sole exec_rbtree consumer. vms_lock_init/cleanup bracket
 * the module lifetime (the resource hash + lock-ID tree live module-global);
 * vms_proc_release_locks drains a process's held locks at proc teardown (the
 * $DEQ-all at process death, mirroring the Linux vms.ko's vms_module.c). The
 * five ioctl entry points are dispatched by vms_netbsd.c.
 * ---------------------------------------------------------------- */
int  vms_lock_init(void);
void vms_lock_cleanup(void);
void vms_proc_release_locks(struct vms_proc *proc);
long vms_ioctl_enq(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_deq(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_convert(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getlki(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_get_resmaster(struct vms_proc *proc, unsigned long arg);

/* ----------------------------------------------------------------
 * Cross-facility image-rundown release helpers. vms_ioctl_image_rundown()
 * (vms_access.c) calls all three; vms_proc_rundown_asts is DEFINED (vms_ast.c)
 * and vms_proc_rundown_locks is DEFINED (vms_lock.c, now in SRCS -- rd vms-ff7).
 * vms_mbx.c does NOT define an image-rundown channel release (a mailbox channel
 * is released at $DASSGN / process death via vms_mbx_release_all, not at image
 * rundown), so vms_netbsd.c still supplies a WEAK no-op stub for
 * vms_proc_rundown_channels only, which a real facility definition would override
 * if one ever lands (see vms_netbsd.c).
 * ---------------------------------------------------------------- */
void vms_proc_rundown_locks(struct vms_proc *proc, uint8_t min_acmode);
void vms_proc_rundown_channels(struct vms_proc *proc, uint8_t min_acmode);

/* ================================================================
 * PROCESS TABLE (P4-A, rd vms-ca7) -- the executive process database
 * (src/kernel-core/vms_proctab.c). The TABLE and its lock are DEFINED by the
 * module glue (src/kernel-netbsd/vms_netbsd.c), exactly as the Linux vms.ko
 * defines them in vms_module.c -- the process table has its lifecycle bound to
 * the module, and on NetBSD the module lifecycle is vms_netbsd.c. The core
 * facility only references the extern table by name (exec_hash_for_each) and
 * calls the glue-provided vms_proc_free_claimed() to reclaim a dead entry.
 *
 * VMS_PROC_HASH_BITS matches src/kernel/vms_internal.h (1024 buckets). The table
 * is a fixed bucket array (EXEC_DEFINE/DECLARE_HASHTABLE), keyed by host pid; all
 * walks run under vms_proc_hash_lock (there are NO lockless readers on this
 * substrate -- the RCU-lite blessed fallback, exec_kbackend_netbsd.h §6).
 * ================================================================ */
#define VMS_PROC_HASH_BITS  10
EXEC_DECLARE_HASHTABLE(vms_proc_hash, VMS_PROC_HASH_BITS);
extern exec_lock_t vms_proc_hash_lock;

/* Glue-provided: tear down a PCB already unlinked from the hash under
 * vms_proc_hash_lock (the unlink is the ownership claim). DEFINED in
 * vms_netbsd.c; CALLED by the facility's reaper (vms_proc_reap_dead). */
void vms_proc_free_claimed(struct vms_proc *proc);

/* Facility-provided (src/kernel-core/vms_proctab.c). vms_proc_reap_dead and
 * vms_proc_may_read are used cross-file on Linux; here they are simply part of
 * the compiled facility. The ioctl entry points are dispatched by vms_netbsd.c. */
void vms_proc_reap_dead(void);
bool vms_proc_may_read(const struct vms_proc *caller, const struct vms_proc *target);

long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setident(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_establish_system(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_hiber(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wake(struct vms_proc *proc, unsigned long arg);

#endif /* _VMS_INTERNAL_H */
