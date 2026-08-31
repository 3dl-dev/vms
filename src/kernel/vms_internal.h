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
 * C-string vocabulary a shared-core facility relies on: a shared-core .c file
 * (under src/kernel-core) compiles against exec_* + this struct header ONLY and
 * may not include a
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
#define SS__DEVMOUNT    2684        /* device already mounted (ssdef.h SS$_DEVMOUNT) */
/*
 * SS__DEVNOTMOUNT (SS$_DEVNOTMOUNT == 2688, this tree's src/libvms/include/
 * ssdef.h value, single-lineage the same way SS__NOSUCHDEV above is). The
 * Files-11 ODS-2 ACP $MOUNT (vms_ioctl_acp_mount, vms-127) returns it when the
 * named unit's backing block device is NOT a genuine ODS-2 volume -- the home
 * block / SCB fail the codec's validation (bad "DECFILE11B  " format, wrong
 * structure level, or a checksum mismatch). REJECT-THE-MEDIA is an OVMX design
 * choice of an already-grounded status (Rule 8): VMS's own MOUNT reports an
 * unrecognized structure through the MOUNT-facility message set (MOUNT-F-
 * FILESTRUCT, not an SS$ condition), which this tree does not carry, so the ACP
 * answers "this device could not be mounted" -- SS$_DEVNOTMOUNT -- rather than
 * inventing a status it cannot cite. Labelled as such in vmsfs_acp.c.
 */
#define SS__DEVNOTMOUNT 2688        /* device not mounted / not a mountable volume */
/*
 * SS__ABORT (SS$_ABORT == 44, this tree's src/libvms/include/ssdef.h value,
 * single-lineage the same way SS__EXQUOTA / SS__ENDOFFILE below are). The BGn:
 * driver (vms-527) returns it when a host-kernel socket operation
 * (connect/send/recv) fails -- the same status src/libvms/syssvc/sys_qio.c's
 * synchronous read/write path already maps a failed I/O to. Not the connection-
 * specific SS$_LINKDISCON/SS$_CONNECFAIL family (not oracle-pinned in this tree
 * yet); reusing an already-grounded status rather than inventing one this tree
 * cannot cite (CLAUDE.md Rule 8), the same choice vms_mbx.c makes for SS$_MBFULL.
 */
#define SS__ABORT       44          /* I/O aborted (ssdef.h SS$_ABORT) */
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
 * Files-11 ACP file-open statuses (vms-204, epic vms-208). Single-lineage from
 * this tree's src/libvms/include/ssdef.h -- SS$_NOSUCHFILE == 2696 ("no such
 * file", the fail-honest answer when an IO$_ACCESS name/FID resolves to no
 * directory entry / header) and SS$_FILNOTACC == 2744 ("file not accessed", an
 * IO$_DEACCESS of a channel with no file accessed on it). Not re-derived; the
 * same values ssdef.h carries. (SS$_NOPRIV for a protection-denied open is
 * SS__NOPRIV, already defined above.)
 */
#define SS__NOSUCHFILE  2696        /* no such file (IO$_ACCESS resolve miss) */
#define SS__FILNOTACC   2744        /* file not accessed (IO$_DEACCESS w/o access) */
/*
 * SS__DEVICEFULL (SS$_DEVICEFULL == 2664): a PUBLIC STARLET SYSTEM-facility code
 * ("%SYSTEM-?-DEVICEFULL, device full"), added for the ACP IO$_WRITEVBLK
 * implicit-extend path (vms-c60) as the HONEST answer when a write past EOF
 * cannot allocate the shortfall from BITMAP.SYS -- never a fabricated success
 * (INV-6). Rule 8: a documented status-code number, not a lab-observed value.
 * The write-locked case (channel accessed read-only) returns the already-defined
 * SS__NOPRIV rather than a newly-introduced SS$_WRITLCK, to avoid an un-grounded
 * constant on an asserted path (the oracle-authentic SS$_WRITLCK is a labelled
 * follow-up, the same footing #633 used for its SS__DEVALLOC "busy" choice).
 */
#define SS__DEVICEFULL  2664        /* device full (extend cannot allocate) */
/*
 * SS__NOMOREFILES (SS$_NOMOREFILES == 2352, %X0930) -- ORACLE-PINNED (vms-a0b,
 * 2026-08-17). MEASURED on the reference lab OpenVMS VAX V7.3 node VAX1 by
 * assembling `.LONG SS$_NOMOREFILES` after `$SSDEF` and reading the resolved
 * absolute value off the MACRO/LIST listing (00000930 == 2352); cross-checked
 * against SS$_NOMOREDEV = 00000A58 = 2648 in the same run (matches this tree's
 * SS__NOMOREDEV exactly, confirming the method). src/libvms/include/ssdef.h
 * carries the same value + the full provenance. The Files-11 ODS-2 ACP's
 * IO$_ACPCONTROL wildcard directory search ($SEARCH) returns it when the
 * wildcard context is exhausted (VSI I/O User's Reference, "ACP-QIO Interface").
 */
#define SS__NOMOREFILES 2352        /* no more files (wildcard $SEARCH exhausted) */
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
 * SS__ENDOFFILE -- this tree's existing src/libvms/include/ssdef.h value
 * (SS$_ENDOFFILE == 2160), same single-lineage discipline as SS__EXQUOTA
 * above (not independently oracle-pinned here). The mailbox read path
 * returns it for a $QIO IO$M_NOW read that finds the mailbox empty: the
 * public VSI OpenVMS I/O User's Reference Manual (Mailbox Driver) documents
 * that a read specifying IO$M_NOW completes immediately, and an empty
 * mailbox completes such a read with an end-of-file status (vms-5df).
 */
#define SS__ENDOFFILE   2160

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
    uint32_t            req_csid;       /* cluster CSID this lock is held FOR.
                                         * 0 = a local process on THIS node owns
                                         * it (the common case). Non-zero = a
                                         * cross-node grant: the master holds this
                                         * lock on behalf of a REMOTE node whose
                                         * CSID this is (DLM epic vms-7fa rung 2,
                                         * vms-e8f1). Set from vms_enq_args
                                         * .owner_csid at creation; surfaced by
                                         * GET_RESMASTER.remote_holder_csid. */
    uint32_t            req_lkid;       /* the REMOTE requester's own lock handle
                                         * for a cross-node grant (vms-6ca, DLM
                                         * epic vms-7fa rung H5). 0 for a local
                                         * lock. Set from the wire ENQ's req_lkid
                                         * so a later deferred GRANT (sent when
                                         * this lock's release grants a queued
                                         * cross-node waiter) can name the
                                         * requester's ORIGINAL request. */
    uint32_t            parent_id;      /* the lkid of this lock's PARENT lock, or
                                         * 0 for a root (parentless) lock. Set from
                                         * vms_enq_args.parid at creation; reported
                                         * by GETLKI. RMS record locks (vms-0dd)
                                         * carry the file-access lock (vms-50e) as
                                         * parent, so a record lock is getlki-
                                         * visible UNDER its file lock. Every
                                         * existing $ENQ passes parid=0 (a root
                                         * lock), so this is purely additive. The
                                         * parent-child AUTO-RELEASE cascade is a
                                         * follow-on (vms-489), not wired here. */
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
     * Authorized JIB quota set (vms-14a). LOGINOUT copies the account's SYSUAF
     * quota cells here via VMS_IOCTL_SETIDENT, alongside username/uic/privs and
     * under the SAME hash_lock+mode_lock the rest of the identity is stamped
     * under -- one indivisible identity, never observed half-updated. quota is
     * meaningful only when quota_valid == 1; a process whose identity carried no
     * quota (a $CREPRC subprocess that did not re-read SYSUAF) has quota_valid
     * == 0 and proc_fill_info leaves VMS_PI_V_QUOTA clear (honest omission,
     * INV-6). OVMX shows the CONFIGURED quota; it does not ENFORCE/charge it
     * (enforcement is a separate facility -- vms-14a spec).
     */
    uint8_t             quota_valid;    /* 1 = quota below is sourced */
    struct vms_jib_quota quota;         /* authorized JIB quota set (SYSUAF) */

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

    /*
     * Hibernate/wake + async AST-delivery wakeup (vms-feb). $HIBER blocks the
     * process in the executive on hiber_wq; it is released when a $WAKE arrives
     * (wake_pending set by vms_ioctl_wake) OR when an AST becomes deliverable
     * (vms_ast_notify_arrival broadcasts hiber_wq after an entry is queued into
     * this process's ast[] -- $DCLAST, a mailbox write-attention write, or a
     * lock completion/blocking AST). This is what makes $HIBER interruptible by
     * asynchronous AST delivery instead of a bare pause(): a process B action
     * that queues an AST into process A's queue actually wakes A to run it
     * (CLAUDE.md Rule 9 / INV-6 -- cross-process, through the executive).
     *
     * wake_pending is a STICKY SINGLE BIT, matching VMS's PCB$V_WAKEPEN: a
     * $WAKE that precedes $HIBER makes the next $HIBER fall straight through,
     * and one pending wake satisfies exactly one $HIBER (VSI System Services
     * Reference, $WAKE/$HIBER). It is set under hiber_lock and consumed (test-
     * and-cleared) by vms_ioctl_hiber, which is the SAME lock hiber_wq is paired
     * with -- so a wake set under hiber_lock is never lost against a waiter that
     * dropped hiber_lock only inside exec_cv_wait.
     */
    wait_queue_head_t   hiber_wq;
    spinlock_t          hiber_lock;
    uint8_t             wake_pending;

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
     * INET pseudo-device channels (executive-resident BGn:, vms-527). A
     * separate list, same chan_lock and next_chan counter as the device and
     * mailbox channels above (vms_bg.h) -- a BG channel carries a host-kernel
     * `struct socket *` a generic device row has no field for, exactly as a
     * mailbox channel carries a message queue, so it gets its own per-process
     * binding (struct vms_bg_chan, src/kernel/vms_bg.c). Released at process
     * teardown by vms_bg_release_all().
     */
    struct list_head    bg_channels;    /* struct vms_bg_chan */

    /*
     * Files-11 (ODS-2) ACP file-class channels (vms-149, epic vms-208). A
     * separate list, same chan_lock and next_chan counter as the device,
     * mailbox and BG channels above -- a file channel is bound to a mounted
     * ODS-2 volume in the executive-global mount table, not a struct vms_device
     * row, so it gets its own per-process binding (struct vms_acp_chan,
     * src/kernel-core/vmsfs_acp.c). vms_ioctl_dassgn() (vms_devtab.c) falls back
     * to vms_acp_dassgn() for it; released at process teardown by
     * vms_acp_release_all().
     */
    struct list_head    file_channels;  /* struct vms_acp_chan */

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

    /*
     * Image completion status -- the executive half of $EXIT/$STATUS
     * (vms-f60d). When an activated image finishes (its port crt0's
     * main() returns, or it calls SYS$EXIT explicitly), IMGACT records
     * the returned VMS condition value here through VMS_IOCTL_SETEXIT.
     * That value IS the process's $STATUS: the full longword is the
     * condition value, bit<0> (STS$M_SUCCESS) is the success/fail bit
     * and bits<2:0> (STS$V_SEVERITY) are the severity, exactly as
     * $STATUS/$SEVERITY report them (VSI OpenVMS DCL Dictionary; System
     * Services Reference, $EXIT). It lives in the executive rather than
     * in the image's own memory for the same reason prcnam/username do:
     * so it survives the image and is a fact the invoking CLI (and an
     * authorized reader through VMS_IOCTL_GETEXIT) can observe, not a
     * value only the exiting image ever saw.
     *
     * has_exit_status distinguishes "recorded the condition value 0"
     * (a legal, even/warning-severity value) from "no image has exited
     * yet" -- a reader must never have to guess that from a zero, the
     * same discipline as vms_procinfo.redacted.
     *
     * Guarded by vms_proc_hash_lock, alongside prcnam/username/terminal:
     * VMS_IOCTL_GETEXIT reads another process's recorded status under
     * that lock (gated by vms_proc_may_read, like $GETJPI).
     */
    uint32_t            exit_status;      /* recorded image completion $STATUS */
    uint8_t             has_exit_status;  /* 1 once SETEXIT recorded a value */

    /*
     * /NOWAIT subprocess-exit completion registration (vms-e9a B1,
     * docs/design-libspawn-ovmx.md §3b). Lives on the CHILD's PCB: a parent
     * that spawned this subprocess /NOWAIT (LIB$SPAWN with efn/astadr) arms it
     * through VMS_IOCTL_SPAWN_NOTIFY, and vms_ioctl_setexit() delivers it -- set
     * the parent's completion event flag and/or queue the parent's completion
     * AST -- when THIS process records its exit status. One-shot: compl_armed is
     * cleared as the notification is delivered. All fields guarded by
     * vms_proc_hash_lock, the same lock exit_status is written under, so the
     * exit and its notification are one indivisible event.
     */
    uint8_t             compl_armed;      /* 1 = a parent awaits this proc's exit */
    uint8_t             compl_acmode;     /* access mode to queue the AST at */
    uint32_t            compl_parent_pid; /* VMS PID to notify on exit */
    uint32_t            compl_efn;        /* parent EF to set, or VMS_EF_NONE */
    uint64_t            compl_astadr;     /* parent completion AST routine (0=none) */
    uint64_t            compl_astprm;     /* parameter for the completion AST */

    /*
     * CLI invocation context -- the executive source for IMGACT's
     * cliflag / cli_util->get_command_line (vms-f60d, ovmx_activation.h).
     * cli_present is the cliflag: 1 iff this image was invoked from a
     * CLI (DCL). cli_command is the invoking DCL command line, length
     * cli_length (no trailing NUL required within the count; a NUL is
     * still kept for C readers). An image inherits both from its
     * invoking CLI's PCB at VMS_IOCTL_REGISTER_CONTINUE time
     * (vms_proc_continue_identity), the same inheritance path as
     * uic/username/privs -- so DCL sets the context once (SETCLI) and
     * every image it activates reads it back (GETCLI) from the
     * executive, never from a Linux env-var shim (conductor ruling,
     * INV-6). Same hash_lock as the identity fields.
     *
     * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): the 256-byte command-line
     * bound matches the classic 255-character DCL command line the
     * OpenVMS User's Manual documents; the byte layout of the ioctls
     * carrying it is OVMX's own (public docs give the semantics of a
     * CLI command line and $CLI callbacks, not a wire format).
     */
    uint8_t             cli_present;      /* cliflag: invoked from a CLI/DCL */
    uint16_t            cli_length;       /* length of cli_command in bytes */
    char                cli_command[VMS_CLI_CMDLINE_SIZE];

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
     * Ethernet backing (devclass == DC$_SCOM, vms-9d2). The host network
     * interface this LAN unit (ETH0:) was enumerated from at module init --
     * "eth0", "enp0s1", whatever the host names its primary non-loopback
     * Ethernet net device, sourced through the GENERIC netdev abstraction so it
     * is driver-agnostic (virtio-net in the QEMU runtime, e1000, a real NIC on
     * bare metal all land the same way). link_up is the carrier state observed
     * at enumeration. Empty/zero for every non-Ethernet device.
     *
     * NEVER SURFACED TO A VMS PROGRAM (INV-4). Unlike `backing` above (which a
     * process reads via vms_ioctl_disk_resolve to MOUNT a disk), there is no
     * ioctl that hands `netif` back: it is the executive's own record of which
     * real interface ETH0: fronts, for the IP stack layered over it later
     * (vms-527) to bind to. A VMS program sees ETH0:, never "eth0".
     */
    char                netif[VMS_NETIF_SIZE];
    uint32_t            link_up;

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
/*
 * inherit_identity: replace the fresh, credential-derived identity with the
 *   registering task's real_parent's executive identity (UIC/user name/privs).
 * share_pid: when inheriting, ALSO adopt the parent's VMS PID (image
 *   activation, _CONTINUE) rather than minting a fresh one (a subprocess,
 *   _SUBPROCESS). Ignored when inherit_identity is false.
 */
struct vms_proc *vms_proc_register(pid_t pid, bool inherit_identity,
                                   bool share_pid);
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

/*
 * Async AST-delivery wakeup (vms-feb). Called by every executive path that
 * queues an AST entry into `proc`'s ast[] -- $DCLAST, a mailbox write-attention
 * write (vms_mbx.c) and the lock manager's completion/blocking ASTs
 * (vms_lock.c) -- AFTER the entry is on the queue, to wake a $HIBER waiter so
 * it drains and runs the routine. Takes proc->hiber_lock internally; callers
 * must NOT hold it, and must not hold an ast_state->lock across the call.
 */
void vms_ast_notify_arrival(struct vms_proc *proc);

/*
 * Is an AST deliverable to `proc` at or outside `cur_mode`? Same bound as
 * vms_ioctl_deliverast (an enabled, non-empty queue at mode >= cur_mode). Used
 * by vms_ioctl_hiber as half of its wait predicate. Takes each ast_state->lock
 * briefly; caller holds proc->hiber_lock (order hiber_lock OUTER).
 */
int vms_ast_has_deliverable(struct vms_proc *proc, uint8_t cur_mode);

/* Hibernate / wake ($HIBER/$WAKE, executive-resident -- vms-feb). */
long vms_ioctl_hiber(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wake(struct vms_proc *proc, unsigned long arg);

/* Event flags (3c) */
/* Set a resolvable event flag on an ARBITRARY target proc and wake its waiters
 * -- the cross-process completion-EF primitive (vms-e9a B1) the /NOWAIT spawn
 * exit path uses to set the PARENT's flag from the CHILD's exit. Returns 0 if
 * set, -1 if `efn` does not resolve on `target`. */
int vms_ef_set_for(struct vms_proc *target, uint32_t efn);
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
/* In-kernel volume-synchronization lock for the Files-11 ACP (vms-233): an
 * EX-mode $ENQ on a per-volume resource, enqueued directly in the caller's
 * ioctl context (no /dev/vms round-trip). Release with a zero lkid is a no-op. */
uint32_t vms_lock_acp_vol_ex(struct vms_proc *proc, const char *resnam,
                             uint32_t *lkid_out);
uint32_t vms_lock_acp_vol_release(struct vms_proc *proc, uint32_t lkid);
/*
 * DLM resource-directory + mastering (vms-ci.5 DB). Read-only diagnostic:
 * report the directory node, the mastering node and the granted-lock count
 * for a resource name, so the local-master scaffolding is observable against
 * a real /dev/vms (tests/qemu/test_kmod_resdir.c) rather than asserted from a
 * hand-set structure.
 */
long vms_ioctl_get_resmaster(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dlm_member_depart(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dlm_get_granted(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dlm_enum_waits(struct vms_proc *proc, unsigned long arg);
/* Cluster membership crosses into the executive (rd vms-551): the SET/CLEAR/
 * GET handlers for the module-global membership block (vms_lock.c). SET/
 * CLEAR are scsd's local-ioctl populate path; GET is SHOW CLUSTER's read. */
long vms_ioctl_cluster_member_set(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_cluster_member_clear(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_cluster_member_get(struct vms_proc *proc, unsigned long arg);
/* vms-94c (DLM epic vms-7fa rung 1): the cross-node DLM RECEIVE handler and its
 * ioctl wrapper. Rung 1 delivers a decoded remote DLM request TO the handler,
 * which returns SS$_UNSUPPORTED (no fabricated cross-node grant, INV-6). */
uint32_t vms_lock_dlm_xnode_dispatch(struct vms_proc *proc,
                                     struct vms_dlm_xnode_args *req);
long vms_ioctl_dlm_xnode(struct vms_proc *proc, unsigned long arg);

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

/*
 * DLM directory membership vector (rd vms-1bba, the "DB" rung). A CONTROLLED,
 * STATIC configuration input -- the operator/harness supplies the ordered
 * cluster-member CSID vector at insmod time (module_param_array in the Linux
 * rind, src/kernel/vms_module.c), the same footing as vms_local_csid above.
 * dlm_directory_csid() hashes a resource name across THIS vector to pick the
 * directory node, so every node given the SAME vector resolves the SAME
 * directory (and, this rung, master) for a name.
 *
 * This is NOT the live membership feed from the connection manager / SCS
 * rejoin -- that is the 0.4 "DC" successor (and overlaps vms-2f3's rejoin
 * territory). This static vector is an honest controlled input for the DB
 * directory proof, never fabricated live cluster state. dlm_member_count == 0
 * (the default) means "not configured" -> the helpers fall back to a
 * cluster-of-one on the local CSID, preserving single-node behaviour. The
 * vector must be supplied in the SAME order on every node (the directory index
 * is position-based); the harness passes one canonical vector to all nodes.
 */
#define VMS_DLM_MAX_MEMBERS 16
extern uint32_t dlm_member_csids[VMS_DLM_MAX_MEMBERS];
extern int      dlm_member_count;

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
/*
 * Internal (non-ioctl) twin of disk_resolve for an in-executive caller: the
 * Files-11 ODS-2 ACP $MOUNT (vms-127) resolves a canonical disk-unit name to its
 * backing (major,minor) so it can read the home block/SCB and validate the
 * volume. SS$_NORMAL / SS$_NOSUCHDEV / SS$_IVDEVNAM. Defined in kernel-core.
 */
uint32_t vms_devtab_disk_backing(const char *devnam,
                                 uint32_t *major_out, uint32_t *minor_out);
/*
 * Record ONE genuine device I/O error against the disk unit whose backing block
 * device is (major,minor) -- the WRITER for the per-device errcnt SHOW ERROR and
 * F$GETDVI(...,"ERRCNT") read (rd vms-5f82). Called from the ACP block-I/O path
 * (kernel-core/vmsfs_acp.c) only on a real failure return from
 * exec_blockdev_read_block/write_block; never speculatively (INV-6). Defined in
 * kernel-core, on every substrate.
 */
void vms_devtab_note_io_error(uint32_t major, uint32_t minor);
#if defined(OVMX_KTEST_FAULT_INJECT)
/*
 * TEST-ONLY: arm block-I/O fault injection for the QEMU-test vms.ko so a real
 * ACP block op can be made to FAIL (exactly as a bad sector would) and the real
 * errcnt accounting observed. Compiled in ONLY when src/kernel/Makefile defines
 * OVMX_KTEST_FAULT_INJECT (never in the bootable executive). Armed from the
 * Linux module rind (vms_module.c module_param); count 0 = disarmed.
 */
void vmsfs_acp_test_arm_bdev_fault(uint32_t major, uint32_t minor, uint32_t count);
#endif
/* The job-to-terminal binding. Lives with the channel machinery because
 * its argument is a channel; the value it writes is process-table
 * state (struct vms_proc::terminal). */
long vms_ioctl_setterm(struct vms_proc *proc, unsigned long arg);

/* Process table (executive-resident PCB directory) */
long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setident(struct vms_proc *proc, unsigned long arg);
/*
 * $EXIT / $STATUS and CLI invocation context (vms-f60d) -- the executive
 * half of IMGACT's crt0 return path (ovmx_activation.h). SETEXIT records
 * the returned condition value as the process's image completion $STATUS;
 * GETEXIT reads it back (self, or another process gated by
 * vms_proc_may_read like $GETJPI). SETCLI records the invoking CLI command
 * line + cliflag; GETCLI reads the caller's own CLI context. See the PCB
 * fields exit_status/cli_command above and vms_proctab.c.
 */
long vms_ioctl_setexit(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getexit(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setcli(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getcli(struct vms_proc *proc, unsigned long arg);
/* /NOWAIT subprocess-exit completion arm (vms-e9a B1). */
long vms_ioctl_spawn_notify(struct vms_proc *proc, unsigned long arg);
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
 * Register a write-attention AST on a mailbox channel (vms-9003). When another
 * process writes to the mailbox, the executive queues the AST into THIS
 * process's AST queue -- real cross-process delivery, the same queue $DCLAST
 * and the lock manager use (vms_ast.c). One-shot; re-arm with another call.
 */
long vms_ioctl_mbx_set_wrtattn(struct vms_proc *proc, unsigned long arg);
/*
 * Release one mailbox channel by number, for vms_ioctl_dassgn()'s fallback
 * when `chan` is not in proc->channels. Returns 0 if `chan` named a
 * mailbox channel (released), or -ENOENT if it did not (so the generic
 * $DASSGN can report SS$_IVCHAN itself).
 */
int vms_mbx_dassgn(struct vms_proc *proc, uint32_t chan);
/* Give back every mailbox channel a dying process holds. */
void vms_mbx_release_all(struct vms_proc *proc);

/*
 * Files-11 (ODS-2) ACP -- channel + mount front-end (vms-149, epic vms-208;
 * src/kernel-core/vmsfs_acp.c). $MOUNT/$DISMOUNT of an ODS-2 volume into the
 * executive-global mounted table, and $ASSIGN of a file-class channel bound to
 * a mounted volume. See vms_acp.h and docs/design-files11-acp-executive.md §4.
 */
long vms_ioctl_acp_mount(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_dmount(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_assign(struct vms_proc *proc, unsigned long arg);
/*
 * IO$_ACCESS / IO$_DEACCESS (vms-204): open a file by name or by FID on a
 * file-class channel, building its VBN->LBN window; release it. See vms_acp.h
 * and docs/design-files11-acp-executive.md §4.2.
 */
long vms_ioctl_acp_access(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_deaccess(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_acpcontrol(struct vms_proc *proc, unsigned long arg);
/*
 * IO$_READVBLK / IO$_WRITEVBLK (vms-c60): virtual-block transfer on an accessed
 * file channel through its window; a write past EOF triggers an implicit extend
 * (BITMAP.SYS allocation + FH2 grow). See vms_acp.h and design §4.2.
 */
long vms_ioctl_acp_readvb(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_writevb(struct vms_proc *proc, unsigned long arg);
/*
 * IO$_CREATE / IO$_DELETE / IO$_MODIFY (vms-5303): the ACP file-operation
 * umbrella -- allocate a file header from INDEXF.SYS, enter a versioned
 * directory record, deallocate a header + its blocks, extend/truncate/write
 * attributes. func-dispatched on VMS_ACP_FOP_*; see vms_acp.h.
 */
long vms_ioctl_acp_fileop(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_getvol(struct vms_proc *proc, unsigned long arg);  /* vms-e6f: $GETDVI volume items */
/*
 * Release one file-class channel by number, for vms_ioctl_dassgn()'s fallback
 * when `chan` is neither a device nor a mailbox channel. Returns 0 if `chan`
 * named a file channel (released), or -ENOENT if it did not.
 */
int vms_acp_dassgn(struct vms_proc *proc, uint32_t chan);
/* Give back every file-class channel a dying process holds. */
void vms_acp_release_all(struct vms_proc *proc);
void vms_acp_init(void);
void vms_acp_cleanup(void);

/*
 * INET pseudo-device (executive-resident BGn:, vms-527). The BGn: driver is
 * Linux host-kernel-socket glue (src/kernel/vms_bg.c), so unlike the mailbox
 * (which lives in the substrate-agnostic kernel-core) it is NOT called from
 * kernel-core: vms_module.c dispatches its ioctls and calls vms_bg_release_all
 * at process teardown directly, keeping kernel-core free of the host socket
 * API.
 */
long vms_ioctl_bg_create(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_setmode(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_setmode_icmp(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_connect(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_send(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_recv(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_deaccess(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_dassgn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_pollfd(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_getname(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_sockopt(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_bind(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_listen(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_accept(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_bg_materialize_fd(struct vms_proc *proc, unsigned long arg);
/* Give back every BG channel (and its host socket) a dying process holds. */
void vms_bg_release_all(struct vms_proc *proc);
/*
 * Copy PARENT's open BGn: channels into a just-registering CHILD PCB (executive
 * fork/exec inheritance, vms-3bf) -- the fd-inheritance analogue: same channel
 * NUMBERs, the one host socket SHARED by reference. Caller holds
 * vms_proc_hash_lock (keeps parent alive); child is not yet published.
 */
void vms_bg_inherit(struct vms_proc *child, struct vms_proc *parent);

/*
 * Eager fork-time BG channel inheritance (vms-0cd), Linux rind
 * (src/kernel/vms_bg_forkinherit.c). vms_proc_capture_channels_for_task lives in
 * vms_module.c (it owns the PCB hash); the rest are the rind's tracepoint machinery.
 */
struct task_struct;
struct list_head;
int  vms_proc_capture_channels_for_task(struct task_struct *parent_task,
                                        struct list_head *out, uint32_t *out_next_chan);
int  vms_bg_forkinherit_init(void);              /* register the fork/exit hooks */
void vms_bg_forkinherit_exit(void);              /* unregister + drain pending records */
int  vms_bg_forkinherit_consume(struct vms_proc *child);  /* adopt this task's fork record, if any */

/* Subsystem init/cleanup */
int vms_lock_init(void);
void vms_lock_cleanup(void);
void vms_eflag_init(void);
void vms_eflag_cleanup(void);
int vms_devtab_init(void);
void vms_devtab_cleanup(void);
/*
 * Enter ONE disk unit the SUBSTRATE enumerated, for a substrate whose disks the
 * shared /dev/vd* probe cannot name (rd vms-618 -- NetBSD/vax MSCP units). Not
 * called on Linux, where vms_devtab_probe_disks() does the enumeration; declared
 * here so the shared facility source keeps one prototype on every substrate.
 */
int vms_devtab_add_disk(const char *devnam, const char *backing,
                        uint32_t backing_major, uint32_t backing_minor);
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
