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
/*
 * ioctl macros for userspace -- use system macros if already defined.
 *
 * These MUST reproduce the kernel's per-architecture <asm/ioctl.h> encoding
 * byte-for-byte: the command number userspace passes to ioctl(2) is compared
 * for EQUALITY against the number the driver's switch computed from the SAME
 * macros under __KERNEL__ (which pulls the arch's real <asm/ioctl.h>). If the
 * two disagree, the driver falls through to `default: return -ENOTTY` and the
 * ioctl silently never reaches its handler.
 *
 * Most Linux architectures (x86_64, aarch64, arm, ...) use the asm-generic
 * encoding: a 2-bit direction field at bit 30 and a 14-bit size field. Alpha
 * (like MIPS/PowerPC/SPARC) inherits the OSF/1-derived encoding instead: a
 * 3-bit direction field at bit 29 with _IOC_NONE=1/_IOC_READ=2/_IOC_WRITE=4,
 * and a 13-bit size field. Under that layout _IOWR (READ|WRITE = 6 << 29)
 * COLLIDES with the generic _IOWR (3 << 30) -- both are 0xC000_0000 -- so an
 * all-_IOWR interface works by luck, but _IOR (2 << 29 vs 2 << 30) and _IOW
 * (4 << 29 vs 1 << 30) DIVERGE. That is exactly why VMS_IOCTL_DELIVERAST, the
 * one _IOR in the cross-process path, was unreachable on Alpha while every
 * _IOWR lock/eflag/register ioctl worked (rd vms-9535). Encode Alpha's layout
 * explicitly so freestanding userspace agrees with the Alpha kernel.
 */
#if defined(__alpha__)
/* OSF/1-derived encoding: dir(3) << 29 | size(13) << 16 | type << 8 | nr */
#ifndef _IO
#define _IO(type, nr)           (((1U) << 29) | ((type) << 8) | (nr))
#endif
#ifndef _IOR
#define _IOR(type, nr, size)    (((2U) << 29) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#ifndef _IOW
#define _IOW(type, nr, size)    (((4U) << 29) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#ifndef _IOWR
#define _IOWR(type, nr, size)   (((6U) << 29) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#endif
#else
/* asm-generic encoding: dir(2) << 30 | size(14) << 16 | type << 8 | nr */
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
#endif

#define VMS_IOC_MAGIC 'V'

/* ================================================================
 * Access Mode (3a)
 *
 * PSL_C_KERNEL..PSL_C_USER, AND THE VALUES 0..3 -- ORACLE-PINNED TO PUBLIC
 * DOCUMENTATION (CLAUDE.md Rule 8, the public-$PSLDEF branch, not a live
 * lab session: no SDA read is needed for a fact this stable across VMS's
 * history). OpenVMS's four processor access modes, most to least
 * privileged, and their 2-bit PSL<current mode> field encoding are
 * published in:
 *   - *OpenVMS Internals and Data Structures* (IDSM), "Access Modes and
 *     the Processor Status Longword" -- names the four modes in this exact
 *     order and states the field is 2 bits, VAX PSL<26:25>.
 *   - the public $PSLDEF macro (VSI OpenVMS System Services Reference
 *     Manual; OpenVMS Calling Standard) -- PSL$C_KERNEL=0, PSL$C_EXEC=1,
 *     PSL$C_SUPER=2, PSL$C_USER=3.
 * This tree has carried these four constants since vms-2b8; this comment
 * states increment (iii) of the in-process-activation design's requested
 * citation (docs/design-in-process-activation.md Part II §A.1.2) -- it
 * does not change a value.
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
#define VMS_PRV_V_MOUNT     17  /* execute the MOUNT ACP function (vms-651).
                                 * Public $PRVDEF documentation bit position
                                 * (VSI OpenVMS System Services Reference
                                 * Manual / Guide to System Security), same
                                 * provenance class as SYSNAM/GRPNAM/GRPPRV
                                 * above -- invariant across VAX/Alpha/
                                 * Itanium/x86, and the only unclaimed slot
                                 * between oracle-confirmed WORLD=16 and
                                 * OPER=18. src/libvms/include/prvdef.h
                                 * already carried PRV$V_MOUNT=17 from before
                                 * this item; the _Static_assert in
                                 * prv_agreement.c is what makes this copy
                                 * agree with that one, not what re-derives
                                 * either. */
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
#define VMS_PRV_M_MOUNT     (1ULL << VMS_PRV_V_MOUNT)
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
 *   MOUNT   cmd_mount / cmd_dismount (src/vmsdcl/dcl_cmd_misc.c) call
 *           vms_kif_chkpriv(PRV$M_MOUNT) -- itself vms_ioctl_chkpriv,
 *           i.e. THIS check against proc->cur_privs -- before mount(2)/
 *           umount(2)ing a volume (vms-651). Real kernel-enforced state,
 *           not a userspace getuid() check.
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
                             VMS_PRV_M_SYSNAM | VMS_PRV_M_GRPNAM | \
                             VMS_PRV_M_MOUNT)

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
    uint32_t owner_csid;        /* in: cluster CSID that OWNS this lock; 0 = the
                                 * local node (the calling process's own node).
                                 * A userspace $ENQ leaves this 0 (local hold);
                                 * the cross-node DLM dispatch
                                 * (vms_lock_dlm_xnode_dispatch) sets it to the
                                 * REMOTE requester's CSID so the master's lock
                                 * record is stamped with the identity it is held
                                 * FOR (DLM epic vms-7fa rung 2, vms-e8f1). Was a
                                 * reserved pad; same size, no ABI change. */
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

/*
 * DLM resource-directory + mastering readback (vms-ci.5 DB).
 *
 * A READ-ONLY diagnostic view of a resource's DLM state: which node is its
 * DIRECTORY (dir_csid, the node reached by hashing resnam), which node
 * MASTERS it (master_csid, 0 until mastered on first $ENQ), and how many
 * locks are granted on it. It does NOT create or master a resource -- an
 * unknown name comes back found=0 with master_csid=0 -- so it can be called
 * before and after an $ENQ to prove the local-master path actually mastered
 * the resource, rather than a test asserting a hand-set structure.
 *
 * is_local_master is (master_csid == local_csid), surfaced so a test need not
 * know the CSID value. Grounding: IDSM lock-management directory lookups
 * (mined transcript ch6-part02 pp. 6-18..6-35); docs/design-cluster-node.md §5.
 */
struct vms_resmaster_args {
    char     resnam[32];        /* in: resource name (null-terminated) */
    uint32_t found;             /* return: 1 if a resource block exists */
    uint32_t local_csid;        /* return: this node's CSID */
    uint32_t dir_csid;          /* return: directory node CSID for resnam */
    uint32_t master_csid;       /* return: mastering node CSID; 0 = unmastered */
    uint32_t is_local_master;   /* return: 1 if mastered by this node */
    uint32_t n_granted;         /* return: granted locks on the resource */
    uint32_t status;            /* return: SS$_ status */
    uint32_t remote_holder_csid;/* return: the CSID a REMOTE-held granted lock on
                                 * this resource is held FOR (the req_csid stamped
                                 * by the cross-node DLM grant); 0 if every grant
                                 * is local. Lets a test PROVE the master genuinely
                                 * holds a lock for a peer's cluster identity, not
                                 * just that n_granted rose (DLM epic vms-7fa rung
                                 * 2, vms-e8f1). Was a reserved pad; same size. */
};

#define VMS_IOCTL_ENQ       _IOWR(VMS_IOC_MAGIC, 0x30, struct vms_enq_args)
#define VMS_IOCTL_DEQ       _IOWR(VMS_IOC_MAGIC, 0x31, struct vms_deq_args)
#define VMS_IOCTL_CONVERT   _IOWR(VMS_IOC_MAGIC, 0x32, struct vms_enq_args)
#define VMS_IOCTL_GETLKI    _IOWR(VMS_IOC_MAGIC, 0x33, struct vms_getlki_args)
/*
 * Lock-manager ioctl range is 0x30-0x3F (device table starts at 0x50); 0x34
 * is the next free slot after GETLKI. DC/DD (remote forward, remaster) are
 * 0.4 and do not add a new ioctl here -- they extend the enqueue path.
 */
#define VMS_IOCTL_GET_RESMASTER _IOWR(VMS_IOC_MAGIC, 0x34, struct vms_resmaster_args)

/*
 * DLM cross-node lock-request dispatch (vms-94c, DLM epic vms-7fa rung 1).
 *
 * The RECEIVE side of the distributed lock manager. A DLM message that arrived
 * over SCS from a REMOTE node (decoded by src/vmsscs/scs_dlm.c) is marshalled
 * into the executive through this ioctl so it reaches the kernel lock manager's
 * cross-node handler (vms_lock_dlm_xnode_dispatch). Rung 1 is the TRANSPORT
 * only: the message reaches the handler decoded and the handler returns
 * SS$_UNSUPPORTED -- it does NOT grant, queue, dequeue, or deliver a blocking
 * AST (that is rung 2). INV-6: no fabricated cross-node grant; a cross-node op
 * honestly fails, exactly as dlm_resolve_master() already does for the SEND side.
 *
 * `op` carries a DLM message kind as a plain byte so this header takes no
 * dependency on src/vmsscs; the VMS_DLM_OP_* values below MUST match
 * scs_dlm.h's SCS_DLM_OP_* (scsd.c static-asserts they do).
 */
#define VMS_DLM_OP_ENQ      1u   /* lock/convert request  -> master  */
#define VMS_DLM_OP_GRANT    2u   /* status response       <- master  */
#define VMS_DLM_OP_DEQ      3u   /* dequeue request       -> master  */
#define VMS_DLM_OP_BLKAST   4u   /* blocking-AST notify   <- master  */

/*
 * The `status` an ENQ dispatch returns when the request was QUEUED on the master
 * (blocked behind an incompatible holder) rather than granted -- the contention
 * rung (vms-904c). It is deliberately NOT SS$_NORMAL (that means granted) and NOT
 * SS$_NOTQUEUED (that means declined, NOQUEUE): 0 is the VMS lock-status-block
 * "no completion posted yet" state (mirrors vms_lock_entry.grant_state == 0). The
 * grant arrives later -- when the holder releases -- as the request flips to
 * SS$_NORMAL and (in a live cluster) a deferred GRANT message is sent. INV-6: a
 * queued request is a REAL lock on the master's waiting queue, never a fake grant.
 */
#define VMS_DLM_STS_QUEUED  0u

struct vms_dlm_xnode_args {
    uint32_t op;                /* in: VMS_DLM_OP_* */
    uint32_t lkmode;            /* in: LCK$K_ mode (0..5) */
    uint32_t flags;             /* in: LCK$M_ flags */
    uint32_t req_lkid;          /* in: requester's lock handle */
    uint32_t master_lkid;       /* in: master's lock handle */
    uint32_t req_csid;          /* in: requesting node CSID */
    uint32_t master_csid;       /* in: mastering node CSID (0 = resolve) */
    char     resnam[32];        /* in: resource name (null-terminated) */
    uint8_t  valblk[LCK_VALBLK_SIZE]; /* in: value block */
    uint32_t status;            /* return: SS$_ status. ENQ granted immediately =>
                                 * SS$_NORMAL; ENQ queued (blocked, contention rung
                                 * vms-904c) => 0 (VMS_DLM_STS_QUEUED: no completion
                                 * status posted yet -- a later GRANT carries
                                 * SS$_NORMAL); ENQ+NOQUEUE incompatible =>
                                 * SS$_NOTQUEUED; higher rungs => SS$_UNSUPPORTED. */
    /*
     * Cross-node contention outputs (DLM epic vms-7fa rung 3, vms-904c). Filled by
     * the ENQ path so the requester/daemon can act on a QUEUED request:
     *   queued            - 1 when the request was placed on the master's real
     *                       waiting queue (blocked) rather than granted now.
     *   blocking_csid     - CSID of the cross-node HOLDER whose grant blocks this
     *                       request and that must receive a blocking AST (BLKAST);
     *                       0 when nothing blocks it across nodes.
     *   blocking_master_lkid - that holder's master lock handle (the BLKAST target).
     * On a DEQ these are 0. master_lkid above is (re)used as the ENQ output: the
     * master's lock handle for the granted-or-queued request.
     */
    uint32_t queued;
    uint32_t blocking_csid;
    uint32_t blocking_master_lkid;
};
#define VMS_IOCTL_DLM_XNODE _IOWR(VMS_IOC_MAGIC, 0x35, struct vms_dlm_xnode_args)

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
 * Size of a disk unit's BACKING Linux block-device name (e.g. "vda"), NUL
 * included. Long enough for the virtio-blk name space (vd[a-z], and vd[a-z][a-z]
 * if it ever grows). This is an OVMX construct with no VMS counterpart -- a VMS
 * unit is not "backed by" a Linux block device -- so the size is an OVMX design
 * choice (CLAUDE.md Rule 8), not a published VMS field width.
 */
#define VMS_BACKING_SIZE 16

/*
 * Size of the BACKING host network-interface name (e.g. "eth0", "enp0s1") the
 * executive records for the Ethernet unit ETH0: (vms-9d2), NUL included.
 * Long enough for a Linux IFNAMSIZ (16) name. Like VMS_BACKING_SIZE this is an
 * OVMX construct with no VMS counterpart -- a VMS device is not "backed by" a
 * host interface -- so it is an OVMX design choice (CLAUDE.md Rule 8), and it is
 * NEVER surfaced to a VMS program (INV-4): it is the executive's private record
 * of which real, driver-agnostic net device this unit was enumerated from.
 */
#define VMS_NETIF_SIZE 16

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
 * Resolve a DISK unit to the Linux block device the executive enumerated it
 * from (vms-3e8). The executive creates DKA0:/DKA100:/... at module init by
 * enumerating the node's virtio block devices (src/kernel/vms_devtab.c), so it
 * -- not the process -- is the one thing that knows which Linux block device
 * (vda/vdb/...) backs each VMS disk unit. A process that needs to open the
 * backing device (MOUNT, vms-651) asks the executive rather than scanning
 * /sys/block itself: the fact lives in the executive (CLAUDE.md Rule 11).
 *
 * OVMX CONSTRUCT, labelled (CLAUDE.md Rule 8): "the Linux block device behind a
 * VMS unit" has no VMS counterpart, so no public OpenVMS document publishes this
 * exchange. The DEVICE NAMING it resolves is doc-derived (see vms_devtab.c); the
 * backing-device coupling reported here is the OVMX side of that bridge.
 *
 * SS$_NOSUCHDEV if there is no such unit; SS$_IVDEVNAM if the name is not a
 * legal device name, or names a device that is not a DISK (only disk units have
 * a backing block device).
 */
struct vms_diskresolve_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: disk unit name, e.g. "DKA0:" */
    char     backing[VMS_BACKING_SIZE]; /* out: Linux block device, e.g. "vda" */
    uint32_t backing_major;             /* out: backing dev_t major */
    uint32_t backing_minor;             /* out: backing dev_t minor */
    uint32_t status;                    /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_DISK_RESOLVE _IOWR(VMS_IOC_MAGIC, 0x57, struct vms_diskresolve_args)

/*
 * $GETDVI for the VOLUME items of a mounted disk (vms-e6f). SHOW DEVICE's brief
 * and /FULL listings report, for a mounted Files-11 disk, its mount state, the
 * ODS-2 volume label, the volume size and the free-block count -- items that on
 * real VMS come from $GETDVI (DVI$_MNT, DVI$_VOLNAM, DVI$_MAXBLOCK,
 * DVI$_FREEBLOCKS, DVI$_CLUSTER). The base device table (struct vms_devinfo)
 * carries none of these: they are the ACP's, held in the executive-global
 * mounted-volume table a $MOUNT populates (src/kernel-core/vmsfs_acp.c). This
 * ioctl reads THAT table -- so every process on the node sees the same mount
 * state and the same label, never a per-process fake (CLAUDE.md Rule 11 / INV-6).
 *
 * A unit that is not a mounted ODS-2 volume returns SS$_NORMAL with mounted == 0
 * (it is simply not mounted -- not an error, not a fabricated mount). The label,
 * size, cluster factor and INDEXF geometry are the ones the executive VALIDATED
 * and recorded at $MOUNT from the home block / SCB; freeblocks is counted, at
 * call time, from the volume's BITMAP.SYS storage bitmap (the same bitmap the
 * ACP allocator reads), so it is a genuine reading and never a stored guess.
 * free_valid == 0 means the bitmap could not be read this call (a real I/O
 * error) -- freeblocks is then unset and a reader prints no free-block count
 * rather than the fabricated "0" (Rule 10).
 *
 * OVMX CONSTRUCT, labelled (CLAUDE.md Rule 8): OVMX reaches the executive over
 * /dev/vms, not a byte-level $GETDVI itemlist, so this flat arg struct is an
 * OVMX design choice. The DVI$_ items it carries and their MEANINGS are the
 * public ones ($GETDVI in the VSI System Services Reference Manual); the
 * VALUES are read from the genuine on-disk ODS-2 structures (the codec,
 * src/vmsfs/ods2, validated against a real VAX volume).
 */
#define VMS_GETVOL_LABEL_SIZE 16
struct vms_getvol_args {
    char     devnam[VMS_DEVNAM_SIZE];   /* in: unit name, e.g. "DKA0:" */
    uint32_t status;                    /* out: SS$_ status */
    uint32_t mounted;                   /* out: 1 = a mounted ODS-2 volume */
    uint32_t volsize;                   /* out: DVI$_MAXBLOCK, SCB volume size (blocks) */
    uint32_t freeblocks;                /* out: DVI$_FREEBLOCKS (valid iff free_valid) */
    uint32_t free_valid;                /* out: 1 = freeblocks was read this call */
    uint32_t cluster;                   /* out: DVI$_CLUSTER, storage-bitmap cluster factor */
    uint32_t transcnt;                  /* out: file-class channels assigned (Trans Count) */
    char     volnam[VMS_GETVOL_LABEL_SIZE]; /* out: NUL-terminated ODS-2 volume label */
};

#define VMS_IOCTL_GETVOL _IOWR(VMS_IOC_MAGIC, 0x58, struct vms_getvol_args)

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
_Static_assert(sizeof(struct vms_diskresolve_args) == 48,
               "struct vms_diskresolve_args changed size -- disk unit resolution would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_getvol_args) == 60,
               "struct vms_getvol_args changed size -- $GETDVI volume items would decode at the wrong offsets");

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
_Static_assert(VMS_IOCTL_DISK_RESOLVE == 0xC0305657u,
               "VMS_IOCTL_DISK_RESOLVE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_GETVOL == 0xC03C5658u,
               "VMS_IOCTL_GETVOL encodes differently here than on the reference build");

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

/*
 * Invoking CLI command-line bound (vms-f60d). OVMX DESIGN CHOICE
 * (CLAUDE.md Rule 8): 256 bytes holds the classic 255-character DCL
 * command line the OpenVMS User's Manual documents, plus a NUL for C
 * readers. Public docs give the SEMANTICS of a CLI command line and the
 * $CLI get-command-line callback (ovmx_activation.h) but no byte-level
 * wire format, so this size and the ioctl structs carrying it are OVMX's
 * own -- not presented as VMS-authentic.
 */
#define VMS_CLI_CMDLINE_SIZE 256

/*
 * The per-process QUOTA BLOCK (vms-a7e) -- the VMS Job Information Block
 * (JIB) limits SHOW PROCESS/QUOTAS, SHOW QUOTA and F$GETJPI report. The
 * field set and its F$GETJPI item codes are from the PUBLIC OpenVMS System
 * Services Reference ($GETJPI item-code table) and the DCL Dictionary
 * (SHOW PROCESS/QUOTAS), CLAUDE.md Rule 8 -- never disassembled. Each
 * longword is named for the item that reports it:
 *
 *   astlm     JPI$_ASTLM       AST limit
 *   biolm     JPI$_BIOLM       buffered I/O limit
 *   bytlm     JPI$_BYTLM       buffered I/O byte-count quota (BYTLM)
 *   diolm     JPI$_DIOLM       direct I/O limit
 *   enqlm     JPI$_ENQLM       enqueue (lock) quota
 *   fillm     JPI$_FILLM       open file limit
 *   pgflquota JPI$_PGFLQUOTA   paging file quota
 *   prclm     JPI$_PRCLM       subprocess quota
 *   tqelm     JPI$_TQLM        timer queue entry quota
 *   wsdefault JPI$_WSDEFAULT   default working set size
 *   wsquota   JPI$_WSQUOTA     working set quota
 *   wsextent  JPI$_WSEXTENT    working set extent
 *
 * SOURCE STATUS, STATED NOT IMPLIED (CLAUDE.md Rule 10, INV-6): OVMX has
 * no quota/JIB facility yet -- SYSUAF (src/libvms/include/sysuaf.h) carries
 * username/uic/flags/priv and NOTHING quota-shaped, and the executive
 * charges nothing against a limit. So every field here is STRUCTURAL: the
 * data model the follow-ups (SHOW QUOTA vms-9d4, SHOW PROCESS/QUOTAS) need
 * a slot in, carried with VMS_PI_V_QUOTA CLEAR so a reader never mistakes a
 * zero for a measured limit. Filling it faithfully is a quota-system build
 * (a SYSUAF quota extension + executive charging), tracked separately --
 * NOT fabricated here, exactly as SHOW SYSTEM's absent columns were left
 * absent rather than invented (docs/oracle/vax73-show-system-process.md §5).
 */
struct vms_jib_quota {
    uint32_t astlm;
    uint32_t biolm;
    uint32_t bytlm;
    uint32_t diolm;
    uint32_t enqlm;
    uint32_t fillm;
    uint32_t pgflquota;
    uint32_t prclm;
    uint32_t tqelm;
    uint32_t wsdefault;
    uint32_t wsquota;
    uint32_t wsextent;
};

/*
 * fields_valid bits (vms-a7e). A reader MUST test the bit before using the
 * matching field, the same discipline as `redacted` and $GETJPI's return
 * length: an unsourced accounting/quota field is not distinguishable from a
 * genuine zero by its value, so the executive says which it is. A field
 * whose bit is CLEAR carries no measured value and must be displayed as
 * absent, never as zero (the fabrication class vms-8019/vms-6a7 deleted).
 */
#define VMS_PI_V_CPUTIM     0x00000001u  /* cputim is sourced */
#define VMS_PI_V_PAGEFLTS   0x00000002u  /* pageflts is sourced */
#define VMS_PI_V_PAGES      0x00000004u  /* pages (resident) is sourced */
#define VMS_PI_V_LOGINTIM   0x00000008u  /* logintim is sourced */
#define VMS_PI_V_STATE      0x00000010u  /* sched_state is sourced */
#define VMS_PI_V_PRI        0x00000020u  /* cur_pri is sourced */
#define VMS_PI_V_PRIB       0x00000040u  /* base_pri is sourced */
#define VMS_PI_V_DIRIO      0x00000080u  /* dirio is sourced */
#define VMS_PI_V_BUFIO      0x00000100u  /* bufio is sourced */
#define VMS_PI_V_QUOTA      0x00000200u  /* quota block is sourced */

/*
 * proc_type values (vms-c17). The process CLASSIFICATION SHOW USERS uses to
 * fill the Interactive/Subprocess/Batch columns, DECLARED by the executive
 * rather than inferred by the reader -- the same discipline as `redacted` and
 * `fields_valid`. A reader that has to GUESS from a zeroed field (is this the
 * absence of a terminal, or a subprocess whose root it cannot see?) eventually
 * guesses wrong, so the executive says which one it is.
 *
 * The discriminator is the PCB's job_id (vms_internal.h): a job root has
 * job_id == vms_pid; a SPAWNed subprocess inherits its root's job_id. A row is
 * INTERACTIVE when it is a terminal-bound job root, SUBPROCESS when its job
 * root is terminal-bound, and OTHER when neither holds (a detached/system
 * process -- not a "user"). See proc_fill_info() in
 * src/kernel-core/vms_proctab.c for how the value is derived.
 *
 * BATCH IS RESERVED AND NEVER SET TODAY. OVMX has no batch EXECUTION engine
 * (SUBMIT queues an entry but forks/execs nothing -- see the structural note
 * in src/vmsdcl/dcl_cmd_show.c's cmd_show_users), so no row is ever a batch
 * job. The value exists so the wire enum is complete and a future batch
 * executor has a name to set, not because anything produces it now.
 */
#define VMS_PROC_T_OTHER        0u  /* detached / system process (not a "user") */
#define VMS_PROC_T_INTERACTIVE  1u  /* job root with a terminal (login) */
#define VMS_PROC_T_SUBPROCESS   2u  /* belongs to a parent's job (SPAWN) */
#define VMS_PROC_T_BATCH        3u  /* batch job root (reserved -- no engine yet) */

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
    uint8_t  proc_type;   /* VMS_PROC_T_* -- process classification (vms-c17).
                           * Withheld like the identity fields on a redacted
                           * row (job membership is not enumeration): set only
                           * below proc_fill_info()'s redaction early return,
                           * so a redacted row carries the OTHER default. Fits
                           * the existing padding -- struct stays 216 bytes. */
    uint8_t  pad[1];
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

    /*
     * P1 control-region extent (vms-68f.ii, increment (ii) of the Option A
     * in-process image activation design -- docs/design-in-process-
     * activation.md Part II §A.1.1, §A.2.1). [p1_base, p1_limit) is the
     * virtual-address range VMS_IOCTL_P1_MAP last registered for this
     * process: DCL's own process-permanent state (command-loop context,
     * LNM$PROCESS root, RMS process context, user stack). Both zero until
     * a process registers one.
     *
     * THE PROPERTY THIS PAIR EXISTS TO MAKE OBSERVABLE, ALONGSIDE p0_base/
     * p0_limit ABOVE: P0 is per-image and torn down at rundown; P1 is
     * process-permanent and outlives every P0 map/unmap cycle. Unlike
     * p0_base/p0_limit, there is no VMS_IOCTL_P1_UNMAP -- a process does
     * not "run down" its own control region; it lasts for the process's
     * lifetime, same as the design's P1 lifetime row (§A.1.1) says. The
     * executive enforces the other half of that property structurally:
     * VMS_IOCTL_P0_UNMAP (vms_p0.c) touches only proc->p0_base/p0_limit,
     * never proc->p1_base/p1_limit -- see vms_p1.c's header for why that
     * separation, not a shared clear path, is what makes "P0 deleted on
     * rundown, P1 survives" true here rather than merely documented.
     *
     * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), same footing as p0_base/
     * p0_limit above: OpenVMS publishes no $GETJPI item reporting a raw P1
     * base/limit virtual address pair. What is pinned to the design (IDSM,
     * "Process Address Space") is the PROPERTY -- a process-permanent
     * control region distinct from the per-image program region -- not a
     * byte-level wire format for these two field names.
     *
     * WITHHELD WITH THE REST OF THE IDENTITY on a row the caller may not
     * $GETJPI, same placement rule as p0_base/p0_limit and `terminal`
     * above.
     */
    uint64_t p1_base;
    uint64_t p1_limit;

    /*
     * SCHEDULING, ACCOUNTING, QUOTA (vms-a7e) -- the executive row the
     * SHOW/SET parity tree (vms-8ad) reads. Field names carry their
     * F$GETJPI item codes from the PUBLIC OpenVMS System Services Reference
     * ($GETJPI item-code table), CLAUDE.md Rule 8. Two source classes,
     * and the split is the whole point of fields_valid:
     *
     *  SOURCED, a REAL property of the real Linux task the executive owns,
     *  measured in-kernel from proc->pid_ref (vms_proctab.c) -- the SAME
     *  justification the /proc-derived JPI$_CPUTIM already carried
     *  (src/libvms/syssvc/sys_process.c jpi_cputim's header), now moved
     *  INTO the executive so a VMS command READS a facility instead of
     *  being a second source (CLAUDE.md Rule 11):
     *     cputim   JPI$_CPUTIM    utime+stime, 10ms units
     *     pageflts JPI$_PAGEFLTS  min_flt + maj_flt
     *     pages    JPI$_PPGCNT    resident set size, in pages
     *     logintim JPI$_LOGINTIM  process creation time, VMS quadword
     *  These are carried on EVERY row including a redacted one, because a
     *  SHOW SYSTEM row is NOT privileged on VMS -- the oracle printed
     *  State/Pri/I/O/CPU/Page-flts/Pages for a cross-group process the same
     *  caller could not $GETJPI a single item from (docs/oracle/
     *  vax73-show-system-process.md §1.2). They are read from the
     *  executive's OWN pid_ref, not from linux_pid (which redaction
     *  zeroes), which is what lets a redacted row still show real
     *  accounting and closes the divergence §1.2 recorded.
     *
     *  STRUCTURAL, no faithful OVMX source yet, carried with the valid bit
     *  CLEAR so a reader shows them absent rather than as a fabricated
     *  value:
     *     sched_state JPI$_STATE  -- OVMX has no VMS scheduler; mapping a
     *                    Linux task state onto CUR/COM/LEF/HIB is the
     *                    "unpinned invention" §5.1 refused.
     *     base_pri    JPI$_PRIB   } OVMX has no VMS priority. Base priority
     *     cur_pri     JPI$_PRI    } is a SYSUAF attribute VMS's UAF carries
     *                    and OVMX's does not (yet); current priority is a
     *                    scheduler-derived value with no OVMX scheduler.
     *     dirio       JPI$_DIRIO  } Linux has no VMS direct/buffered I/O
     *     bufio       JPI$_BUFIO  } split; a syscall count wearing this
     *                    heading would be a mislabel (the fabrication class).
     *     quota       (see struct vms_jib_quota) -- no quota facility.
     */
    uint32_t fields_valid;   /* VMS_PI_V_* bitmask over the fields below */
    uint8_t  sched_state;    /* JPI$_STATE  (structural) */
    uint8_t  base_pri;       /* JPI$_PRIB   (structural) */
    uint8_t  cur_pri;        /* JPI$_PRI    (structural) */
    uint8_t  acct_pad;       /* keep the longwords below 4-aligned */
    uint32_t dirio;          /* JPI$_DIRIO  (structural) */
    uint32_t bufio;          /* JPI$_BUFIO  (structural) */
    uint32_t pageflts;       /* JPI$_PAGEFLTS (sourced) */
    uint32_t pages;          /* JPI$_PPGCNT   (sourced) */
    uint32_t cputim;         /* JPI$_CPUTIM, 10ms units (sourced) */
    uint32_t quad_pad;       /* keep logintim 8-aligned */
    uint64_t logintim;       /* JPI$_LOGINTIM, VMS quadword (sourced) */
    struct vms_jib_quota quota;  /* (structural) */
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
/*
 * VMS_JPI_SEL_LINUX_PID (GETEXIT only, vms-707): read the completion $STATUS of
 * a process named by its backing Linux pid, carried in the getexit args'
 * `vms_pid` field. This is the primitive DCL's RUN uses to recover the true
 * condition value of an image it activated through the fork()+execve() fallback:
 * that child shares DCL's VMS PID (VMS_IOCTL_REGISTER_CONTINUE), so a by-VMS-PID
 * read is ambiguous between DCL and the child, but the child's Linux pid -- which
 * DCL holds from fork() -- names its PCB row uniquely. Read before the child is
 * reaped (waitpid); an authorized read gated by vms_proc_may_read() exactly like
 * SEL_PID. GETJPI does NOT accept this selector (its switch rejects it).
 */
#define VMS_JPI_SEL_LINUX_PID 3

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
 * Construct the SYSTEM identity onto the calling process (vms-a17e).
 *
 * THE OPA0: PRECEDENT, APPLIED TO IDENTITY. On OpenVMS, EXEC_INIT
 * constructs the system process's identity; LOGINOUT is SYSUAF's FIRST
 * reader. Before this ioctl, OVMX inverted that: a userspace process
 * (PROVISION.EXE) read SYSUAF's SYSTEM record and handed the values to
 * VMS_IOCTL_SETIDENT -- the exact ioctl reserved for LOGINOUT
 * authenticating an ARBITRARY user. This ioctl is different in the one
 * way that matters: IT TAKES NO IDENTITY ARGUMENTS. There is no
 * username, uic, or authorized_privs field for a caller to supply,
 * because the identity is not a claim being ratified -- it is a
 * constant the executive already owns (VMS_SYSTEM_UIC /
 * VMS_PRV_M_SYSTEM_ALL, vms_internal.h), exactly as OPA0:'s name and
 * characteristics are constants vms_devtab_init() creates at module
 * load rather than values a process registers.
 *
 * THE GATE IS THE SAME REAL KERNEL CREDENTIAL vms_proc_register()
 * already uses to decide the enforced privilege set: capable
 * (CAP_SYS_ADMIN). This is not a new trust boundary -- a CAP_SYS_ADMIN
 * caller could already reach an equivalent result today by calling
 * VMS_IOCTL_SETIDENT("SYSTEM", [1,4], PRV$M_ALL) itself (it holds
 * SETPRV via VMS_PRV_M_ENFORCED, so vms_ioctl_setident()'s subset check
 * never engages). What moves is WHERE the specific values SYSTEM/[1,4]/
 * ALL live: compiled into the executive, not read out of
 * SYS$SYSTEM:SYSUAF.DAT by the caller first.
 *
 * (Self-targeted only, same as VMS_IOCTL_SETIDENT, and for the same
 * reason: OVMX has no VMS behaviour pinned for stamping ANOTHER
 * process's identity.)
 *
 * SS$_NOPRIV if the caller lacks CAP_SYS_ADMIN.
 */
struct vms_establish_system_args {
    uint32_t status;      /* return: SS$_ status */
    uint32_t pad;
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

/*
 * $HIBER / $WAKE, executive-resident (vms-feb).
 *
 * VMS_IOCTL_HIBER blocks the calling process in the executive until either a
 * $WAKE is pending for it (wake_pending, set by VMS_IOCTL_WAKE) OR an AST
 * becomes deliverable to it (an entry queued into its ast[] by $DCLAST, a
 * mailbox write-attention write, or a lock AST -- see vms_ast_notify_arrival).
 * On return `woken` is 1 iff the release was a $WAKE (which the ioctl consumes,
 * clearing wake_pending) and 0 iff it was an AST becoming deliverable. sys$hiber
 * loops: it drains and runs the deliverable ASTs in userspace after each return
 * (an AST may itself $WAKE), and returns from $HIBER only once `woken` is 1.
 * This is what makes $HIBER interruptible by asynchronous AST delivery.
 *
 * VMS_IOCTL_WAKE sets the sticky wake bit on the target process (the caller
 * when vms_pid == 0, else the process with that VMS PID, gated by GROUP/WORLD
 * like every other cross-process control operation) and wakes it if it is
 * hibernating. wake_pending is sticky, so a $WAKE that precedes the $HIBER
 * makes that $HIBER fall straight through (VSI System Services Reference,
 * $WAKE/$HIBER).
 */
struct vms_hiber_args {
    uint32_t woken;             /* return: 1 = released by $WAKE, 0 = by an AST */
    uint32_t status;            /* return: SS$_ status */
};

struct vms_wake_args {
    uint32_t vms_pid;           /* target VMS PID; 0 = the calling process */
    uint32_t status;            /* return: SS$_ status */
};

/*
 * $EXIT / $STATUS (vms-f60d) -- the executive facility IMGACT calls when an
 * activated VMS-standard image's crt0 (its main()) returns, so the returned
 * VMS condition value becomes the process's real completion status instead of
 * a fail-honest stub (imgact.c imgact_vms_exit; ovmx_activation.h).
 *
 * VMS_IOCTL_SETEXIT records `condition` as the calling process's image
 * completion $STATUS in the executive PCB (proc->exit_status). The full
 * longword IS $STATUS; bit<0> (STS$M_SUCCESS) is the success/fail bit that
 * $STATUS carries and bits<2:0> (STS$V_SEVERITY) are the $SEVERITY, exactly
 * as the OpenVMS DCL Dictionary defines $STATUS/$SEVERITY. The executive
 * echoes those decoded fields back and, as an OVMX design choice (CLAUDE.md
 * Rule 8 -- POSIX has no VMS counterpart), maps the condition to a Linux
 * exit code the caller (IMGACT) then passes to exit_group(2): 0 on success
 * (bit<0> set), else nonzero. The AUTHORITATIVE completion status is the
 * recorded longword, not the lossy exit code.
 */
struct vms_exit_args {
    uint32_t condition;         /* in:  VMS condition value to record as $STATUS */
    uint32_t status;            /* out: SS$_ status of the record operation */
    uint32_t exit_code;         /* out: OVMX POSIX exit code mapped from condition */
    uint8_t  success;           /* out: bit<0> of condition (STS$M_SUCCESS) */
    uint8_t  severity;          /* out: bits<2:0> of condition (STS$V_SEVERITY) */
    uint8_t  pad[2];
};

/*
 * VMS_IOCTL_GETEXIT reads back a process's recorded image completion status.
 * select is VMS_JPI_SEL_SELF (the caller's own $STATUS -- the invoking CLI
 * reading the status of the image it just ran) or VMS_JPI_SEL_PID (another
 * process, by VMS PID). A cross-process read is AUTHORIZED, NOT FREE: it is
 * gated by vms_proc_may_read() exactly like $GETJPI (same UIC group, or
 * WORLD) and returns SS$_NOPRIV with no data otherwise. has_exited is 0 (and
 * condition 0) when no image has recorded a status yet -- a reader must not
 * infer "not exited" from a zero condition, since 0 is a legal (warning-
 * severity) value. OVMX DESIGN CHOICE (Rule 8): the by-PID observation of a
 * process's completion status and this struct's layout are OVMX's own.
 */
struct vms_getexit_args {
    uint32_t select;            /* in:  VMS_JPI_SEL_SELF or VMS_JPI_SEL_PID */
    uint32_t vms_pid;           /* in:  target VMS PID when select == SEL_PID */
    uint32_t condition;         /* out: recorded $STATUS condition value */
    uint32_t status;            /* out: SS$_ status of the read */
    uint8_t  has_exited;        /* out: 1 iff an image completion status exists */
    uint8_t  success;           /* out: bit<0> of condition (STS$M_SUCCESS) */
    uint8_t  severity;          /* out: bits<2:0> of condition (STS$V_SEVERITY) */
    uint8_t  pad;
};

/*
 * CLI invocation context (vms-f60d) -- the executive source for IMGACT's
 * cliflag and cli_util->get_command_line (ovmx_activation.h). The invoking
 * CLI (DCL) records its command line and cliflag with VMS_IOCTL_SETCLI; the
 * image it activates reads the SAME context back with VMS_IOCTL_GETCLI (self
 * only -- an image asks for its OWN invoking command line), inheriting it
 * from the CLI's PCB at VMS_IOCTL_REGISTER_CONTINUE time. This is why the
 * command line lives in the executive rather than a Linux env var: it is a
 * fact the executive owns and hands down, not a string the image declares
 * about itself (conductor ruling, INV-6). cliflag == 0 means "no CLI"
 * (GETCLI then returns cli_present 0 and a zero-length command line, and
 * decc$main derives argv[0] from image_file_desc instead).
 */
struct vms_setcli_args {
    uint8_t  cliflag;                     /* in:  1 = invoked from a CLI/DCL */
    uint8_t  pad;
    uint16_t length;                      /* in:  command-line length in bytes */
    uint32_t status;                      /* out: SS$_ status */
    char     command[VMS_CLI_CMDLINE_SIZE];  /* in:  invoking DCL command line */
};

struct vms_getcli_args {
    uint8_t  cliflag;                     /* out: 1 = invoked from a CLI/DCL */
    uint8_t  pad;
    uint16_t length;                      /* out: command-line length in bytes */
    uint32_t status;                      /* out: SS$_ status */
    char     command[VMS_CLI_CMDLINE_SIZE];  /* out: invoking DCL command line */
};

#define VMS_IOCTL_SETPRN    _IOWR(VMS_IOC_MAGIC, 0x41, struct vms_setprn_args)
#define VMS_IOCTL_GETJPI    _IOWR(VMS_IOC_MAGIC, 0x42, struct vms_getjpi_args)
#define VMS_IOCTL_PROCSCAN  _IOWR(VMS_IOC_MAGIC, 0x43, struct vms_procscan_args)
#define VMS_IOCTL_SETIDENT  _IOWR(VMS_IOC_MAGIC, 0x44, struct vms_ident_args)
#define VMS_IOCTL_SETTERM   _IOWR(VMS_IOC_MAGIC, 0x45, struct vms_setterm_args)
#define VMS_IOCTL_ESTABLISH_SYSTEM  _IOWR(VMS_IOC_MAGIC, 0x46, struct vms_establish_system_args)
#define VMS_IOCTL_HIBER     _IOWR(VMS_IOC_MAGIC, 0x47, struct vms_hiber_args)
#define VMS_IOCTL_WAKE      _IOWR(VMS_IOC_MAGIC, 0x48, struct vms_wake_args)
#define VMS_IOCTL_SETEXIT   _IOWR(VMS_IOC_MAGIC, 0x49, struct vms_exit_args)
#define VMS_IOCTL_GETEXIT   _IOWR(VMS_IOC_MAGIC, 0x4A, struct vms_getexit_args)
#define VMS_IOCTL_SETCLI    _IOWR(VMS_IOC_MAGIC, 0x4B, struct vms_setcli_args)
#define VMS_IOCTL_GETCLI    _IOWR(VMS_IOC_MAGIC, 0x4C, struct vms_getcli_args)

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
 *
 * THEY MOVED A THIRD TIME WHEN p1_base/p1_limit WERE ADDED (vms-68f.ii):
 * procinfo 112 -> 128, getjpi_args 184 -> 200, procscan_args 120 -> 136,
 * and with them the GETJPI and PROCSCAN request numbers a third time.
 * Same discipline, same reason.
 */
_Static_assert(sizeof(struct vms_procinfo) == 216,
               "vms_procinfo layout changed: process-table ioctl ABI break");
_Static_assert(sizeof(struct vms_setprn_args) == 72,
               "vms_setprn_args layout changed: VMS_IOCTL_SETPRN ABI break");
_Static_assert(sizeof(struct vms_getjpi_args) == 288,
               "vms_getjpi_args layout changed: VMS_IOCTL_GETJPI ABI break");
_Static_assert(sizeof(struct vms_procscan_args) == 224,
               "vms_procscan_args layout changed: VMS_IOCTL_PROCSCAN ABI break");
_Static_assert(sizeof(struct vms_setterm_args) == 8,
               "vms_setterm_args layout changed: VMS_IOCTL_SETTERM ABI break");
_Static_assert(sizeof(struct vms_ident_args) == 48,
               "vms_ident_args layout changed: VMS_IOCTL_SETIDENT ABI break");
_Static_assert(sizeof(struct vms_establish_system_args) == 8,
               "vms_establish_system_args layout changed: VMS_IOCTL_ESTABLISH_SYSTEM ABI break");
_Static_assert(sizeof(struct vms_register_args) == 8,
               "vms_register_args layout changed: VMS_IOCTL_REGISTER ABI break");
_Static_assert(sizeof(struct vms_exit_args) == 16,
               "vms_exit_args layout changed: VMS_IOCTL_SETEXIT ABI break");
_Static_assert(sizeof(struct vms_getexit_args) == 20,
               "vms_getexit_args layout changed: VMS_IOCTL_GETEXIT ABI break");
_Static_assert(sizeof(struct vms_setcli_args) == 8 + VMS_CLI_CMDLINE_SIZE,
               "vms_setcli_args layout changed: VMS_IOCTL_SETCLI ABI break");
_Static_assert(sizeof(struct vms_getcli_args) == 8 + VMS_CLI_CMDLINE_SIZE,
               "vms_getcli_args layout changed: VMS_IOCTL_GETCLI ABI break");
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
_Static_assert(VMS_IOCTL_GETJPI == 0xC1205642u,
               "VMS_IOCTL_GETJPI encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_PROCSCAN == 0xC0E05643u,
               "VMS_IOCTL_PROCSCAN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETIDENT == 0xC0305644u,
               "VMS_IOCTL_SETIDENT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETTERM == 0xC0085645u,
               "VMS_IOCTL_SETTERM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ESTABLISH_SYSTEM == 0xC0085646u,
               "VMS_IOCTL_ESTABLISH_SYSTEM encodes differently here than on the reference build");
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
 * P1 control region (vms-68f.ii, in-process image activation foundation,
 * increment (ii))
 *
 * docs/design-in-process-activation.md Part II, §A.1.1, §A.2.1. On
 * OpenVMS, P1 is the process's control region -- DCL's own code and data,
 * the user stack, LNM$PROCESS, the RMS process context, the image-
 * activation context -- and it is PROCESS-PERMANENT: it is established
 * once, when the process is created, and lasts for the process's whole
 * lifetime, unlike P0 (per-image, deleted every rundown).
 *
 * THIS INCREMENT ONLY: the executive RECORDS a process's P1 extent so it
 * is observable (via struct vms_procinfo.p1_base/p1_limit, above) and
 * DISTINGUISHES it from P0 by giving it a separate field pair, a separate
 * lock (struct vms_proc::p1_lock, vms_internal.h) and a separate ioctl --
 * so that nothing which clears a P0 extent can reach a P1 extent by
 * sharing state with it. There is deliberately NO VMS_IOCTL_P1_UNMAP: on
 * the design this pair models, a process does not tear down its own
 * control region mid-lifetime the way image rundown tears down P0 (§A.1.1
 * lifetime row: P0 "per-image", P1 "process lifetime"). Actually mapping
 * DCL's P1 window and laying its process-permanent state into it in
 * userspace, and the access-mode transitions around P0/P1 (increment
 * iii), are NOT this increment's scope -- see vms-68f's decomposition.
 *
 * INV-6: no /dev/vms -> the vms_kif_p1_map wrapper returns SS$_NOSUCHDEV
 * (src/libvmssys/vms_kif.c) -- there is no per-process fallback that
 * fabricates a registered-or-not answer when the executive cannot be
 * reached.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): this ioctl, its argument layout
 * and its request number are OVMX's own -- OpenVMS publishes no public
 * byte-level interface for "tell the executive my P1 extent", only the
 * property being reproduced (see the vms_procinfo comment above). Nothing
 * here is presented as a VMS-authentic wire format.
 */

struct vms_p1_args {
    uint64_t base;      /* P1_MAP: in, region base VA. */
    uint64_t limit;     /* P1_MAP: in, region limit VA (exclusive). */
    uint32_t status;    /* return: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_P1_MAP    _IOWR(VMS_IOC_MAGIC, 0x65, struct vms_p1_args)

/*
 * ABI lock, same discipline as the P0 block above: this ioctl folds
 * sizeof(struct vms_p1_args) into its request number, so a struct-size
 * drift silently renumbers the ioctl (-ENOTTY, not a mis-decode) rather
 * than failing to compile. 0x65 sits in the gap between the P0 ioctls
 * (0x63-0x64, just above) and the mailbox ioctls (vms_mbx.h, 0x70-0x74) --
 * neither is touched by this addition.
 */
_Static_assert(sizeof(struct vms_p1_args) == 24,
               "vms_p1_args layout changed: VMS_IOCTL_P1_MAP ABI break");
_Static_assert(VMS_IOCTL_P1_MAP == 0xC0185665u,
               "VMS_IOCTL_P1_MAP encodes differently here than on the reference build");

/* ================================================================
 * Access-mode transition primitive (vms-68f.iii, in-process image
 * activation foundation, increment (iii))
 *
 * docs/design-in-process-activation.md Part II §A.1.2, §A.1.3, §A.2.3.
 * DCL activates an image by dropping to User mode to enter it, and image
 * rundown returns to Supervisor -- the CHMx/REI-equivalent transition pair
 * this increment provides. It is deliberately NOT the general-purpose
 * VMS_IOCTL_SETMODE above (which can request ANY mode and is gated purely
 * on the CMEXEC/CMKRNL privilege bits): ENTER_IMAGE/IMAGE_RUNDOWN are a
 * PAIRED, EXECUTIVE-VERIFIED descent-and-return, so the return leg can
 * never be used to reach an arbitrary mode -- only the exact mode the
 * matching ENTER_IMAGE recorded, and only once, per struct vms_proc's
 * image_active flag (vms_internal.h). This is what makes it "controlled":
 * a caller that never legitimately descended cannot manufacture a return.
 *
 * VMS_IOCTL_ENTER_IMAGE: the CALLER'S CURRENT MODE MUST BE PSL_C_SUPER (SS$_
 * NOPRIV otherwise -- descending is not offered from Kernel/Exec, which this
 * increment's ceiling does not need: DCL's command loop is the Supervisor
 * caller the design names, §A.1.2). On success, records the prior mode and
 * sets current mode to PSL_C_USER; image_active becomes 1. No privilege is
 * required to DESCEND (§A.2.3(a): "lowering mode is unprivileged"), matching
 * VMS_IOCTL_SETMODE's own existing rule for a drop.
 *
 * VMS_IOCTL_IMAGE_RUNDOWN: refused (SS$_NOPRIV) unless image_active is
 * currently 1 -- i.e. unless THIS process is mid a controlled descent that
 * ENTER_IMAGE actually performed. On success, restores current mode to
 * whatever ENTER_IMAGE recorded (Supervisor, on every path this increment's
 * design uses) and clears image_active, so a second RUNDOWN with no
 * intervening ENTER_IMAGE is refused the same way. THIS is the negative
 * control the design's §A.5 item 5 and this item's own task both name:
 * a User-mode caller cannot manufacture its way back to Supervisor except
 * through this exact pair, and cannot replay the return leg.
 *
 * ENFORCEMENT CEILING (Rule 10, stated plainly): this is software-tracked
 * current-mode plus a paired-transition check, not a hardware ring. See
 * §A.2.3 for the full honesty statement -- what this DOES enforce is real
 * (the /dev/vms boundary refuses the escalation), what it does not (per-page
 * four-mode memory protection) is Intel MPK/PKU territory, explicitly
 * DEFERRED post-1.0 (vms-978) and NOT implemented here.
 *
 * INV-6: no /dev/vms -> the vms_kif_enter_image/vms_kif_image_rundown
 * wrappers (src/libvmssys/vms_kif.c) return SS$_NOSUCHDEV -- there is no
 * per-process fallback that fabricates a mode transition when the executive
 * cannot be reached.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): this ioctl pair, its argument
 * layout and its request numbers are OVMX's own -- OpenVMS publishes no
 * public byte-level interface for "tell the executive to perform a
 * change-mode transition"; CHMx/REI are VAX/Alpha/x86 PRIVILEGED
 * INSTRUCTIONS, not a documented wire format. What is pinned to the design
 * (IDSM, "Access Modes and the PSL"; §A.1.2-§A.1.3 above) is the PROPERTY
 * these two ioctls exist to reproduce in software: a controlled descent to
 * User for image execution and a controlled, paired return to Supervisor at
 * rundown. Nothing here is presented as a VMS-authentic wire format.
 */

struct vms_modexfer_args {
    uint8_t  prev_mode;     /* return: mode before this transition */
    uint8_t  new_mode;      /* return: mode after this transition */
    uint8_t  pad[2];
    uint32_t status;        /* return: SS$_ status */
};

#define VMS_IOCTL_ENTER_IMAGE    _IOWR(VMS_IOC_MAGIC, 0x66, struct vms_modexfer_args)
#define VMS_IOCTL_IMAGE_RUNDOWN  _IOWR(VMS_IOC_MAGIC, 0x67, struct vms_modexfer_args)

/*
 * ABI lock, same discipline as the P0/P1 blocks above: these ioctls fold
 * sizeof(struct vms_modexfer_args) into their request number, so a
 * struct-size drift silently renumbers the ioctl (-ENOTTY, not a
 * mis-decode) rather than failing to compile. 0x66/0x67 sit in the gap
 * between the P1 ioctl (0x65, just above) and the mailbox ioctls
 * (vms_mbx.h, 0x70-0x74) -- neither is touched by this addition.
 */
_Static_assert(sizeof(struct vms_modexfer_args) == 8,
               "vms_modexfer_args layout changed: VMS_IOCTL_ENTER_IMAGE/IMAGE_RUNDOWN ABI break");
_Static_assert(VMS_IOCTL_ENTER_IMAGE == 0xC0085666u,
               "VMS_IOCTL_ENTER_IMAGE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_IMAGE_RUNDOWN == 0xC0085667u,
               "VMS_IOCTL_IMAGE_RUNDOWN encodes differently here than on the reference build");

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

/* ================================================================
 * INET pseudo-device (executive-resident BGn:, vms-527).
 *
 * The ioctl structures and request numbers live in vms_bg.h, included
 * here for the same reason as vms_mbx.h above: one frozen definition for
 * every consumer, kernel and userspace, on either architecture. BGn: is
 * the TCP/IP transport device (the SRI-QIO / INETDRIVER interface),
 * distinct from the NIC device ETH0: it layers over.
 * ================================================================ */
#include "vms_bg.h"

/* ================================================================
 * Files-11 (ODS-2) ACP -- channel + mount ioctls (vms-149, epic vms-208).
 *
 * The executive-global mounted-volume table, the file-class channel and the
 * ACP ioctl band (0x68-0x6F) live in vms_acp.h, included here for the same
 * reason as vms_lnm.h / vms_mbx.h / vms_bg.h above: one frozen definition for
 * every consumer, kernel and userspace, on either architecture.
 * ================================================================ */
#include "vms_acp.h"

#endif /* _VMS_IOCTL_H */
