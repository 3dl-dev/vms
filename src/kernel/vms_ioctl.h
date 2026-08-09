/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_ioctl.h - Shared ioctl definitions for the VMS kernel module
 *
 * This header is used by both the kernel module and userspace code.
 * It defines all ioctl numbers and the data structures passed between
 * userspace and kernel.
 */

#ifndef _VMS_IOCTL_H
#define _VMS_IOCTL_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
/* ioctl macros for userspace -- use system macros if already defined */
#ifndef _IO
#define _IO(type, nr)           (((type) << 8) | (nr))
#endif
#ifndef _IOR
#define _IOR(type, nr, size)    (((2U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#ifndef _IOW
#define _IOW(type, nr, size)    (((1U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#ifndef _IOWR
#define _IOWR(type, nr, size)   (((3U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#endif

#define VMS_IOC_MAGIC 'V'

/* ================================================================
 * Access Mode (3a)
 * ================================================================ */

#define PSL_C_KERNEL    0
#define PSL_C_EXEC      1
#define PSL_C_SUPER     2
#define PSL_C_USER      3

struct vms_mode_args {
    uint8_t  mode;          /* target access mode (0-3) */
    uint8_t  pad[3];
    uint32_t status;        /* return: SS$_ status */
};

/* ================================================================
 * Privilege mask bits -- ONE definition, shared by the executive and
 * by userspace (vms-2b8).
 *
 * ORACLE-PINNED. Source: the reference lab OpenVMS VAX V7.3 node VAX1
 * (~/vax/cluster), via documented tool output --
 *   $ ANALYZE/SYSTEM
 *   SDA> READ SYS$SYSTEM:SYSDEF.STB
 *   SDA> EVALUATE PRV$V_<name>
 * PRV$V_ symbols live in SYSDEF.STB and evaluate to the BIT POSITION.
 * See docs/oracle/vax73-privileges.md for the verbatim transcript.
 *
 * WHY THIS BLOCK EXISTS AT ALL. Before vms-2b8 this tree carried FOUR
 * disagreeing privilege-bit tables, and three of them were wrong:
 *
 *   src/libvms/include/prvdef.h   correct (agrees with the oracle)
 *   src/kernel/vms_access.c       PRV_M_SETPRV = bit 5   -> that is DETACH
 *   src/kernel/vms_internal.h     "TMPMBX|NETMBX" = bits 7,8 -> LOG_IO|GROUP
 *   src/vmsdcl/dcl_cmd_show.c     its own table, ~10 bits wrong
 *
 * A privilege mask that means different things in different files is
 * not an access control system: the executive was checking DETACH and
 * calling it SETPRV, and handing out LOG_IO|GROUP while reporting
 * TMPMBX|NETMBX. Every one of those files now derives its bits from
 * here, and src/libvms/prv_agreement.c -- a translation unit built into
 * LIBVMS$SHR by the default target, which includes this header and
 * prvdef.h together -- static-asserts the two tables agree, so the
 * disagreement cannot come back silently. Change a value below and that
 * file stops compiling until prvdef.h is changed to match.
 * ================================================================ */

#define VMS_PRV_V_CMKRNL     0
#define VMS_PRV_V_CMEXEC     1
#define VMS_PRV_V_SYSNAM     2
#define VMS_PRV_V_GRPNAM     3
#define VMS_PRV_V_DETACH     5
#define VMS_PRV_V_LOG_IO     7
#define VMS_PRV_V_GROUP      8
#define VMS_PRV_V_PRMMBX    11  /* create permanent mailbox (vms-d44; mirrors
                                 * the already oracle-pinned bit position this
                                 * tree carries in src/libvms/include/prvdef.h's
                                 * PRV$V_PRMMBX -- not independently re-derived
                                 * here, same discipline as the other bits in
                                 * this block. */
#define VMS_PRV_V_PSWAPM    12
#define VMS_PRV_V_SETPRI    13
#define VMS_PRV_V_SETPRV    14
#define VMS_PRV_V_TMPMBX    15
#define VMS_PRV_V_WORLD     16
#define VMS_PRV_V_OPER      18
#define VMS_PRV_V_NETMBX    20
#define VMS_PRV_V_SYSPRV    28
#define VMS_PRV_V_BYPASS    29
#define VMS_PRV_V_GRPPRV    34

/*
 * SYSNAM (2) / GRPNAM (3) / GRPPRV (34) -- ORACLE-PIN NOTE (vms-5b7).
 *
 * docs/oracle/vax73-privileges.md section 2's EVALUATE transcript did not
 * include these three symbols (the session that produced it queried the
 * bits it needed at the time), so they are not re-derivations of a live
 * SDA read the way CMKRNL/CMEXEC/DETACH/LOG_IO/GROUP above are. They ARE
 * independently grounded two ways:
 *
 *   1. Section 4 of that same document transcribes a real `SHOW PROCESS/
 *      PRIVILEGES` from VAX1 naming SYSNAM ("may insert in system logical
 *      name table"), GRPNAM ("may insert in group logical name table") and
 *      GRPPRV ("may access group objects via system protection") as real,
 *      distinct VAX 7.3 privileges with exactly these descriptions -- so
 *      their EXISTENCE and MEANING are oracle-observed, only their bit
 *      POSITIONS are not from that transcript.
 *   2. The bit positions themselves are public $PRVDEF documentation (VSI
 *      OpenVMS System Services Reference Manual, OpenVMS Guide to System
 *      Security) and have been invariant across VAX/Alpha/Itanium/x86 for
 *      exactly these three low bits. They are also the ONLY values that
 *      fit the gaps the oracle-confirmed neighbors above leave open: with
 *      CMKRNL=0 and CMEXEC=1 confirmed, and DETACH=5 confirmed, bits 2-4
 *      are unassigned to anything else the oracle named, and 2/3 (5 CANNOT
 *      be SYSNAM or GRPNAM -- it is oracle-confirmed DETACH) are exactly
 *      where public $PRVDEF puts SYSNAM/GRPNAM. Likewise GRPPRV=34 is the
 *      only unclaimed slot between oracle-confirmed BYPASS=29 and the
 *      pre-existing (unconfirmed-but-undisputed) READALL=35 in
 *      src/libvms/include/prvdef.h.
 *
 * Per CLAUDE.md Rule 8 this is the "public $PRVDEF/$SSDEF documentation"
 * branch of the oracle-pin requirement, not the live-lab branch -- stated
 * so a future reader does not mistake this for a from-scratch SDA read.
 * src/libvms/include/prvdef.h already carried PRV$V_SYSNAM=2/PRV$V_GRPNAM=3
 * /PRV$V_GRPPRV=34 from before this item; the _Static_assert below is what
 * makes THIS copy agree with that one, not what re-derives either.
 */
#define VMS_PRV_M_CMKRNL    (1ULL << VMS_PRV_V_CMKRNL)
#define VMS_PRV_M_CMEXEC    (1ULL << VMS_PRV_V_CMEXEC)
#define VMS_PRV_M_SYSNAM    (1ULL << VMS_PRV_V_SYSNAM)
#define VMS_PRV_M_GRPNAM    (1ULL << VMS_PRV_V_GRPNAM)
#define VMS_PRV_M_SETPRV    (1ULL << VMS_PRV_V_SETPRV)
#define VMS_PRV_M_PRMMBX    (1ULL << VMS_PRV_V_PRMMBX)
#define VMS_PRV_M_TMPMBX    (1ULL << VMS_PRV_V_TMPMBX)
#define VMS_PRV_M_WORLD     (1ULL << VMS_PRV_V_WORLD)
#define VMS_PRV_M_NETMBX    (1ULL << VMS_PRV_V_NETMBX)
#define VMS_PRV_M_SYSPRV    (1ULL << VMS_PRV_V_SYSPRV)
#define VMS_PRV_M_GRPPRV    (1ULL << VMS_PRV_V_GRPPRV)

/*
 * The privileges the OVMX executive actually ENFORCES today.
 *
 * CLAUDE.md Rule 10, and this item's own constraint: a privilege that
 * is reported but unenforced is worse than an absent one, because it
 * reads as a security control. This set is exactly the privileges some
 * code path in vms.ko will refuse an operation over:
 *   CMKRNL  vms_ioctl_setmode -> kernel mode; vms_ioctl_dclast at kernel
 *   CMEXEC  vms_ioctl_setmode -> exec mode;   vms_ioctl_dclast at exec
 *   SETPRV  vms_ioctl_setprv widening; vms_ioctl_setident granting
 *   WORLD   vms_ioctl_getjpi / vms_ioctl_procscan reading a process
 *           OUTSIDE the caller's UIC group (vms_proc_may_read)
 *   SYSNAM  vms_ioctl_lnm_define / vms_ioctl_lnm_delete against
 *           LNM$SYSTEM (vms-5b7)
 *   GRPNAM  vms_ioctl_lnm_define / vms_ioctl_lnm_delete against
 *           LNM$GROUP (vms-5b7)
 * Bits outside this set are STORED and REPORTED (they come from SYSUAF
 * and VMS reports them) but nothing in this tree gates on them. Adding
 * a privilege here without adding the check it names is the defect this
 * constant exists to prevent.
 *
 * SYSPRV AND GRPPRV ARE DELIBERATELY ABSENT even though vms_lnm.c's new
 * check also accepts either as an alternate to SYSNAM/GRPNAM (real,
 * documented VMS behaviour -- OpenVMS DCL Dictionary, DEFINE: SYSPRV
 * substitutes for SYSNAM on LNM$SYSTEM, and SYSPRV or GRPPRV substitutes
 * for GRPNAM on LNM$GROUP). Adding them here would tell every OTHER
 * reader of this mask (dcl_cmd_set.c's enforced_privs_held(), SHOW
 * PROCESS/PRIVILEGES, F$PRIVILEGE) that OVMX enforces SYSPRV/GRPPRV in
 * the general VMS sense -- bypass system/group object protection
 * everywhere -- which remains false pending vms-pv1. They are consulted
 * by exactly one narrow code path, not enforced as their own control.
 *
 * GROUP IS DELIBERATELY ABSENT, and that is a measurement rather than an
 * oversight. The obvious guess -- GROUP to read another process in your
 * own group, WORLD to read any other -- is WRONG on the oracle: with
 * SET PROCESS/PRIVILEGE=(NOALL) a VAX 7.3 process read a same-group
 * process's USERNAME successfully, and with GROUP and nothing else it
 * was still refused a cross-group read. WORLD alone lifted it. The
 * transcript is docs/oracle/vax73-privileges.md §5. OVMX does not
 * enforce GROUP anywhere, so it is not listed here.
 */
#define VMS_PRV_M_ENFORCED  (VMS_PRV_M_CMKRNL | VMS_PRV_M_CMEXEC | \
                             VMS_PRV_M_SETPRV | VMS_PRV_M_WORLD | \
                             VMS_PRV_M_SYSNAM | VMS_PRV_M_GRPNAM)

struct vms_priv_args {
    uint64_t mask;          /* privilege mask to set/clear/check */
    uint64_t prev;          /* return: previous privilege mask */
    uint32_t enable;        /* 1=enable, 0=disable */
    uint32_t permanent;     /* 1=permanent, 0=temporary */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

struct vms_getmode_args {
    uint8_t  mode;          /* return: current mode */
    uint8_t  pad[3];
    uint64_t cur_privs;     /* return: current privileges */
    uint64_t perm_privs;    /* return: permanent privileges */
};

#define VMS_IOCTL_SETMODE   _IOW(VMS_IOC_MAGIC, 0x01, struct vms_mode_args)
#define VMS_IOCTL_GETMODE   _IOR(VMS_IOC_MAGIC, 0x02, struct vms_getmode_args)
#define VMS_IOCTL_SETPRV    _IOWR(VMS_IOC_MAGIC, 0x03, struct vms_priv_args)
#define VMS_IOCTL_CHKPRIV   _IOWR(VMS_IOC_MAGIC, 0x04, struct vms_priv_args)

/* ================================================================
 * AST Delivery (3b)
 * ================================================================ */

struct vms_ast_args {
    uint64_t astadr;        /* AST routine address (in userspace) */
    uint64_t astprm;        /* AST parameter */
    uint8_t  acmode;        /* access mode for this AST (0-3) */
    uint8_t  pad[3];
    uint32_t status;        /* return: SS$_ status */
};

struct vms_setast_args {
    uint32_t enable;        /* 1=enable, 0=disable */
    uint32_t prev_state;    /* return: previous state */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_DCLAST      _IOW(VMS_IOC_MAGIC, 0x10, struct vms_ast_args)
#define VMS_IOCTL_SETAST      _IOWR(VMS_IOC_MAGIC, 0x11, struct vms_setast_args)
/* DELIVERAST: userspace passes a pointer to a vms_ast_args buffer to receive
 * the next pending AST. Changed from _IO to _IOR so the ioctl arg carries the
 * userspace buffer address instead of being ignored. */
#define VMS_IOCTL_DELIVERAST  _IOR(VMS_IOC_MAGIC, 0x12, struct vms_ast_args)

/* ================================================================
 * Event Flags (3c)
 * ================================================================ */

struct vms_ef_args {
    uint32_t efn;           /* event flag number (0-127) */
    uint32_t status;        /* return: SS$_ status */
};

struct vms_ef_wait_args {
    uint32_t efn;           /* cluster base EFN (for wflor/wfland) or single EFN */
    uint32_t mask;          /* bitmask for wflor/wfland */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

struct vms_ef_read_args {
    uint32_t efn;           /* event flag number */
    uint32_t state;         /* return: cluster state (32-bit) */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

struct vms_ef_common_args {
    uint32_t efn;           /* starting EFN (64 or 96) */
    char     name[32];      /* cluster name */
    uint32_t prot;          /* protection mask */
    uint32_t perm;          /* permanent flag */
    uint32_t status;        /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_SETEF     _IOWR(VMS_IOC_MAGIC, 0x20, struct vms_ef_args)
#define VMS_IOCTL_CLREF     _IOWR(VMS_IOC_MAGIC, 0x21, struct vms_ef_args)
#define VMS_IOCTL_WAITFR    _IOWR(VMS_IOC_MAGIC, 0x22, struct vms_ef_args)
#define VMS_IOCTL_WFLOR     _IOWR(VMS_IOC_MAGIC, 0x23, struct vms_ef_wait_args)
#define VMS_IOCTL_WFLAND    _IOWR(VMS_IOC_MAGIC, 0x24, struct vms_ef_wait_args)
#define VMS_IOCTL_READEF    _IOWR(VMS_IOC_MAGIC, 0x25, struct vms_ef_read_args)
#define VMS_IOCTL_ASCEFC    _IOWR(VMS_IOC_MAGIC, 0x26, struct vms_ef_common_args)
#define VMS_IOCTL_DACEFC    _IOWR(VMS_IOC_MAGIC, 0x27, struct vms_ef_args)
/* $DLCEFC takes a NAME and no flag number -- it can delete a permanent
 * cluster this process never associated with -- so it reuses
 * vms_ef_common_args and ignores efn/prot/perm. (vms-2a8) */
#define VMS_IOCTL_DLCEFC    _IOWR(VMS_IOC_MAGIC, 0x28, struct vms_ef_common_args)

/* ================================================================
 * Lock Manager (3d)
 * ================================================================ */

/* VMS lock modes */
#define LCK_K_NLMODE    0   /* Null */
#define LCK_K_CRMODE    1   /* Concurrent Read */
#define LCK_K_CWMODE    2   /* Concurrent Write */
#define LCK_K_PRMODE    3   /* Protected Read */
#define LCK_K_PWMODE    4   /* Protected Write */
#define LCK_K_EXMODE    5   /* Exclusive */

/* ENQ flags */
#define LCK_M_CONVERT   0x01   /* Convert existing lock */
#define LCK_M_NOQUEUE   0x02   /* Don't queue if not granted */
#define LCK_M_SYSTEM    0x04   /* System-wide resource */
#define LCK_M_VALBLK    0x08   /* Lock has value block */
/*
 * LCK_M_SYNC (OVMX design choice, not a real $LCKDEF bit): request that the
 * kernel ENQ/CONVERT ioctl BLOCK in-kernel until the lock is granted (or a
 * deadlock is detected), instead of returning immediately with the request
 * queued. This is how sys$enqw's synchronous "wait" is realized without a
 * userspace poll loop. Callers that want async ($ENQ) semantics leave it
 * clear. Lives in the kernel LCK_M_* namespace and is never exposed through
 * the public $ENQ flag contract (see src/libvms/syssvc/sys_lock.c).
 */
#define LCK_M_SYNC      0x10   /* Block in-kernel until granted (sync ENQ) */

/* Lock value block size */
#define LCK_VALBLK_SIZE 16

struct vms_enq_args {
    uint32_t efn;               /* event flag for completion */
    uint32_t lkmode;            /* requested lock mode (0-5) */
    uint32_t flags;             /* LCK_M_* flags */
    uint32_t parid;             /* parent lock ID (0 for root) */
    char     resnam[32];        /* resource name (null-terminated) */
    uint64_t astadr;            /* completion AST address */
    uint64_t astprm;            /* AST parameter */
    uint64_t blkastadr;         /* blocking AST address */
    uint32_t lkid;              /* return: lock ID (or input for convert) */
    uint32_t lk_status;         /* return: lock status (granted mode in LKSB) */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* lock value block */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

struct vms_deq_args {
    uint32_t lkid;              /* lock ID to dequeue */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* value block to write back */
    uint32_t flags;             /* dequeue flags */
    uint32_t status;            /* return: SS$_ status */
};

struct vms_getlki_args {
    uint32_t lkid;              /* lock ID to query */
    uint32_t granted_mode;      /* return: current granted mode */
    uint32_t requested_mode;    /* return: requested mode (if waiting) */
    uint32_t parent_id;         /* return: parent lock ID */
    char     resnam[32];        /* return: resource name */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* return: value block */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_ENQ       _IOWR(VMS_IOC_MAGIC, 0x30, struct vms_enq_args)
#define VMS_IOCTL_DEQ       _IOWR(VMS_IOC_MAGIC, 0x31, struct vms_deq_args)
#define VMS_IOCTL_CONVERT   _IOWR(VMS_IOC_MAGIC, 0x32, struct vms_enq_args)
#define VMS_IOCTL_GETLKI    _IOWR(VMS_IOC_MAGIC, 0x33, struct vms_getlki_args)

/* ================================================================
 * Process registration
 * ================================================================ */

/*
 * Registration carries NO privilege request (vms-2b8).
 *
 * It used to carry an init_privs quadword: the process told the
 * executive which privileges it had. That is an honor system, not an
 * access control system -- the thing being asked to enforce the mask
 * was taking the mask from the party it was enforcing against. The
 * field is GONE, not ignored, so no caller can keep passing it and no
 * reader can be tempted to trust it.
 *
 * The executive now derives the authorized mask from the task's real
 * credentials at registration (vms_proc_register), the same way it
 * already derived the UIC. A process cannot change what it gets by
 * asking differently, because it is no longer asked.
 *
 * THE VMS PROCESS ID IS ASSIGNED BY THE EXECUTIVE, NOT REQUESTED
 * (vms-2b8, round 3). vms_pid used to be an INPUT: the process told the
 * executive which VMS process ID to file it under, and nothing checked
 * that the value was not already in use. Two processes could therefore
 * hold one VMS PID, and $GETJPI by that PID resolved to whichever row
 * the hash walk reached first -- so an unprivileged process could make
 * a privileged process's identity answer to a key it chose. That is the
 * same defect shape as the deleted init_privs field, one field along:
 * the process telling the executive what it is.
 *
 * The field is now OUTPUT-ONLY. Whatever userspace puts here is
 * overwritten before the executive looks at anything.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): the VALUE. OpenVMS composes a
 * process ID from a PCB-vector index plus a sequence number so that a
 * reused slot never yields a repeated PID, and the extended PID adds
 * cluster node information; no public document publishes that layout
 * byte for byte, so OVMX does not pretend to reproduce it. What OVMX
 * reproduces is the PROPERTY: the ID is assigned by the executive,
 * unique among live processes, and not immediately reused after a
 * process exits. See assign_vms_pid() in vms_module.c.
 */
struct vms_register_args {
    uint32_t vms_pid;           /* return: the VMS process ID assigned */
    uint32_t status;            /* return: SS$_ status */
};

#define VMS_IOCTL_REGISTER  _IOWR(VMS_IOC_MAGIC, 0x40, struct vms_register_args)

/*
 * VMS_IOCTL_REGISTER_CONTINUE - register a task that is CONTINUING an
 * already-registered VMS process's identity, not starting a new one
 * (vms-4d7, "Option B": the fork-per-image model made invisible to VMS).
 *
 * WHY THIS EXISTS. On OpenVMS, activating an image (RUN, a foreign
 * command, a DCL utility) does NOT create a process -- the image is mapped
 * into the CURRENT process and runs with its UIC, username and privileges;
 * image rundown returns to DCL in the same process. OVMX instead fork()s +
 * execve()s a fresh Linux process for every image, so without this the
 * image would auto-register a BRAND NEW PCB and derive its own privilege
 * mask from capable(CAP_SYS_ADMIN) -- which is why SYSTEM could not RUN
 * AUTHORIZE (the child never held SYSPRV, though its DCL did).
 *
 * This ioctl tells the executive "the process you already have for my
 * PARENT is the process I am continuing." The executive VALIDATES that
 * relationship itself -- it reads the identity from the parent's PCB in
 * the task hierarchy, never from anything the caller declares -- and
 * shares the parent's VMS PID, UIC, user name and privilege masks onto
 * this task. NOTHING is read from args (output-only, exactly like
 * VMS_IOCTL_REGISTER); the struct returns the shared VMS PID.
 *
 * DISTINCT FROM A NEW PROCESS. SPAWN, RUN/DETACHED and $CREPRC create a
 * genuinely new VMS process and use VMS_IOCTL_REGISTER (new PID, derived
 * identity). Only image activation continues the caller's identity, and
 * only DCL's image-activation path (src/vmsdcl/dcl_cmd_process.c) signals
 * it -- see kif_bind() in src/libvmssys/vms_kif.c.
 *
 * SECURITY. The shared identity is the parent's CURRENT identity: a
 * context that setident'd DOWN to an unprivileged UIC has reduced masks,
 * so continuing it CANNOT resurrect a privilege. The caller cannot pick
 * its parent (the executive reads real_parent), so this is not a path to
 * borrow an unrelated privileged process's identity.
 */
#define VMS_IOCTL_REGISTER_CONTINUE \
                            _IOWR(VMS_IOC_MAGIC, 0x41, struct vms_register_args)

/* ================================================================
 * Device table (executive-resident I/O database)
 *
 * On OpenVMS a device is not something a process owns a private idea
 * of: the driver enters a Unit Control Block in the executive's I/O
 * database at boot, and from then on the device EXISTS for every
 * process on the node. $ASSIGN takes a channel to it, $GETDVI reads
 * its attributes, $DEVICE_SCAN enumerates it, and SHOW DEVICE /
 * SHOW TERMINAL are readers of that one table. The owner, the
 * reference count and the terminal characteristics are properties of
 * the device, not of whoever happens to be asking.
 *
 * These ioctls put the same property behind /dev/vms (vms-d0b). The
 * console terminal OPA0: is created by the executive at module init,
 * exactly as the terminal driver creates it at VMS boot -- no process
 * registers it, and no process can be the only one that sees it.
 * ================================================================ */

/*
 * VMS device names: at most 15 significant characters plus the
 * terminating NUL (OpenVMS I/O User's Reference Manual; the physical
 * name form is ddcu:, e.g. OPA0:).
 */
#define VMS_DEVNAM_SIZE 16

/*
 * Terminal characteristics.
 *
 * PROVENANCE (CLAUDE.md rules 8 and 10): the NAMES below and the fact
 * that each is a two-state characteristic are pinned to the oracle --
 * SHOW TERMINAL on the ~/vax OpenVMS VAX V7.3 lab console (nodes VAX1
 * and VAX2, 30-JUL-2026), captured verbatim in
 * docs/oracle/vax73-terminal-device.md. Every name here appears in
 * that output; no name was invented, and no characteristic VMS does
 * not display was added.
 *
 * The BIT POSITIONS are an OVMX design choice and are NOT VMS's
 * $TTDEF layout. The public OpenVMS documentation available to this
 * work does not publish the byte-level TT$M_ layout, so rather than
 * guess at it OVMX defines its own vector and labels it as its own
 * (rule 8: define our representation, never present it as
 * VMS-authentic). Nothing outside the executive and its client may
 * assume these values match VMS.
 */
#define VMS_TTC_INTERACTIVE     (1ULL << 0)
#define VMS_TTC_ECHO            (1ULL << 1)
#define VMS_TTC_TYPEAHEAD       (1ULL << 2)
#define VMS_TTC_ESCAPE          (1ULL << 3)
#define VMS_TTC_HOSTSYNC        (1ULL << 4)
#define VMS_TTC_TTSYNC          (1ULL << 5)
#define VMS_TTC_LOWERCASE       (1ULL << 6)
#define VMS_TTC_TAB             (1ULL << 7)
#define VMS_TTC_WRAP            (1ULL << 8)
#define VMS_TTC_HARDCOPY        (1ULL << 9)
#define VMS_TTC_REMOTE          (1ULL << 10)
#define VMS_TTC_EIGHTBIT        (1ULL << 11)
#define VMS_TTC_BROADCAST       (1ULL << 12)
#define VMS_TTC_READSYNC        (1ULL << 13)
#define VMS_TTC_FORM            (1ULL << 14)
#define VMS_TTC_FULLDUP         (1ULL << 15)
#define VMS_TTC_MODEM           (1ULL << 16)
#define VMS_TTC_LOCAL_ECHO      (1ULL << 17)
#define VMS_TTC_AUTOBAUD        (1ULL << 18)
#define VMS_TTC_HANGUP          (1ULL << 19)
#define VMS_TTC_BRDCSTMBX       (1ULL << 20)
#define VMS_TTC_DMA             (1ULL << 21)
#define VMS_TTC_ALTYPEAHD       (1ULL << 22)
#define VMS_TTC_SET_SPEED       (1ULL << 23)
#define VMS_TTC_COMMSYNC        (1ULL << 24)
#define VMS_TTC_LINE_EDITING    (1ULL << 25)
#define VMS_TTC_INSERT_EDITING  (1ULL << 26)
#define VMS_TTC_FALLBACK        (1ULL << 27)
#define VMS_TTC_DIALUP          (1ULL << 28)
#define VMS_TTC_SECURE_SERVER   (1ULL << 29)
#define VMS_TTC_DISCONNECT      (1ULL << 30)
#define VMS_TTC_PASTHRU         (1ULL << 31)
#define VMS_TTC_SYSPASSWORD     (1ULL << 32)
#define VMS_TTC_SIXEL           (1ULL << 33)
#define VMS_TTC_SOFT_CHARACTERS (1ULL << 34)
#define VMS_TTC_PRINTER_PORT    (1ULL << 35)
#define VMS_TTC_NUMERIC_KEYPAD  (1ULL << 36)
#define VMS_TTC_ANSI_CRT        (1ULL << 37)
#define VMS_TTC_REGIS           (1ULL << 38)
#define VMS_TTC_BLOCK_MODE      (1ULL << 39)
#define VMS_TTC_ADVANCED_VIDEO  (1ULL << 40)
#define VMS_TTC_EDIT_MODE       (1ULL << 41)
#define VMS_TTC_DEC_CRT         (1ULL << 42)
#define VMS_TTC_DEC_CRT2        (1ULL << 43)
#define VMS_TTC_DEC_CRT3        (1ULL << 44)
#define VMS_TTC_DEC_CRT4        (1ULL << 45)
#define VMS_TTC_DEC_CRT5        (1ULL << 46)
#define VMS_TTC_ANSI_COLOR      (1ULL << 47)
#define VMS_TTC_VMS_STYLE_INPUT (1ULL << 48)

/*
 * One row of the executive device table, as handed to userspace.
 *
 * owner_pid and `allocated` are TWO DIFFERENT THINGS, exactly as the
 * oracle prints them as two different things (measured; see
 * docs/oracle/vax73-terminal-device.md section 7):
 *
 *   - owner_pid is "Owner process ID". A device can be owned with no
 *     allocation at all: on the lab a bare OPEN/WRITE to the
 *     non-shareable terminal TTA0: moved it from Owner "" to
 *     Owner "SYSTEM" / 20400216 with no "allocated" in its status
 *     clause, and the console OPA0: shows Owner "SYSTEM" on a system
 *     where nobody has run ALLOCATE. A channel to a SHAREABLE device
 *     (NLA0:) confers nothing.
 *   - `allocated` is the flag behind the word "allocated" in SHOW
 *     DEVICE/FULL's status clause, and only $ALLOC sets it.
 *
 * refcnt is the "Reference count": one per assigned channel plus one
 * for an outstanding allocation. Ownership itself costs no reference.
 *
 * opcnt/errcnt are the
 * "Operations completed" and "Error count" SHOW DEVICE/FULL reports.
 *
 * NOTE what is deliberately ABSENT (rule 10 -- hide what we cannot
 * answer faithfully rather than reporting a plausible value): the
 * oracle's SHOW TERMINAL also prints Input/Output speed, Parity, and
 * LFfill/CRfill. OVMX's console is a QEMU serial line with no such
 * physical parameters to report, so the executive does not carry a
 * value for them and no reader can print one.
 */
struct vms_devinfo {
    char     devnam[VMS_DEVNAM_SIZE];   /* physical name, e.g. "OPA0:" */
    uint32_t devclass;                  /* DC$_ device class */
    uint32_t devtype;                   /* device type code; 0 = Unknown */
    uint32_t owner_pid;                 /* VMS pid of the owner, 0 = unowned */
    uint32_t owner_uic;                 /* (group << 16) | member */
    uint32_t refcnt;                    /* channels assigned + allocation */
    uint32_t errcnt;                    /* Error count */
    uint64_t opcnt;                     /* Operations completed */
    uint64_t devchar;                   /* VMS_TTC_* (terminals only) */
    uint32_t width;                     /* terminal width */
    uint32_t page;                      /* terminal page length */
    uint32_t allocated;                 /* 1 = allocated to owner_pid */
    uint32_t pad;
};

/*
 * $ALLOC / $DALLOC: allocate a device to this process, and give it
 * back. Allocation is not the only route to ownership -- see
 * struct vms_devinfo -- but it is the only thing that makes a device
 * "allocated", and it holds the device after the last channel is gone.
 *
 * $ALLOC returns SS$_DEVALLOC when the device is OWNED by another
 * process, whether that owner allocated it (ALLOCATE OPA0: from a
 * detached process while the interactive job held the console) or
 * merely assigned a channel to it (ALLOCATE TTA0: while the detached
 * CHANHOLD process held one channel and no allocation). Both are
 * measured; see docs/oracle/vax73-terminal-device.md section 7.
 *
 * $DALLOC returns SS$_DEVNOTALLOC when this process does not have the
 * device ALLOCATED -- including when it owns the device by channel.
 */
struct vms_alloc_args {
    char     devnam[VMS_DEVNAM_SIZE];
    uint32_t status;
    uint32_t pad;
};

/* $ASSIGN: take a channel to a device by name. */
struct vms_assign_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: device name (with or without ':') */
    uint32_t chan;                      /* out: channel number */
    uint32_t status;                    /* return: SS$_ status */
};

/* $DASSGN: give a channel back. */
struct vms_dassgn_args {
    uint32_t chan;
    uint32_t status;
};

/* Selector for VMS_IOCTL_GETDVI: how the device is named. */
#define VMS_DVI_SEL_DEVNAM  0   /* by info.devnam */
#define VMS_DVI_SEL_CHAN    1   /* by an assigned channel */

struct vms_getdvi_args {
    uint32_t select;            /* VMS_DVI_SEL_* */
    uint32_t chan;              /* in: channel, for VMS_DVI_SEL_CHAN */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
    struct vms_devinfo info;    /* in: name for SEL_DEVNAM; out: the row */
};

/*
 * Cursor-driven enumeration of the device table (the reader behind
 * SHOW DEVICE). Set index to 0 for the first row; each call returns
 * one row and advances index. SS$_NOMOREDEV terminates the scan,
 * which is what $DEVICE_SCAN returns when the search is exhausted.
 */
struct vms_devscan_args {
    uint32_t index;             /* in: cursor; out: cursor for next call */
    uint32_t status;            /* return: SS$_ status */
    struct vms_devinfo info;    /* out: the row at the incoming cursor */
};

/*
 * Modify terminal characteristics THROUGH AN ASSIGNED CHANNEL, the
 * way VMS does it: SET TERMINAL is $QIO IO$_SETMODE on a channel, not
 * an operation that names a device out of nowhere. A caller with no
 * channel to the device gets SS$_IVCHAN, which is why a process
 * cannot quietly redefine a terminal it never opened.
 */
#define VMS_TTSET_CHAR      0x1     /* apply setchar/clrchar */
#define VMS_TTSET_WIDTH     0x2     /* apply width */
#define VMS_TTSET_PAGE      0x4     /* apply page */

struct vms_setmode_args {
    uint32_t chan;              /* channel assigned to the terminal */
    uint32_t flags;             /* VMS_TTSET_* : which fields are being set */
    uint64_t setchar;           /* VMS_TTC_* bits to set */
    uint64_t clrchar;           /* VMS_TTC_* bits to clear */
    uint32_t width;
    uint32_t page;
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_ASSIGN    _IOWR(VMS_IOC_MAGIC, 0x50, struct vms_assign_args)
#define VMS_IOCTL_DASSGN    _IOWR(VMS_IOC_MAGIC, 0x51, struct vms_dassgn_args)
#define VMS_IOCTL_GETDVI    _IOWR(VMS_IOC_MAGIC, 0x52, struct vms_getdvi_args)
#define VMS_IOCTL_DEVSCAN   _IOWR(VMS_IOC_MAGIC, 0x53, struct vms_devscan_args)
#define VMS_IOCTL_TTSETMODE _IOWR(VMS_IOC_MAGIC, 0x54, struct vms_setmode_args)
#define VMS_IOCTL_ALLOC     _IOWR(VMS_IOC_MAGIC, 0x55, struct vms_alloc_args)
#define VMS_IOCTL_DALLOC    _IOWR(VMS_IOC_MAGIC, 0x56, struct vms_alloc_args)

/*
 * The kernel module and the userspace client compile these structures
 * separately, from this one header, and then pass them across the
 * /dev/vms boundary by raw address. If a field is ever reordered,
 * widened or padded differently on one side, every ioctl above starts
 * reading the wrong offsets -- silently, and only at runtime, and only
 * for the fields past the change. Freeze the layouts here so that
 * failure is a compile error on whichever side moved.
 *
 * The ioctl encodings are asserted for the same reason and one more:
 * _IOWR folds sizeof(struct) into the request number, so a size change
 * ALSO renumbers the request. The executive would then reject it with
 * -ENOTTY rather than mis-decode it -- a different symptom, same root
 * cause, and equally worth catching before it ships.
 *
 * These values are measured, not chosen: aarch64 and x86_64 agree,
 * because every field is a fixed-width type.
 */
_Static_assert(sizeof(struct vms_devinfo) == 72,
               "struct vms_devinfo changed size -- kernel and userspace would disagree on device attribute offsets");
_Static_assert(sizeof(struct vms_assign_args) == 24,
               "struct vms_assign_args changed size -- $ASSIGN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_dassgn_args) == 8,
               "struct vms_dassgn_args changed size -- $DASSGN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_getdvi_args) == 88,
               "struct vms_getdvi_args changed size -- $GETDVI would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_devscan_args) == 80,
               "struct vms_devscan_args changed size -- $DEVICE_SCAN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_setmode_args) == 40,
               "struct vms_setmode_args changed size -- IO$_SETMODE would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_alloc_args) == 24,
               "struct vms_alloc_args changed size -- $ALLOC/$DALLOC would decode at the wrong offsets");

_Static_assert(VMS_IOCTL_ASSIGN == 0xC0185650u,
               "VMS_IOCTL_ASSIGN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DASSGN == 0xC0085651u,
               "VMS_IOCTL_DASSGN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETDVI == 0xC0585652u,
               "VMS_IOCTL_GETDVI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DEVSCAN == 0xC0505653u,
               "VMS_IOCTL_DEVSCAN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_TTSETMODE == 0xC0285654u,
               "VMS_IOCTL_TTSETMODE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ALLOC == 0xC0185655u,
               "VMS_IOCTL_ALLOC encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_DALLOC == 0xC0185656u,
               "VMS_IOCTL_DALLOC encodes differently here than on the reference build");

/* ================================================================
 * Process table (executive-resident PCB directory)
 *
 * On OpenVMS the process name lives in the executive's process
 * database, not in the process's own address space: $SETPRN writes it,
 * $GETJPI resolves a process by it, and SHOW SYSTEM enumerates the
 * table. That is why a VMS process name means anything at all -- every
 * other process can see it.
 *
 * These ioctls put the same property behind /dev/vms. The entry is
 * keyed by the Linux pid, which is invariant across execve(), so the
 * name survives image activation without any userspace carrier.
 * ================================================================ */

/*
 * VMS process names are 1-15 characters (OpenVMS System Services
 * Reference, $SETPRN / $CREPRC prcnam argument). 16 bytes = 15
 * significant characters plus the NUL terminator, matching the
 * in-tree struct vms_pcb prcnam[16].
 */
#define VMS_PRCNAM_SIZE 16

/*
 * Inbound name transfer buffer -- OVMX DESIGN CHOICE, not a VMS format.
 *
 * A name travelling FROM userspace INTO the executive is carried in a
 * buffer strictly larger than the longest legal VMS process name, so an
 * OVERSIZED name arrives INTACT and the executive is the thing that
 * rejects it. Copying an inbound name into a VMS_PRCNAM_SIZE field in
 * userspace would truncate it into a legal-looking name and convert a
 * rejection into a success -- for $SETPRN, silently naming the process
 * something the caller never asked for; for the $GETJPI prcnam
 * selector, silently resolving a DIFFERENT process.
 *
 * Oracle (VAX1, OpenVMS VAX V7.3, 2026-07-30, documented tool output):
 *   $ SET PROCESS/NAME="IMPL8019NAM15XY"     ! 15 chars -> accepted
 *   $ SET PROCESS/NAME="IMPL8019NAM15XYZ"    ! 16 chars
 *   %SET-E-NOTSET, error modifying process name
 *   -SYSTEM-F-IVLOGNAM, invalid logical name
 *   $ WRITE SYS$OUTPUT F$GETJPI("","PRCNAM") ! old name UNCHANGED
 *   IMPL8019NAM15XY
 * VMS rejects the oversized name outright and leaves the existing name
 * in place. It does not truncate, and it does not partially apply.
 *
 * The userspace copy is still bounded (at VMS_PRCNAM_XFER - 1), but
 * that bound cannot turn a rejection into an acceptance: every name too
 * long to fit in VMS_PRCNAM_SIZE is illegal, and VMS_PRCNAM_XFER is far
 * larger than VMS_PRCNAM_SIZE, so a clipped name still has no NUL
 * within the executive's inspection window and is still rejected.
 */
#define VMS_PRCNAM_XFER 64

/*
 * One row of the executive process table.
 *
 * uic is [group,member] packed as (group << 16) | member -- the same
 * packing sys$getjpi's JPI$_UIC item returns. The executive derives it
 * from the task's credentials; it is never supplied by the process
 * itself (a process must not be able to declare its own UIC).
 */
/*
 * Executive-resident username field width -- OVMX DESIGN CHOICE.
 *
 * OpenVMS user names are 1-12 characters (Guide to System Security),
 * but OVMX's SYSUAF record already uses a 32-byte space-padded username
 * as its primary key, and that width is itself declared an OVMX design
 * choice in src/libvms/include/sysuaf.h. This field matches the SYSUAF
 * key so an authenticated record can be stamped onto a process without
 * a width conversion that could truncate a name into a different one.
 * The executive enforces NUL-termination inside the buffer (a trust
 * boundary check) and nothing else -- SYSUAF is the authority on which
 * names exist, so the executive does not invent a rejection semantic
 * VMS never showed us (CLAUDE.md Rule 10).
 */
#define VMS_USERNAME_SIZE 32

struct vms_procinfo {
    uint32_t vms_pid;                   /* VMS-style process ID */
    uint32_t linux_pid;                 /* Linux pid backing the process */
    char     prcnam[VMS_PRCNAM_SIZE];   /* process name ("" if unnamed) */
    uint32_t uic;                       /* (group << 16) | member */
    uint8_t  current_mode;              /* PSL_C_KERNEL..PSL_C_USER */
    /*
     * REDACTION IS DECLARED, NOT INFERRED (vms-8019 round 4).
     *
     * A row the caller may not $GETJPI comes back with every identity
     * field zeroed (see the vms_procscan_args comment below). A reader
     * that has to GUESS whether a zero means "withheld" or "genuinely
     * zero" will eventually guess wrong, and the first reader already
     * did: src/vmsdcl/dcl_cmd_show.c passed the zeroed linux_pid to
     * /proc, got a failure it did not check, and printed the CPU time
     * its own buffer happened to be initialised with -- a fabricated
     * accounting value for a process whose accounting the caller is
     * FORBIDDEN to read. That is the fabrication class this item
     * exists to delete, so the executive now says so in the row.
     *
     * 1 = identity fields (linux_pid, uic, current_mode, privilege
     * masks, username) were withheld; only vms_pid and prcnam are
     * present. Occupies a byte of the existing padding, so the ioctl
     * ABI is unchanged -- the _Static_asserts below still hold.
     */
    uint8_t  redacted;
    uint8_t  pad[2];
    uint64_t cur_privs;                 /* current (process) privileges */
    uint64_t perm_privs;                /* authorized (permanent) privileges */
    char     username[VMS_USERNAME_SIZE]; /* "" until an identity is stamped */
    /*
     * The job's terminal (vms-d0b). "" when this process has none.
     *
     * WHY IT IS HERE AND NOT IN THE PROCESS'S OWN MEMORY. SHOW TERMINAL
     * names the terminal THIS JOB is on, and to be a reader of anything
     * it must first be able to READ that name from somewhere every
     * other process can see. Before this field the tree had no such
     * place: PID 1 handed the name to its login child in a
     * VMS_TERMINAL environment variable, which vms-fb9 deleted as the
     * rejected VMS_PRCNAM shape -- a process telling itself what it is,
     * in a way nothing can see or contradict (CLAUDE.md Rule 10,
     * worked example 2). The name now lives in the executive's process
     * table beside prcnam, uic and username, which is what makes
     * "process 20400216 is on OPA0:" a fact about the system rather
     * than a claim by one process.
     *
     * WITHHELD WITH THE REST OF THE IDENTITY on a row the caller may
     * not $GETJPI. The oracle refused EVERY $GETJPI item on a
     * cross-UIC-group process without WORLD, not a subset of them
     * (docs/oracle/vax73-privileges.md §5), so this field follows
     * username and uic rather than vms_pid and prcnam.
     *
     * The value is a device name out of the executive's OWN device
     * table -- VMS_IOCTL_SETTERM takes a CHANNEL, never a name, and
     * the executive copies the name off the device that channel is
     * assigned to. A process therefore cannot name a terminal it has
     * no channel to, and cannot name a device that does not exist.
     */
    char     terminal[VMS_DEVNAM_SIZE];   /* "" when the job has no terminal */

    /*
     * P0 program-region extent (vms-68f.i, in-process image activation
     * foundation -- docs/design-in-process-activation.md Part II §A.2.1).
     * [p0_base, p0_limit) is the virtual-address range VMS_IOCTL_P0_MAP
     * last registered for this process; both zero when no image is
     * currently mapped into P0 (the process has never RUN an image
     * in-process, or the last one was rundown by VMS_IOCTL_P0_UNMAP).
     *
     * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), stated rather than implied:
     * OpenVMS publishes no $GETJPI item that reports a raw P0 base/limit
     * virtual address pair -- what is pinned to the design (IDSM,
     * "Process Address Space") is the PROPERTY these two fields exist to
     * make observable: a P0 program region that exists while an image is
     * activated and is gone after image rundown, distinct from P1
     * (process-permanent) state. There is no VMS wire format being
     * claimed for these two field names or their byte layout.
     *
     * WITHHELD WITH THE REST OF THE IDENTITY on a row the caller may not
     * $GETJPI, same placement rule as `terminal` above: a process's
     * memory layout is not something a same-UIC-group stranger gets for
     * free either, and the oracle's blanket refusal in
     * docs/oracle/vax73-privileges.md §5 covers "every item", not a
     * subset.
     */
    uint64_t p0_base;
    uint64_t p0_limit;
};

/*
 * WHO MAY READ WHOSE ROW -- ORACLE-PINNED (vms-2b8 round 3).
 * Transcript: docs/oracle/vax73-privileges.md §5, measured on
 * VAX1 (OpenVMS VAX V7.3) in one session with SET PROCESS/PRIVILEGE
 * driving the caller's mask.
 *
 *   caller's own row                  -> allowed, no privilege
 *   another process, SAME UIC group   -> allowed, no privilege
 *   another process, OTHER UIC group  -> SS$_NOPRIV (36) unless WORLD
 *
 * Measured, in that order:
 *   NOALL      + $GETJPI(AUDIT_SERVER [SYSTEM], "USERNAME") -> AUDIT$SERVER
 *   NOALL      + $GETJPI(TCPIP$FTP_1 [TCPIP$AUX,..], "USERNAME")
 *                                          -> %SYSTEM-F-NOPRIV
 *   GROUP only + the same cross-group read -> %SYSTEM-F-NOPRIV
 *   WORLD only + the same cross-group read -> TCPIP$FTP
 * Every item tried on the cross-group process (USERNAME, PRCNAM, STATE,
 * UIC, CURPRIV) was refused identically, so the refusal is on the
 * PROCESS, not on the item.
 *
 * Enforced by vms_proc_may_read() in vms_proctab.c.
 */

/* Selector for VMS_IOCTL_GETJPI: how the target process is named. */
#define VMS_JPI_SEL_SELF    0   /* the calling process */
#define VMS_JPI_SEL_PID     1   /* by vms_pid */
#define VMS_JPI_SEL_PRCNAM  2   /* by prcnam, within the caller's UIC group */

struct vms_getjpi_args {
    uint32_t select;            /* VMS_JPI_SEL_* */
    uint32_t status;            /* return: SS$_ status */
    struct vms_procinfo info;   /* in: vms_pid selector; out: the row */
    /*
     * The name selector lives OUTSIDE info, in an inbound transfer
     * buffer, so an oversized name reaches the executive untruncated.
     * info.prcnam is output-only: it is the row's name, never the
     * lookup key.
     */
    char     sel_prcnam[VMS_PRCNAM_XFER];
};

/*
 * Cursor-driven enumeration of the process table (the reader behind
 * SHOW SYSTEM). Set index to 0 for the first row; each call returns
 * one row and advances index. SS$_NONEXPR terminates the scan, which
 * is what $PROCESS_SCAN returns when the wildcard search is exhausted.
 *
 * ROWS OUTSIDE THE CALLER'S REACH COME BACK REDACTED, NOT OMITTED, and
 * that split is measured rather than chosen (docs/oracle/vax73-
 * privileges.md Section 4). On the oracle, a process holding NO
 * privileges at all still saw EVERY process in SHOW SYSTEM -- including
 * one in another UIC group, with its process name -- while $GETJPI on
 * that same process was refused SS$_NOPRIV for every item. Enumeration
 * is not privileged on VMS; identity is.
 *
 * So a row the caller may not $GETJPI is returned carrying only what
 * SHOW SYSTEM displays of it -- the process ID and the process name --
 * with username, UIC, privilege masks and access mode zeroed. It is
 * NOT skipped, because skipping it would hide a process VMS shows, and
 * it is not returned whole, because that is the leak this exists to
 * close. linux_pid is zeroed too: it is an OVMX implementation handle
 * with no VMS counterpart, so there is no measurement making it
 * visible.
 *
 * info.redacted is set to 1 on such a row so the reader KNOWS the
 * fields are withheld rather than inferring it from a zero.
 */
struct vms_procscan_args {
    uint32_t index;             /* in: cursor; out: cursor for next call */
    uint32_t status;            /* return: SS$_ status */
    struct vms_procinfo info;   /* out: the row at the incoming cursor */
};

struct vms_setprn_args {
    char     prcnam[VMS_PRCNAM_XFER];   /* new process name, untruncated */
    uint32_t status;                    /* return: SS$_ status */
    uint32_t pad;
};

/*
 * Stamp an AUTHENTICATED identity onto the calling process (vms-2b8).
 *
 * This is the LOGINOUT shape. On OpenVMS a process does not choose its
 * user name, UIC or authorized privileges: LOGINOUT authenticates
 * against SYSUAF while holding privilege, and the identity it proved is
 * placed in the executive's process database, where it becomes what
 * every other process sees. The image the user then runs inherits that
 * identity and cannot widen it.
 *
 * The GRANT RULE the executive enforces, and the whole point of the
 * ioctl: a caller WITHOUT SETPRV may only stamp an identity whose
 * authorized privilege mask is a SUBSET of its own authorized mask,
 * may not change its UIC, AND MAY NOT CHANGE ITS USER NAME. So identity
 * establishment is a one-way drop unless the caller holds the privilege
 * VMS names for exceeding its own authorization. A process therefore
 * cannot grant itself a privilege it was not given -- it can only give
 * privileges away.
 *
 * THE USER NAME CLAUSE WAS MISSING UNTIL ROUND 3, and its absence made
 * the whole ioctl decorative for the half of identity that every reader
 * DISPLAYS. The mask and the UIC were guarded; the name was memcpy'd
 * unconditionally. A process that had done a real setuid() off root
 * could therefore call SETIDENT with its OWN uic and its OWN mask --
 * satisfying both guards by construction -- and a username of "SYSTEM",
 * and the executive would report it as SYSTEM to every other process
 * from then on. VMS has no service by which a process names itself a
 * user (CLAUDE.md Rule 10); the name arrives from SYSUAF, through
 * something holding privilege, or it does not arrive.
 *
 * A consequence worth stating: a process that has never been given a
 * user name has "" as its name, and "" is not a legal name to stamp, so
 * an unauthenticated caller without SETPRV cannot use this ioctl at
 * all. That is the intended shape. It can still drop privileges through
 * $SETPRV; what it cannot do is acquire a name.
 *
 * (Self-targeted only. Stamping ANOTHER process's identity is not
 * offered, because OVMX has no VMS behaviour pinned for it yet and
 * Rule 10 forbids inventing one: what is not matched is hidden.)
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): the ioctl and its argument
 * layout are ours. Public OpenVMS documentation describes LOGINOUT's
 * EFFECT but publishes no byte-level interface for it, so this is not
 * presented as a VMS-authentic mechanism -- only its semantics are
 * pinned (SETPRV is what lets a process exceed its authorization).
 *
 * OVMX DESIGN CHOICE: cur_privs is set equal to authorized_privs.
 * OpenVMS distinguishes AUTHORIZED privileges (AUTHORIZE /PRIVILEGES)
 * from the DEFAULT privileges a process logs in with (/DEFPRIVILEGES).
 * The OVMX SYSUAF record carries a single uaf$q_priv quadword, so OVMX
 * has one mask and authorized == default. Labelled here rather than
 * silently conflated.
 */
struct vms_ident_args {
    char     username[VMS_USERNAME_SIZE]; /* authenticated user name */
    uint32_t uic;                         /* (group << 16) | member */
    uint32_t status;                      /* return: SS$_ status */
    uint64_t authorized_privs;            /* SYSUAF uaf$q_priv */
};

/*
 * Record the calling process's TERMINAL in the executive (vms-d0b).
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), stated rather than implied:
 * OpenVMS publishes no system service by this name. What it publishes
 * is the RESULT -- $GETJPI's JPI$_TERMINAL item, and SHOW TERMINAL's
 * "Terminal: _OPA0:" header -- and the fact that the answer comes out
 * of the executive's process database rather than out of the asking
 * process. On VMS the binding is made by the terminal driver and
 * LOGINOUT when the interactive job is created; in OVMX it is made by
 * PID 1's login child, in the same place and for the same reason, and
 * it is this ioctl that puts it where every other process can read it.
 *
 * THE ARGUMENT IS A CHANNEL, NOT A NAME, and that is the whole guard.
 * A name would let a process declare itself to be on any terminal it
 * liked, including one it has never opened and one that does not
 * exist -- the environment-variable facade with an ioctl in front of
 * it. A channel is a thing the executive itself issued, to this
 * process, for a device in its own table: the executive reads the
 * device off the channel and copies ITS name. The caller supplies no
 * text at all.
 *
 *   SS$_IVCHAN    the channel is not one this process holds. Same
 *                 condition and same status as IO$_SETMODE above,
 *                 which is the other operation that reaches a device
 *                 through a channel.
 *   SS$_IVDEVNAM  the channel is to a device that is not a terminal
 *                 (device class DC$_TERM). Same status IO$_SETMODE's
 *                 terminal function returns for the same mistake.
 */
struct vms_setterm_args {
    uint32_t chan;              /* channel assigned to the terminal */
    uint32_t status;            /* return: SS$_ status */
};

#define VMS_IOCTL_SETPRN    _IOWR(VMS_IOC_MAGIC, 0x41, struct vms_setprn_args)
#define VMS_IOCTL_GETJPI    _IOWR(VMS_IOC_MAGIC, 0x42, struct vms_getjpi_args)
#define VMS_IOCTL_PROCSCAN  _IOWR(VMS_IOC_MAGIC, 0x43, struct vms_procscan_args)
#define VMS_IOCTL_SETIDENT  _IOWR(VMS_IOC_MAGIC, 0x44, struct vms_ident_args)
#define VMS_IOCTL_SETTERM   _IOWR(VMS_IOC_MAGIC, 0x45, struct vms_setterm_args)

/*
 * ABI lock for the process-table ioctls (vms-8019).
 *
 * The kernel side of this header gets _IOWR from <linux/ioctl.h>; the
 * userspace side may instead fall back to the hand-rolled macros at the
 * top of this file, and OVMX builds on two architectures. Nothing
 * previously checked that all four combinations produce the same
 * numbers -- the executive proof has only ever been RUN on aarch64, so
 * the x86_64 half of that agreement was an assumption.
 *
 * These assertions turn it into a build failure instead. They are
 * evaluated by every translation unit that includes this header, kernel
 * or userspace, on whatever architecture is compiling -- so the CI
 * x86_64 build proves the layout even where the QEMU proof cannot run.
 *
 * The literals are the asm-generic _IOC encoding written out by hand:
 *   (dir << 30) | (sizeof(struct) << 16) | ('V' << 8) | nr
 * with dir == 3 (_IOC_READ|_IOC_WRITE). If a struct grows, these fail
 * and the ioctl NUMBER has changed -- which is a wire break, not a
 * cosmetic one, and must be handled deliberately.
 */
/*
 * THESE THREE LITERALS MOVED WHEN THE TERMINAL FIELD WAS ADDED
 * (vms-d0b): procinfo 80 -> 96, getjpi_args 152 -> 168,
 * procscan_args 88 -> 104, and with them the GETJPI and PROCSCAN
 * request numbers, which fold sizeof into the encoding. That is
 * exactly the deliberate handling these assertions exist to force --
 * the kernel module and every userspace client are rebuilt together
 * from this one header, so the break is a compile-time event, not a
 * runtime mis-decode.
 *
 * THEY MOVED AGAIN WHEN p0_base/p0_limit WERE ADDED (vms-68f.i):
 * procinfo 96 -> 112, getjpi_args 168 -> 184, procscan_args 104 -> 120,
 * and with them the GETJPI and PROCSCAN request numbers a second time.
 * Same discipline, same reason.
 */
_Static_assert(sizeof(struct vms_procinfo) == 112,
               "vms_procinfo layout changed: process-table ioctl ABI break");
_Static_assert(sizeof(struct vms_setprn_args) == 72,
               "vms_setprn_args layout changed: VMS_IOCTL_SETPRN ABI break");
_Static_assert(sizeof(struct vms_getjpi_args) == 184,
               "vms_getjpi_args layout changed: VMS_IOCTL_GETJPI ABI break");
_Static_assert(sizeof(struct vms_procscan_args) == 120,
               "vms_procscan_args layout changed: VMS_IOCTL_PROCSCAN ABI break");
_Static_assert(sizeof(struct vms_setterm_args) == 8,
               "vms_setterm_args layout changed: VMS_IOCTL_SETTERM ABI break");
_Static_assert(sizeof(struct vms_ident_args) == 48,
               "vms_ident_args layout changed: VMS_IOCTL_SETIDENT ABI break");
_Static_assert(sizeof(struct vms_register_args) == 8,
               "vms_register_args layout changed: VMS_IOCTL_REGISTER ABI break");
/*
 * The inbound transfer buffer must be strictly larger than the
 * executive's inspection window, or an oversized name would be clipped
 * to exactly VMS_PRCNAM_SIZE-1 characters and pass name_is_valid() --
 * reintroducing the silent truncation this split exists to kill.
 */
_Static_assert(VMS_PRCNAM_XFER > VMS_PRCNAM_SIZE,
               "VMS_PRCNAM_XFER must exceed VMS_PRCNAM_SIZE or oversized names get truncated into valid ones");
_Static_assert(VMS_IOCTL_SETPRN == 0xC0485641u,
               "VMS_IOCTL_SETPRN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETJPI == 0xC0B85642u,
               "VMS_IOCTL_GETJPI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_PROCSCAN == 0xC0785643u,
               "VMS_IOCTL_PROCSCAN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETIDENT == 0xC0305644u,
               "VMS_IOCTL_SETIDENT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETTERM == 0xC0085645u,
               "VMS_IOCTL_SETTERM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_REGISTER == 0xC0085640u,
               "VMS_IOCTL_REGISTER encodes differently here than on the reference build");

/* ================================================================
 * P0 program region (vms-68f.i, in-process image activation foundation)
 *
 * docs/design-in-process-activation.md Part II, §A.2.1. On OpenVMS,
 * activating an image maps it into the process's P0 (program) region;
 * image rundown deletes that region and leaves P1 (process-permanent
 * state -- DCL's own code/data, the user stack, LNM$PROCESS) untouched.
 * There is exactly one VMS process throughout (§A.1.1, §A.1.3).
 *
 * THIS INCREMENT ONLY: the executive RECORDS a process's current P0
 * extent so it is observable (via struct vms_procinfo.p0_base/p0_limit,
 * above) and can be cleared. It does not itself map or unmap any memory
 * -- the P0 window reservation and the per-image PT_LOAD mmaps into it
 * are DCL/imgact$activate's job, in userspace, per the design's
 * MAP_FIXED-into-a-PROT_NONE-reservation model (§A.2.1 steps 1-4). This
 * ioctl pair is the executive's half of step 4 ("register the P0 extent
 * ... so it knows this process has an image mapped") and of rundown step
 * 4 ("executive marks the process image-less"). Activating an image in
 * P0 (increment iv), rundown releasing image-scoped executive state
 * (increment v) and the access-mode transitions around them (increment
 * iii) are NOT this increment's scope.
 *
 * INV-6: no /dev/vms -> the vms_kif_p0_map/p0_unmap wrappers return
 * SS$_NOSUCHDEV (src/libvmssys/vms_kif.c) -- there is no per-process
 * fallback that fabricates a mapped-or-unmapped answer when the
 * executive cannot be reached.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): this ioctl pair, its argument
 * layout and its request numbers are OVMX's own -- OpenVMS publishes no
 * public byte-level interface for "tell the executive my P0 extent",
 * only the property being reproduced (see the vms_procinfo comment
 * above). Nothing here is presented as a VMS-authentic wire format.
 */

struct vms_p0_args {
    uint64_t base;      /* P0_MAP: in, region base VA. P0_UNMAP: out, the
                         * extent that WAS mapped (0 if none was). */
    uint64_t limit;     /* P0_MAP: in, region limit VA (exclusive).
                         * P0_UNMAP: out, same convention as base. */
    uint32_t status;    /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_P0_MAP    _IOWR(VMS_IOC_MAGIC, 0x63, struct vms_p0_args)
#define VMS_IOCTL_P0_UNMAP  _IOWR(VMS_IOC_MAGIC, 0x64, struct vms_p0_args)

/*
 * ABI lock, same discipline as the process-table block above: these
 * ioctls fold sizeof(struct vms_p0_args) into their request number, so a
 * struct-size drift silently renumbers the ioctl (-ENOTTY, not a
 * mis-decode) rather than failing to compile. 0x63/0x64 sit in the gap
 * between the logical-name ioctls (vms_lnm.h, 0x60-0x62) and the mailbox
 * ioctls (vms_mbx.h, 0x70-0x74) -- neither header is touched by this
 * addition.
 */
_Static_assert(sizeof(struct vms_p0_args) == 24,
               "vms_p0_args layout changed: VMS_IOCTL_P0_MAP/P0_UNMAP ABI break");
_Static_assert(VMS_IOCTL_P0_MAP == 0xC0185663u,
               "VMS_IOCTL_P0_MAP encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_P0_UNMAP == 0xC0185664u,
               "VMS_IOCTL_P0_UNMAP encodes differently here than on the reference build");

/* ================================================================
 * Logical name tables (executive-resident LNM$SYSTEM/GROUP/JOB).
 *
 * The arena format, the mmap read path and the define/delete ioctls live
 * in vms_lnm.h, included here so every consumer of this header -- kernel
 * and userspace, on either architecture -- gets one frozen definition.
 * ================================================================ */
#include "vms_lnm.h"

/* ================================================================
 * Mailboxes (executive-resident MBAn:, vms-d44).
 *
 * The ioctl structures and request numbers live in vms_mbx.h, included
 * here for the same reason as vms_lnm.h above: one frozen definition for
 * every consumer, kernel and userspace, on either architecture.
 * ================================================================ */
#include "vms_mbx.h"

#endif /* _VMS_IOCTL_H */
