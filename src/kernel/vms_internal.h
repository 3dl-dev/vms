/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_internal.h - Internal kernel module definitions for vms.ko
 *
 * Shared structures and prototypes between the kernel module subsystems.
 */

#ifndef _VMS_INTERNAL_H
#define _VMS_INTERNAL_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/rbtree.h>
#include <linux/mutex.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/hashtable.h>

#include "vms_ioctl.h"

/* ================================================================
 * VMS status codes (canonical definitions — do not redefine in .c files)
 * ================================================================ */

#define SS__NORMAL      0x00000001
#define SS__BADPARAM    0x00000014
/*
 * Process-table statuses -- ORACLE-PINNED (vms-8019).
 *
 * SOURCE: the reference lab OpenVMS VAX V7.3 node VAX1 (~/vax/cluster),
 * 2026-07-30, by two independent documented-tool observations:
 *
 *   1. LIBRARY/EXTRACT=$SSDEF/OUTPUT=SYS$SCRATCH:SSDEF.MAR
 *          SYS$LIBRARY:STARLET.MLB
 *      SEARCH SYS$SCRATCH:SSDEF.MAR "IVLOGNAM","DUPLNAM","NONEXPR"
 *        $EQU  SS$_DUPLNAM    148
 *        $EQU  SS$_IVLOGNAM   340
 *        $EQU  SS$_NONEXPR    2280
 *
 *   2. round-trip through the message formatter:
 *        F$MESSAGE(148)  -> %SYSTEM-F-DUPLNAM,  duplicate name
 *        F$MESSAGE(340)  -> %SYSTEM-F-IVLOGNAM, invalid logical name
 *        F$MESSAGE(2280) -> %SYSTEM-W-NONEXPR,  nonexistent process
 *
 * These replace 434 / 596 / 2540, which this tree carried and which the
 * same oracle disproves: F$MESSAGE(596) is %SYSTEM-F-VOLINV, and
 * SS$_VOLINV is 596 in $SSDEF.
 *
 * SS__IVLOGNAM is what the executive returns for a malformed
 * process-name string handed to VMS_IOCTL_SETPRN (unterminated buffer,
 * or zero length). The CHOICE of IVLOGNAM for a bad process name is
 * oracle-pinned too, behaviourally, on the same node:
 *   $ SET PROCESS/NAME="THISNAMEISWAYTOOLONG"
 *   %SET-E-NOTSET, error modifying process name
 *   -SYSTEM-F-IVLOGNAM, invalid logical name
 */
#define SS__DUPLNAM     148         /* duplicate process name (ssdef.h SS$_DUPLNAM) */
#define SS__NONEXPR     2280        /* nonexistent process (ssdef.h SS$_NONEXPR) */
#define SS__IVLOGNAM    340         /* invalid name string (ssdef.h SS$_IVLOGNAM) */
#define SS__NOPRIV      0x00000024
#define SS__ACCVIO      0x0000000C
#define SS__INSFMEM     0x0000002C  /* insufficient memory (44 decimal, matches real VMS) */
#define SS__EXASTLM     0x00000038  /* AST quota exceeded */
#define SS__WASSET      9           /* flag/AST was enabled/set */
#define SS__WASCLR      5           /* flag/AST was disabled/clear */
#define SS__ILLEFC      44          /* illegal event flag number */
#define SS__UNASEFC     48          /* unassociated common EFC */
#define SS__NOTQUEUED   40          /* lock not queued (NOQUEUE flag) */
#define SS__DEADLOCK    100         /* deadlock detected */
#define SS__IVLOCKID    108         /* invalid lock ID */
#define SS__SUBLOCKS    112         /* sublocks still held */
#define SS__CANCELGRANT 116         /* conversion cancelled */
#define SS__VALNOTVALID 120         /* value block not valid */

/*
 * Default privilege set for non-CAP_SYS_ADMIN processes.
 * Allows basic operational use (mailboxes, networking) without
 * granting any mode-change or bypass privileges.
 */
#define VMS_DEFAULT_PRIVS   ((1ULL << 7) | (1ULL << 8))  /* TMPMBX | NETMBX */

/* ================================================================
 * Per-process VMS state
 *
 * Allocated on VMS_IOCTL_REGISTER, looked up by Linux pid.
 * Freed when the process exits (via task_work or explicit cleanup).
 * ================================================================ */

/* Maximum AST queue depth per access mode */
#define VMS_AST_MAX_PER_MODE    64

/* Maximum locks per process */
#define VMS_MAX_LOCKS_PER_PROC  256

struct vms_ast_entry {
    struct list_head    list;
    uint64_t            astadr;     /* userspace handler address */
    uint64_t            astprm;     /* parameter to handler */
    uint8_t             acmode;     /* access mode (0-3) */
};

struct vms_ast_state {
    struct list_head    pending;    /* list of vms_ast_entry */
    int                 count;      /* number of pending ASTs */
    int                 enabled;    /* delivery enabled for this mode */
    spinlock_t          lock;
};

/* Common event flag cluster (shared between processes) */
struct vms_common_ef_cluster {
    struct list_head    list;
    char                name[32];
    uint32_t            flags;      /* 32-bit event flag state */
    uint32_t            prot;       /* protection mask */
    uint32_t            perm;       /* permanent flag */
    int                 refcount;
    wait_queue_head_t   waitq;
    spinlock_t          lock;
};

/* Per-process event flag state */
struct vms_ef_state {
    uint32_t            local[2];       /* clusters 0 (EFN 0-31) and 1 (EFN 32-63) */
    struct vms_common_ef_cluster *common[2]; /* clusters 2 (EFN 64-95) and 3 (EFN 96-127) */
    wait_queue_head_t   waitq;          /* wait queue for local flags */
    spinlock_t          lock;
};

/* Lock entry (per-process view of a granted/waiting lock) */
struct vms_lock_entry {
    struct list_head    proc_list;      /* link in process's lock list */
    struct list_head    res_granted;    /* link in resource's granted list */
    struct list_head    res_waiting;    /* link in resource's waiting list */
    struct rb_node      rb_node;        /* in global lock ID tree */
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
    int                 waiting;        /* 1 if on waiting list */
    int                 refcount;       /* reference count for safe lookup */
    wait_queue_head_t   wait_wq;        /* sync ENQ ($ENQW): blocker sleeps here */
    int                 grant_state;    /* sync wake: 0=pending, SS__NORMAL=granted,
                                         *            SS__DEADLOCK=cycle detected */
};

/* Lock resource (named resource in the lock database) */
struct vms_lock_resource {
    struct hlist_node   hash_node;      /* in global resource hash */
    char                name[32];
    struct list_head    granted;        /* granted lock list */
    struct list_head    waiting;        /* waiting lock list (FIFO) */
    uint8_t             valblk[LCK_VALBLK_SIZE]; /* resource value block */
    spinlock_t          lock;
    int                 refcount;
    struct vms_lock_resource *parent;
};

/* Per-process VMS state */
struct vms_proc {
    struct hlist_node   hash_node;      /* in global process hash */
    pid_t               linux_pid;      /* Linux PID (key) */
    uint32_t            vms_pid;        /* VMS-style PID */

    /*
     * Executive-resident identity. This is the whole point of the
     * process table: prcnam lives here, in the executive, so it is
     * visible to every other process and survives execve() (the pid
     * key does not change across image activation).
     *
     * uic is derived by the executive from the task's credentials at
     * registration -- never supplied by the process, which must not be
     * able to declare its own UIC. Protected by vms_proc_hash_lock.
     */
    char                prcnam[VMS_PRCNAM_SIZE];
    uint32_t            uic;            /* (group << 16) | member */

    /*
     * Reference to the backing task's struct pid. The PCB belongs to
     * the PROCESS, not to an open channel, so it is not destroyed when
     * /dev/vms is closed (notably the implicit close at exec time).
     * Liveness is tested through this reference and dead entries are
     * reaped lazily -- see vms_proc_reap_dead().
     */
    struct pid          *pid_ref;

    /* Access mode (3a) */
    uint8_t             current_mode;   /* PSL_C_KERNEL..PSL_C_USER */
    uint64_t            cur_privs;      /* current (temporary) privileges */
    uint64_t            perm_privs;     /* permanent privileges */
    spinlock_t          mode_lock;

    /* AST state (3b) - one queue per access mode */
    struct vms_ast_state ast[4];

    /* Event flags (3c) */
    struct vms_ef_state ef;

    /* Lock manager (3d) */
    struct list_head    locks;          /* all locks held by this process */
    int                 lock_count;
    spinlock_t          lock_list_lock;

    struct rcu_head     rcu;
};

/* ================================================================
 * Global module state
 * ================================================================ */

/* Process hash table (keyed by linux PID) */
#define VMS_PROC_HASH_BITS  10
extern DECLARE_HASHTABLE(vms_proc_hash, VMS_PROC_HASH_BITS);
extern spinlock_t vms_proc_hash_lock;

/* Lock ID allocator and tree */
extern struct rb_root vms_lock_id_tree;
extern spinlock_t vms_lock_id_lock;
extern uint32_t vms_next_lock_id;

/* Resource hash table */
#define VMS_RES_HASH_BITS   10
extern DECLARE_HASHTABLE(vms_res_hash, VMS_RES_HASH_BITS);
extern spinlock_t vms_res_hash_lock;

/* Common event flag cluster list */
extern struct list_head vms_common_ef_list;
extern spinlock_t vms_common_ef_lock;

/* ================================================================
 * Process lookup
 * ================================================================ */

struct vms_proc *vms_proc_find(pid_t pid);
struct vms_proc *vms_proc_find_or_err(void);
struct vms_proc *vms_proc_register(pid_t pid, uint32_t vms_pid, uint64_t init_privs);
void vms_proc_free(struct vms_proc *proc);
/* Tear down an entry the caller has ALREADY unlinked under
 * vms_proc_hash_lock (the unlink is the ownership claim). */
void vms_proc_free_claimed(struct vms_proc *proc);

/* Drop table entries whose backing task no longer exists. */
void vms_proc_reap_dead(void);

/* ================================================================
 * Subsystem ioctl handlers
 * ================================================================ */

/* Access mode (3a) */
long vms_ioctl_setmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setprv(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_chkpriv(struct vms_proc *proc, unsigned long arg);

/* AST delivery (3b) */
long vms_ioctl_dclast(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setast(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_deliverast(struct vms_proc *proc, unsigned long arg);

/* Event flags (3c) */
long vms_ioctl_setef(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_clref(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_waitfr(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wflor(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wfland(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_readef(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_ascefc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dacefc(struct vms_proc *proc, unsigned long arg);

/* Lock manager (3d) */
long vms_ioctl_enq(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_deq(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_convert(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getlki(struct vms_proc *proc, unsigned long arg);

/* Process table (executive-resident PCB directory) */
long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg);

/* Subsystem init/cleanup */
int vms_lock_init(void);
void vms_lock_cleanup(void);
void vms_eflag_init(void);
void vms_eflag_cleanup(void);

/* Lock manager helpers */
void vms_proc_release_locks(struct vms_proc *proc);
void vms_proc_release_common_ef(struct vms_proc *proc);

#endif /* _VMS_INTERNAL_H */
