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
/*
 * ctype (isalnum/toupper) and the kstrtox number parsers (kstrtouint) are the
 * C-string vocabulary a shared-core facility relies on: a src/kernel-core/*.c
 * file compiles against exec_* + this struct header ONLY and may not include a
 * <linux/…> header of its own, so the Linux vms_internal.h -- the Linux
 * substrate's vocabulary header, the counterpart of the NetBSD twin's
 * <sys/systm.h> -- supplies them here. vms_mbx.c (rd vms-a88, Phase E) is the
 * first core facility to need them for its MBAn: device-name normalization;
 * vms_proctab.c (rd vms-846b, Phase F) adds the string.h spellings
 * (memcpy/memset/strncmp/strncpy) it uses on process names and user names.
 */
#include <linux/ctype.h>
#include <linux/kstrtox.h>
#include <linux/string.h>

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
 * SS__UNSUPPORTED -- this tree's existing src/libvms/include/ssdef.h value
 * (SS$_UNSUPPORTED == 2296; also src/libvmssys/vms_errno.h), NOT independently
 * re-derived here, same discipline as the device-table block above so the
 * executive and the runtime cannot drift apart. The kernel DLM scaffolding
 * (vms-ci.5 DB) returns it for the paths that are honestly 0.4, not yet built:
 * an $ENQ whose resource DIRECTORY or MASTER resolves to a REMOTE node, which
 * would have to be forwarded over the VMS$VAXcluster VC (DC) or driven through
 * remastering (DD). It is the honest "not built yet" answer -- never a
 * fabricated remote grant (INV-6 spirit).
 */
#define SS__UNSUPPORTED 2296        /* unsupported operation (ssdef.h SS$_UNSUPPORTED) */

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
 * SS__EXQUOTA -- this tree's existing src/libvms/include/ssdef.h value
 * (SS$_EXQUOTA == 28), NOT independently re-derived here, same discipline
 * as the device-table block above. ssdef.h carries no oracle citation for
 * it (a pre-existing, single-lineage value); mailboxes (vms-d44) reuse it
 * for "no room for this message" -- both a per-message size over maxmsg
 * and the aggregate bufquo being full -- rather than inventing a new
 * status this tree cannot oracle-pin (real VMS's SS$_MBFULL is not yet
 * measured against any reference lab; see src/kernel/vms_mbx.c).
 */
#define SS__EXQUOTA     28

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

/*
 * The SYSTEM process's CONSTRUCTED identity (vms-a17e).
 *
 * On OpenVMS, EXEC_INIT constructs the system process's identity; LOGINOUT
 * is SYSUAF's FIRST reader. Until this item OVMX inverted that: a userspace
 * process (src/ovmx_provision/ovmx_provision.c) read SYSUAF's SYSTEM record
 * and asked the executive to stamp it via VMS_IOCTL_SETIDENT, which is the
 * exact shape reserved for LOGINOUT authenticating an arbitrary user. The
 * fix follows the OPA0: precedent in vms_devtab.c: the executive creates
 * the fact itself, from constants it owns, rather than trusting a
 * caller-supplied value that happens to have come from a file read.
 * vms_ioctl_establish_system() (vms_proctab.c) is the ioctl that hands a
 * process this identity -- note it takes NO username/uic/privs arguments,
 * unlike VMS_IOCTL_SETIDENT: there is nothing for a caller to supply.
 *
 * UIC [1,4] -- ORACLE-PINNED, not invented here. AUTHORIZE's own SYSUAF.DAT
 * is owned by UIC [1,4] (docs/oracle/vax73-authorize-privilege.md: "owner
 * UIC [1,4]", from the reference lab's OpenVMS VAX V7.3 AUTHORIZE probe).
 * src/libvms/syssvc/sys_security.c's uic_is_system() cites the identical
 * fact ("the SYSTEM account's real UIC [1,4]"), and
 * tools/vmsfs_master.c's MASTER_UIC_GROUP/MASTER_UIC_MEMBER encode the same
 * pair for the same account. Three independent sites, one number.
 *
 * Privileges ALL -- OpenVMS ships its default SYSTEM account with every
 * privilege (OpenVMS System Manager's Manual, default UAF template); OVMX's
 * own distro/rootfs/.../SYSUAF.DAT SYSTEM record carries PRIVILEGES=ALL,
 * which is the fact this constant constructs rather than a file OVMX reads
 * at boot to learn it. VMS_PRV_M_SYSTEM_ALL mirrors userspace's PRV$M_ALL
 * (src/libvms/include/prvdef.h) -- both are simply "every bit", so unlike
 * VMS_PRV_M_ENFORCED this needs no _Static_assert bit-position agreement
 * with that header.
 */
#define VMS_SYSTEM_UIC          ((1u << 16) | 4u)   /* [1,4] */
#define VMS_PRV_M_SYSTEM_ALL    (~(uint64_t)0)

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
    uint8_t             acmode;         /* access mode $ENQ was issued from
                                         * (0-3). NOT a lock mode (see
                                         * granted_mode above). Image rundown
                                         * (vms-68f.v) dequeues locks owned at
                                         * USER mode; inner-mode locks are
                                         * process-permanent. See
                                         * vms_proc_rundown_locks(). */
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

    /*
     * DLM directory + mastering (vms-ci.5 DB, LOCAL scaffolding).
     *
     * On OpenVMS every resource is MASTERED on one node, found via a
     * DIRECTORY node reached by hashing the resource name (IDSM lock-
     * management chapter, "directory lookups" -- mined transcript
     * ch6-part02, pp. 6-18..6-35; and docs/design-cluster-node.md §5).
     * dir_csid is the CSID of the directory node for this name; master_csid
     * is the CSID of the node that masters the resource, 0 until it is
     * mastered on first use. Both are resolved and read under `lock` above.
     *
     * LOCAL SCAFFOLDING: the cluster is a stub-of-one, so both resolve to
     * vms_local_csid and the enqueue grants through the existing single-node
     * lock manager. Forwarding an enqueue to a REMOTE directory/master over
     * the VMS$VAXcluster VC (DC) and dynamic remastering on state transitions
     * (DD) are 0.4 -- the enqueue path returns SS$_UNSUPPORTED for a non-local
     * directory/master rather than fabricating a remote answer (INV-6 spirit).
     */
    uint32_t            dir_csid;       /* directory node CSID for `name` */
    uint32_t            master_csid;    /* mastering node CSID; 0 = unmastered */
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
     * The JOB this process belongs to (vms-aba, LNM$JOB residency). A VMS
     * job is a top-level process (an interactive login, or a detached
     * process) plus every subprocess it spawns -- SPAWN's child stays in
     * the parent's job (OpenVMS DCL Dictionary, SPAWN; System Services
     * Reference, $CREPRC's JOBCTL parameter). job_id is set exactly once,
     * at registration (see vms_proc_parent_job_id() in vms_module.c): a
     * task whose real parent is ALREADY a registered VMS process inherits
     * that parent's job_id -- true whether this registration is an image-
     * activation CONTINUE of the very same VMS process, or a genuinely new
     * PCB (a SPAWNed subprocess). A task with no registered parent (the
     * top of a job tree: an interactive session's first process, a
     * detached process, PID 1) becomes its own job root: job_id is set to
     * its own freshly assigned vms_pid, exactly as a new VMS job's ID is
     * the PID of the process that started it. NEVER supplied by the
     * caller -- derived from task_struct ancestry, the same discipline as
     * uic.
     */
    uint32_t            job_id;

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

    /*
     * Controlled mode-transition pairing (vms-68f.iii, in-process image
     * activation foundation, increment (iii) -- docs/design-in-process-
     * activation.md Part II §A.1.3, §A.2.3). VMS_IOCTL_ENTER_IMAGE sets
     * image_active=1 and records the mode it descended FROM in
     * pre_image_mode; VMS_IOCTL_IMAGE_RUNDOWN requires image_active==1,
     * restores current_mode to pre_image_mode, and clears image_active.
     * That guard -- not current_mode alone -- is what makes RUNDOWN a
     * PAIRED return rather than a second way to request an arbitrary mode:
     * a process that never legitimately descended (image_active==0) cannot
     * manufacture a return leg, and a process that already returned cannot
     * replay one. Both fields are read and written entirely under
     * mode_lock, alongside current_mode itself -- no separate lock, because
     * the three are one state machine and must never be observed
     * half-updated relative to each other.
     */
    uint8_t             image_active;     /* 1 while a controlled descent is open */
    uint8_t             pre_image_mode;   /* mode to restore on IMAGE_RUNDOWN */

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
     * Mailbox channels (executive-resident MBAn:, vms-d44). A separate
     * list from `channels` above because a mailbox is not a
     * struct vms_device row (see src/kernel/vms_mbx.c's header) -- but the
     * SAME channel-number space: mailbox channels are also drawn from
     * next_chan under chan_lock, exactly like device channels, because on
     * real VMS a process's channels are one number space regardless of
     * device kind. vms_ioctl_dassgn() (vms_devtab.c) checks `channels`
     * first and falls back to this list, so $DASSGN is one ioctl for
     * either kind.
     */
    struct list_head    mbx_channels;   /* struct vms_mbx_chan */

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

    /*
     * P0 program-region extent (vms-68f.i, in-process image activation
     * foundation). [p0_base, p0_limit) is the VA range VMS_IOCTL_P0_MAP
     * last registered for this process; both zero while no image is
     * mapped into P0. See the struct vms_procinfo comment in
     * vms_ioctl.h for what this ioctl pair does and does not claim.
     *
     * A dedicated lock rather than mode_lock or hash_lock: this is
     * accounting for a fact userspace already established (the P0
     * window mmap), not a decision the executive is enforcing, and it
     * has no ordering dependency on either of those locks' contents.
     */
    uint64_t            p0_base;
    uint64_t            p0_limit;
    spinlock_t          p0_lock;

    /*
     * P1 control-region extent (vms-68f.ii, in-process image activation
     * foundation, increment (ii)). [p1_base, p1_limit) is the VA range
     * VMS_IOCTL_P1_MAP last registered for this process; zero until
     * registered. See the struct vms_procinfo comment in vms_ioctl.h for
     * what this ioctl does and does not claim.
     *
     * A DEDICATED LOCK, DELIBERATELY SEPARATE FROM p0_lock -- this is the
     * mechanism, not decoration, behind "P0 deleted on rundown, P1
     * survives": vms_ioctl_p0_unmap() (vms_p0.c) takes p0_lock and touches
     * only p0_base/p0_limit, so it cannot observe or clear p1_base/
     * p1_limit even by accident of lock scope. A shared lock would not by
     * itself break the invariant, but a shared CLEAR PATH would -- keeping
     * the fields under separate locks makes that a structural fact rather
     * than a discipline someone has to remember while editing vms_p0.c.
     */
    uint64_t            p1_base;
    uint64_t            p1_limit;
    spinlock_t          p1_lock;

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
     * Disk backing (devclass == DC$_DISK, vms-3e8). The Linux block device
     * this unit was enumerated from at module init -- "vda" for DKA0:, "vdb"
     * for DKA100:, and so on. Empty and zero for every non-disk device (the
     * console has no backing block device). This is what lets a process
     * resolve a VMS unit to the block device it must open (MOUNT, vms-651)
     * without scanning /sys/block itself: the fact lives here, in the
     * executive (CLAUDE.md Rule 11). Set once at init, before /dev/vms
     * exists, and read (under `lock`) by vms_ioctl_disk_resolve().
     */
    char                backing[VMS_BACKING_SIZE];
    uint32_t            backing_major;
    uint32_t            backing_minor;

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
    uint8_t             acmode;         /* access mode $ASSIGN was issued from
                                         * (0-3). Image rundown (vms-68f.v)
                                         * deassigns channels owned at USER
                                         * mode; inner-mode channels are
                                         * process-permanent. See
                                         * vms_proc_rundown_channels(). */
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
/* Controlled mode-transition pair (vms-68f.iii): the CHMx/REI-equivalent
 * DCL uses to enter an image (Supervisor -> User) and to run it down
 * (User -> Supervisor). See vms_ioctl.h's VMS_IOCTL_ENTER_IMAGE/
 * VMS_IOCTL_IMAGE_RUNDOWN comment for why this is not just SETMODE twice. */
long vms_ioctl_enter_image(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_image_rundown(struct vms_proc *proc, unsigned long arg);
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
/*
 * DLM resource-directory + mastering (vms-ci.5 DB). Read-only diagnostic:
 * report the directory node, the mastering node and the granted-lock count
 * for a resource name, so the local-master scaffolding is observable against
 * a real /dev/vms (tests/qemu/test_kmod_resdir.c) rather than asserted from a
 * hand-set structure.
 */
long vms_ioctl_get_resmaster(struct vms_proc *proc, unsigned long arg);

/*
 * This node's cluster system ID (CSID) for the DLM (vms-ci.5 DB).
 *
 * LOCAL SCAFFOLDING: a single-node "cluster of one". The value is an OVMX
 * local default (an insmod module parameter, default non-zero -- 0 is
 * reserved for "unmastered"); the real CSID is assigned by the connection
 * manager at cluster join, which will feed it in 0.4 (vms-ci.5 DC). It is
 * never a claim of a VMS-authentic CSID layout (CLAUDE.md Rule 8).
 */
extern uint32_t vms_local_csid;

/* Device table (executive-resident I/O database) */
long vms_ioctl_assign(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dassgn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getdvi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_devscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_ttsetmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_alloc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dalloc(struct vms_proc *proc, unsigned long arg);
/*
 * Resolve a DISK unit to its backing Linux block device (vms-3e8). Read-only:
 * reports the vda/vdb/... name and dev_t the executive enumerated the unit
 * from at module init, so a process (MOUNT, vms-651) need never scan
 * /sys/block itself. SS$_NOSUCHDEV / SS$_IVDEVNAM as in the header.
 */
long vms_ioctl_disk_resolve(struct vms_proc *proc, unsigned long arg);
/* The job-to-terminal binding. Lives with the channel machinery because
 * its argument is a channel; the value it writes is process-table
 * state (struct vms_proc::terminal). */
long vms_ioctl_setterm(struct vms_proc *proc, unsigned long arg);

/* Process table (executive-resident PCB directory) */
long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setident(struct vms_proc *proc, unsigned long arg);
/* Construct the SYSTEM identity onto the caller (vms-a17e) -- the
 * OPA0:-style counterpart to vms_ioctl_setident() that takes no
 * caller-supplied username/uic/privs; see VMS_SYSTEM_UIC's comment. */
long vms_ioctl_establish_system(struct vms_proc *proc, unsigned long arg);

/* May `caller` read `target`'s identity? Oracle-pinned rule -- see the
 * definition in vms_proctab.c and docs/oracle/vax73-privileges.md §5. */
bool vms_proc_may_read(const struct vms_proc *caller,
                       const struct vms_proc *target);

/*
 * P0 program region (vms-68f.i, in-process image activation foundation).
 * Records/clears [p0_base, p0_limit) for the calling process; see
 * vms_p0.c and the struct vms_p0_args comment in vms_ioctl.h.
 */
long vms_ioctl_p0_map(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_p0_unmap(struct vms_proc *proc, unsigned long arg);

/*
 * P1 control region (vms-68f.ii, in-process image activation foundation,
 * increment (ii)). Records [p1_base, p1_limit) for the calling process;
 * no unmap -- see vms_p1.c and the struct vms_p1_args comment in
 * vms_ioctl.h for why P1 has no rundown counterpart.
 */
long vms_ioctl_p1_map(struct vms_proc *proc, unsigned long arg);

/* Logical name tables (executive-resident LNM$SYSTEM/GROUP/JOB, vms-d37) */
long vms_ioctl_lnm_define(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_lnm_delete(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_lnm_getscope(struct vms_proc *proc, unsigned long arg);
/*
 * Arena accessors for the host char device's mmap handler (vms-d61). The
 * substrate-agnostic facility (src/kernel-core/vms_lnm.c) owns and writes the
 * arena; the Linux rind's vms_lnm_mmap (vms_module.c) reads the base+size back
 * through these and does the mmap-time mapping (remap_vmalloc_range + clearing
 * VM_MAYWRITE) itself, so the mm coupling never enters the portable facility.
 * vms_lnm_arena_base() returns NULL until vms_lnm_init() has run.
 */
void *vms_lnm_arena_base(void);
size_t vms_lnm_arena_size(void);

/* Mailboxes (executive-resident MBAn:, vms-d44) */
long vms_ioctl_mbx_create(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_assign(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_write(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_read(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_mbx_delmbx(struct vms_proc *proc, unsigned long arg);
/*
 * Release one mailbox channel by number, for vms_ioctl_dassgn()'s fallback
 * when `chan` is not in proc->channels. Returns 0 if `chan` named a
 * mailbox channel (released), or -ENOENT if it did not (so the generic
 * $DASSGN can report SS$_IVCHAN itself).
 */
int vms_mbx_dassgn(struct vms_proc *proc, uint32_t chan);
/* Give back every mailbox channel a dying process holds. */
void vms_mbx_release_all(struct vms_proc *proc);

/* Subsystem init/cleanup */
int vms_lock_init(void);
void vms_lock_cleanup(void);
void vms_eflag_init(void);
void vms_eflag_cleanup(void);
int vms_devtab_init(void);
void vms_devtab_cleanup(void);
int vms_lnm_init(void);
void vms_lnm_cleanup(void);
void vms_mbx_init(void);
void vms_mbx_cleanup(void);

/* Give back every channel a process holds (process teardown). */
void vms_proc_release_channels(struct vms_proc *proc);

/* Lock manager helpers */
void vms_proc_release_locks(struct vms_proc *proc);
void vms_proc_release_common_ef(struct vms_proc *proc);

/*
 * Image rundown (vms-68f.v, SYS$RUNDWN image-scoped release --
 * docs/design-in-process-activation.md Part II §A.2.1, §A.6.1). Each releases
 * ONLY this process's resources owned at access mode >= min_acmode (numerically
 * greater == less privileged == outer), leaving process-permanent (inner-mode)
 * state intact. Image rundown passes min_acmode = PSL_C_USER, so exactly the
 * USER-mode (image) resources are released and everything a supervisor/exec/
 * kernel-mode context owns survives -- the "P0 dies, P1 survives" split at the
 * resource level (the process-permanent P1 extent is under its own lock and is
 * never touched here). Grounding per resource class: docs/oracle/
 * image-rundown-resource-classes.md. */
void vms_proc_rundown_channels(struct vms_proc *proc, uint8_t min_acmode);
void vms_proc_rundown_locks(struct vms_proc *proc, uint8_t min_acmode);
void vms_proc_rundown_asts(struct vms_proc *proc, uint8_t min_acmode);

#endif /* _VMS_INTERNAL_H */
