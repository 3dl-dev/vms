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
/*
 * Event-flag and memory statuses -- ORACLE-PINNED (vms-68c, 2026-07-30),
 * full transcript in docs/oracle/vax73-event-flags.md. Same node and same
 * two-method protocol as the block above (OpenVMS VAX V7.3, VAX1):
 *
 *   LIBRARY/EXTRACT=$SSDEF ... SYS$LIBRARY:STARLET.MLB + SEARCH
 *     $EQU  SS$_WASCLR      1
 *     $EQU  SS$_WASSET      9
 *     $EQU  SS$_ILLEFC    236
 *     $EQU  SS$_INSFMEM   292
 *     $EQU  SS$_UNASEFC   564
 *   F$MESSAGE round-trip
 *     1   -> %SYSTEM-S-NORMAL,   normal successful completion
 *     236 -> %SYSTEM-F-ILLEFC,   illegal event flag cluster
 *     292 -> %SYSTEM-F-INSFMEM,  insufficient dynamic memory
 *     564 -> %SYSTEM-F-UNASEFC,  unassociated event flag cluster
 *
 * FOUR VALUES HERE WERE WRONG, and the same oracle names what each of them
 * actually is:
 *     WASCLR  was 5   -> F$MESSAGE(5)  = %NONAME-?-NOMSG (not a status at all)
 *     ILLEFC  was 44  -> F$MESSAGE(44) = %SYSTEM-F-ABORT
 *     UNASEFC was 48  -> F$MESSAGE(48) = %SYSTEM-W-BADATTRIB
 *     INSFMEM was 44  -> %SYSTEM-F-ABORT, under a comment asserting that 44
 *                        "matches real VMS". It does not.
 *
 * This was latent only because NOTHING in the product called the event flag
 * facility (vms-2a8): src/libvms/syssvc/sys_event.c kept all 128 flags in
 * per-process memory, so no event-flag status ever crossed /dev/vms. Wiring
 * it is what makes these values load-bearing, which is why they are pinned
 * in the same branch.
 *
 * SS__WASCLR == SS__NORMAL == 1 IS VMS, NOT A COLLISION. $SSDEF gives both
 * the value 1 and F$MESSAGE(1) has exactly one rendering. A caller
 * distinguishes "was clear" from "was set" by testing SS$_WASSET, never by
 * expecting a WASCLR value distinct from success. Do not "fix" this back.
 */
#define SS__INSFMEM     292         /* insufficient dynamic memory (ssdef.h SS$_INSFMEM) */
#define SS__EXASTLM     0x00000038  /* AST quota exceeded */
#define SS__WASSET      9           /* flag/AST was enabled/set (ssdef.h SS$_WASSET) */
#define SS__WASCLR      1           /* flag/AST was disabled/clear (ssdef.h SS$_WASCLR) */
#define SS__ILLEFC      236         /* illegal event flag cluster (ssdef.h SS$_ILLEFC) */
#define SS__UNASEFC     564         /* unassociated common EFC (ssdef.h SS$_UNASEFC) */
/*
 * ============================================================
 * THE LOCK MANAGER YIELDS VMS CONDITION VALUES (vms-82a)
 * ============================================================
 * These six were a PRIVATE numbering -- 40/100/108/112/116/120 -- that no
 * VMS ever produced. The executive emitted them and kstat_to_ss(), running
 * in the CALLING PROCESS, mapped them onto the public ssdef.h constants.
 *
 * WHY THAT WAS A DEFECT AND NOT A SPELLING CHOICE. On real OpenVMS the
 * condition value IS what the lock manager yields; SS$_DEADLOCK is the
 * lock manager's answer, not a userspace rendering of it. Measured on the
 * old arrangement: mutating one case arm in sys_lock.c -- with ZERO
 * executive code changed -- moved what a public-API caller received from
 * 3594 to 2488, i.e. SS$_DEADLOCK became SS$_NOTQUEUED. That is a
 * DIFFERENT ANSWER, not a different spelling: a caller branching on the
 * status behaves differently. So part of the answer was decided outside
 * the executive, and the one facility cited as proof that OVMX does
 * executive residency properly (vms-ci.7, ENQ/DEQ -> kernel lock manager)
 * was the one still finishing its answer in userspace.
 *
 * NO NEW CONSTANT IS INVENTED HERE (Rule 8/Rule 10). Each value below is
 * the value src/libvms/include/ssdef.h ALREADY ships for that name, and
 * the pairing is the one kstat_to_ss() already asserted -- 100 <-> 
 * SS$_DEADLOCK, 108 <-> SS$_IVLOCKID, 116 <-> SS$_CVTUNGRANT and so on.
 * What changed is WHERE the value is produced, not WHAT it is. The
 * provenance of the numbers themselves is unchanged and is recorded above
 * the lock section of ssdef.h (single-lineage, not independently confirmed
 * against an official VSI $SSDEF extract); that remains open there and is
 * not settled, or worsened, by moving the value into the executive.
 *
 * SS__CANCELGRANT keeps its kernel name but carries SS$_CVTUNGRANT's
 * value: same concept (a queued conversion that could not be granted),
 * and that pairing is inherited from the mapping this replaces.
 * ============================================================
 */
#define SS__NOTQUEUED   2488        /* lock not queued (ssdef.h SS$_NOTQUEUED) */
#define SS__DEADLOCK    3594        /* deadlock detected (ssdef.h SS$_DEADLOCK) */
#define SS__IVLOCKID    8484        /* invalid lock ID (ssdef.h SS$_IVLOCKID) */
#define SS__SUBLOCKS    8492        /* sublocks still held (ssdef.h SS$_SUBLOCKS) */
#define SS__CANCELGRANT 8508        /* conversion cancelled (ssdef.h SS$_CVTUNGRANT) */
#define SS__VALNOTVALID 2544        /* value block not valid (ssdef.h SS$_VALNOTVALID) */

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
 * SS__NOTALLPRIV -- ORACLE-PINNED (vms-2b8).
 *
 * MEASURED on the reference lab OpenVMS VAX V7.3 node VAX1, 2026-07-30,
 * by the implementer of vms-2b8 (docs/oracle/vax73-privileges.md §1):
 *   $ WRITE SYS$OUTPUT "1664="+F$MESSAGE(1664)
 *   1664=%SYSTEM-W-NOTALLPRIV, not all requested privileges authorized
 *
 * This tree's src/libvms/include/ssdef.h carried 532, which the SAME
 * oracle disproves in the same session:
 *   $ WRITE SYS$OUTPUT "532="+F$MESSAGE(532)
 *   532=%SYSTEM-F-RESULTOVF, resultant string overflow
 * ssdef.h is corrected under this item to agree.
 *
 * SS__NOPRIV (36) was verified in the same session and already agreed:
 *   36=%SYSTEM-F-NOPRIV, insufficient privilege or object protection violation
 */
#define SS__NOTALLPRIV  1664

/*
 * Logical-name statuses (vms-d37). Values are this tree's existing
 * src/libvms/include/ssdef.h, NOT independently re-derived here, so the
 * executive and the runtime cannot drift apart -- same discipline as the
 * device-table block above.
 *
 *   SS$_SUPERSEDE 844  -- an existing name was replaced (ssdef.h, oracle
 *                         provenance recorded there)
 *   SS$_NOLOGNAM  444  -- no logical name match (ssdef.h, ORACLE-PINNED
 *                         vms-8019)
 *
 * SS__EXLNMQUOTA IS THE ONE VALUE NOT YET IN ssdef.h. It is ORACLE-PINNED,
 * not self-certified: docs/design-logical-name-placement.md §4.2 records the
 * vms-ln0 round-3 veracity adversary running F$MESSAGE(8780) on the reference
 * lab OpenVMS VAX V7.3 (lab-1, node vax1) and getting %SYSTEM-...-EXLNMQUOTA.
 * The coupled ssdef.h edit is tracked in vms-556. It is the status the
 * executive returns when the fixed arena is full -- VMS's "exceeded logical
 * name quota" condition, not "insufficient memory".
 */
#define SS__SUPERSEDE   844
#define SS__NOLOGNAM    444
#define SS__EXLNMQUOTA  8780        /* oracle-pinned lab-1 F$MESSAGE (design §4.2, vms-556) */

/*
 * Default privilege set for processes with no elevated credential.
 *
 * These are the two privileges OpenVMS grants essentially every user, so
 * they are what a process gets when the executive can prove nothing more
 * about it. The BIT POSITIONS are oracle-pinned (TMPMBX 15, NETMBX 20 --
 * docs/oracle/vax73-privileges.md §2), which is the correction: this
 * constant previously read (1<<7)|(1<<8) with the comment
 * "TMPMBX | NETMBX", and bits 7 and 8 are LOG_IO and GROUP. The
 * executive was handing out two privileges while naming two others.
 */
#define VMS_DEFAULT_PRIVS   (VMS_PRV_M_TMPMBX | VMS_PRV_M_NETMBX)

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
    pid_t               linux_pid;      /* Linux thread-group id == getpid(2)
                                         * (key). NOT a thread id: one PCB
                                         * per process, shared by its
                                         * threads -- see
                                         * vms_proc_find_or_err(). */
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
     * Authenticated user name (vms-2b8). Empty until an identity is
     * stamped by VMS_IOCTL_SETIDENT, which is the LOGINOUT path: a
     * process does not choose its user name any more than it chooses
     * its UIC. Read back by every other process through $GETJPI, which
     * is what makes it an identity rather than a self-description.
     *
     * LOCKING: username and uic are read by proc_fill_info() and
     * find_by_name() under vms_proc_hash_lock, and the privilege masks
     * are read and written by vms_access.c under mode_lock. An identity
     * is one indivisible fact -- a reader must never see this process's
     * new user name beside its old privilege mask -- so
     * vms_ioctl_setident() takes BOTH, hash_lock OUTER then mode_lock,
     * and that is the only place the two are held together. Nothing
     * takes them in the opposite order.
     */
    char                username[VMS_USERNAME_SIZE];

    /*
     * Reference to the backing PROCESS's struct pid -- task_tgid(), the
     * thread group's pid, NOT task_pid() (vms-9fc round 2). The PCB
     * belongs to the PROCESS, not to an open channel and not to a
     * thread, so it is not destroyed when /dev/vms is closed (notably
     * the implicit close at exec time) and not destroyed when one
     * thread of a multithreaded image exits. Liveness is tested through
     * this reference and dead entries are reaped lazily -- see
     * vms_proc_reap_dead().
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

    /*
     * The job's terminal (vms-d0b). "" until VMS_IOCTL_SETTERM records
     * one, which the executive only does from a channel this process
     * already holds to a device of class DC$_TERM -- so the name is a
     * device name out of the executive's own table, never a string the
     * process supplied.
     *
     * LOCKING: written and read under vms_proc_hash_lock, alongside
     * prcnam, uic and username. It is read by proc_fill_info(), which
     * every $GETJPI and $PROCSCAN row goes through.
     */
    char                terminal[VMS_DEVNAM_SIZE];

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
     * shareable mirrors the word the oracle prints in SHOW DEVICE/FULL's
     * status clause. It decides whether a channel confers ownership, so
     * it is not decoration. MEASURED, ~/vax OpenVMS VAX V7.3, node VAX2
     * (docs/oracle/vax73-terminal-device.md section 7):
     *   "Device NLA0: ... record-oriented device, shareable, mailbox
     *    device."                                  -> shareable
     *   "Terminal TTA0: ... is online, record-oriented device, carriage
     *    control."                                 -> not shareable
     */
    uint32_t            shareable;      /* 1 = "shareable" in the status clause */

    /*
     * OWNERSHIP AND ALLOCATION ARE TWO DIFFERENT THINGS, and both are
     * measured (docs/oracle/vax73-terminal-device.md section 7):
     *
     *  - A channel to a NON-shareable device that nobody owns makes the
     *    assigner the OWNER, with no allocation. TTA0: went from
     *    Owner "" / refcount 0 to Owner "SYSTEM" / refcount 1 on a bare
     *    OPEN/WRITE, and its status clause still said only "is online,
     *    record-oriented device, carriage control" -- no "allocated".
     *  - A channel to a SHAREABLE device confers nothing. The same DCL
     *    sequence on NLA0: left Owner "" with the reference count
     *    moving 2 -> 3 -> 2.
     *  - $ALLOC sets `allocated`, and it is the only thing that does.
     *  - Ownership without allocation ends when the owner returns its
     *    last channel (CLOSE -> Owner "", refcount 0) or dies
     *    (STOP CHANHOLD -> Owner "", refcount 0). An ALLOCATION outlives
     *    the channel until $DALLOC or the owner's death.
     *
     * refcnt is the device's "Reference count": one per assigned channel
     * plus one for an outstanding allocation. Implicit ownership costs
     * no reference (TTA0: one channel -> refcount 1, owned).
     */
    uint32_t            allocated;      /* 1 while $ALLOC'd to owner_* */
    uint32_t            owner_pid;      /* VMS pid of the owner, 0 = unowned */
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
     * The device has to know this to decide when implicit ownership
     * ends: it ends when the owner has no channel left, not when any
     * channel is returned.
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
/*
 * Register a process. Takes NO privilege argument (vms-2b8): the
 * authorized mask is derived from the task's real credentials inside,
 * never requested by the caller.
 */
/* The VMS process ID is assigned by the executive (vms-2b8), so there is
 * no vms_pid parameter to pass: read proc->vms_pid afterwards. */
struct vms_proc *vms_proc_register(pid_t pid, bool continue_identity);
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
long vms_ioctl_dlcefc(struct vms_proc *proc, unsigned long arg);

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
/* The job-to-terminal binding. Lives with the channel machinery because
 * its argument is a channel; the value it writes is process-table
 * state (struct vms_proc::terminal). */
long vms_ioctl_setterm(struct vms_proc *proc, unsigned long arg);

/* Process table (executive-resident PCB directory) */
long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setident(struct vms_proc *proc, unsigned long arg);

/* May `caller` read `target`'s identity? Oracle-pinned rule -- see the
 * definition in vms_proctab.c and docs/oracle/vax73-privileges.md §5. */
bool vms_proc_may_read(const struct vms_proc *caller,
                       const struct vms_proc *target);

/* Logical name tables (executive-resident LNM$SYSTEM/GROUP/JOB, vms-d37) */
struct file;
struct vm_area_struct;
long vms_ioctl_lnm_define(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_lnm_delete(struct vms_proc *proc, unsigned long arg);
/* Map the read-only logical-name arena into the caller (design §3.3). */
int vms_lnm_mmap(struct file *filp, struct vm_area_struct *vma);

/* Subsystem init/cleanup */
int vms_lock_init(void);
void vms_lock_cleanup(void);
void vms_eflag_init(void);
void vms_eflag_cleanup(void);
int vms_devtab_init(void);
void vms_devtab_cleanup(void);
int vms_lnm_init(void);
void vms_lnm_cleanup(void);

/* Give back every channel a process holds (process teardown). */
void vms_proc_release_channels(struct vms_proc *proc);

/* Lock manager helpers */
void vms_proc_release_locks(struct vms_proc *proc);
void vms_proc_release_common_ef(struct vms_proc *proc);

#endif /* _VMS_INTERNAL_H */
