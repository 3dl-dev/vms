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
 * Device-table statuses. Values are this tree's existing ssdef.h
 * (src/libvms/include/ssdef.h) -- they are NOT independently
 * re-derived here, so the executive and the runtime cannot drift
 * apart. Note that ssdef.h already carries an operator-sign-off flag
 * on SS$_NOSUCHDEV / SS$_NOMOREDEV (multi-source disagreement, see
 * vms-fb3); this file inherits that caveat rather than papering over
 * it. The CHOICE of status per condition is pinned to the oracle
 * where observable: SHOW DEVICE of an absent device on the ~/vax
 * OpenVMS VAX V7.3 lab reports
 *   %SYSTEM-W-NOSUCHDEV, no such device available
 * (docs/oracle/vax73-terminal-device.md).
 */
#define SS__IVCHAN      602         /* invalid I/O channel */
#define SS__IVDEVNAM    608         /* invalid device name */
#define SS__NOMOREDEV   2648        /* device scan exhausted */
#define SS__NOSUCHDEV   2680        /* no such device available */
/*
 * Allocation statuses. Unlike the four above, these two were measured
 * directly on the oracle rather than inherited: VMS's own message
 * facility on the ~/vax OpenVMS VAX V7.3 lab reports
 *   2112 %SYSTEM-W-DEVALLOC, device already allocated to another user
 *   2136 %SYSTEM-W-DEVNOTALLOC, device not allocated
 * and $ALLOC/$DALLOC were observed returning exactly those conditions
 * (docs/oracle/vax73-terminal-device.md sections 7-9). ssdef.h carries
 * the same values and the same citation.
 */
#define SS__DEVALLOC    2112        /* device already allocated to another user */
#define SS__DEVNOTALLOC 2136        /* device not allocated */

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

    /*
     * I/O channels (device table, vms-d0b). A channel is this
     * process's handle on a device that the EXECUTIVE owns -- the
     * device itself is not per-process, only the channel to it is.
     * Released when the process's executive state is torn down, which
     * is what drops the device's reference count and its ownership.
     */
    struct list_head    channels;       /* struct vms_channel */
    uint32_t            next_chan;      /* channel number allocator */
    spinlock_t          chan_lock;

    struct rcu_head     rcu;
};

/* ================================================================
 * Device table (executive-resident I/O database)
 *
 * One entry per device on the node, created by the executive and
 * visible to every process. See vms_devtab.c.
 * ================================================================ */

struct vms_device {
    struct list_head    list;           /* in vms_device_list */
    char                devnam[VMS_DEVNAM_SIZE];
    uint32_t            devclass;       /* DC$_ device class */
    uint32_t            devtype;        /* device type code; 0 = Unknown */

    /*
     * Ownership comes from ALLOCATION, not from assigning a channel.
     * This is measured, not assumed: on the ~/vax OpenVMS VAX V7.3 lab
     * a process that held an open channel to NLA0: left the device's
     * "Owner process" empty and its "Owner process ID" 00000000, while
     * ALLOCATE set both and added the word "allocated" to the
     * SHOW DEVICE/FULL status clause
     * (docs/oracle/vax73-terminal-device.md sections 7-9).
     *
     * refcnt is the device's "Reference count": one per assigned
     * channel plus one for an outstanding allocation -- also measured
     * (NLA0: 2 -> 3 -> 2 across an OPEN/CLOSE; OPA0: 2 -> 3 on
     * ALLOCATE and back to 2 on DEALLOCATE).
     */
    uint32_t            allocated;      /* 1 while allocated to owner_* */
    uint32_t            owner_pid;      /* VMS pid of the allocating process */
    pid_t               owner_linux_pid;
    uint32_t            owner_uic;
    uint32_t            refcnt;

    uint32_t            errcnt;
    uint64_t            opcnt;

    /* Terminal state (devclass == DC$_TERM) */
    uint64_t            devchar;        /* VMS_TTC_* */
    uint32_t            width;
    uint32_t            page;

    /*
     * Every channel currently assigned to this device, by any process.
     * The device has to know this to answer $ALLOC: the oracle refuses
     * to allocate a device that another process merely has channels to
     * (ALLOCATE NLA0: -> %SYSTEM-W-DEVALLOC with the owner field still
     * empty and a reference count of 2).
     */
    struct list_head    chanlist;       /* of vms_channel.devlink */

    spinlock_t          lock;
};

/* A process's handle on a device. */
struct vms_channel {
    struct list_head    list;           /* in vms_proc->channels */
    struct list_head    devlink;        /* in vms_device->chanlist */
    uint32_t            chan;
    pid_t               owner_linux_pid;/* process holding this channel */
    struct vms_device   *dev;
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

/* Device table (executive-resident I/O database) */
long vms_ioctl_assign(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dassgn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getdvi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_devscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_ttsetmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_alloc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dalloc(struct vms_proc *proc, unsigned long arg);

/* Subsystem init/cleanup */
int vms_lock_init(void);
void vms_lock_cleanup(void);
void vms_eflag_init(void);
void vms_eflag_cleanup(void);
int vms_devtab_init(void);
void vms_devtab_cleanup(void);

/* Give back every channel a process holds (process teardown). */
void vms_proc_release_channels(struct vms_proc *proc);

/* Lock manager helpers */
void vms_proc_release_locks(struct vms_proc *proc);
void vms_proc_release_common_ef(struct vms_proc *proc);

#endif /* _VMS_INTERNAL_H */
