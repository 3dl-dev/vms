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
#define VMS_PRV_V_PHY_IO    22  /* may do physical I/O (vms-7eb; gates
                                 * VMS_IOCTL_L2_OPEN). Public $PRVDEF
                                 * documentation bit position (VSI OpenVMS
                                 * System Services Reference Manual / Guide to
                                 * System Security) -- oracle-observed too:
                                 * docs/oracle/vax73-privileges.md's SHOW
                                 * PROCESS/PRIVILEGES transcript names PHY_IO
                                 * "may do physical i/o" at this position.
                                 * src/libvms/include/prvdef.h already carried
                                 * PRV$V_PHY_IO=22 from before this item; the
                                 * _Static_assert in prv_agreement.c is what
                                 * makes this copy agree with that one, same
                                 * discipline as PRMMBX/MOUNT above. */
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
#define VMS_PRV_M_PHY_IO    (1ULL << VMS_PRV_V_PHY_IO)
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
 *   PHY_IO  vms_ioctl_l2_open (src/kernel-core/vms_l2.c) -- opening a
 *           kernel-owned raw AF_PACKET/L2 socket for the SCS cluster wire
 *           (vms-7eb, auth slice of vms-1e4). This IS the real VMS
 *           physical-I/O privilege, standing in for the Linux CAP_NET_RAW a
 *           userspace raw socket would otherwise need -- the kernel owns the
 *           socket, so the gate is the VMS privilege check, not a Linux
 *           capability.
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
                             VMS_PRV_M_MOUNT  | VMS_PRV_M_PHY_IO)

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
 * DIRECTORY (dir_csid), which node MASTERS it (master_csid, 0 until mastered
 * on first $ENQ), and how many locks are granted on it. It does NOT create or
 * master a resource -- an unknown name comes back found=0 with master_csid=0 --
 * so it can be called before and after an $ENQ to prove the local-master path
 * actually mastered the resource, rather than a test asserting a hand-set
 * structure.
 *
 * dir_csid IS 0 WHEN THERE IS NO ANSWER (FC-P4.3). It used to be reported for
 * every name, because the executive COMPUTED it (an OVMX hash over a static
 * member vector). It is now the entry the cluster's own Lock Directory Weight
 * Vector holds at the index of the cluster's own wire-carried hash for that
 * name (Davis pp. 6-31/6-50, src/kernel-core/vms_dlm_ldwv.h), which the
 * executive can only report when it genuinely holds both. 0 means "not
 * resolved" -- no cluster, no wire-learned hash, or a vector under rebuild --
 * exactly as master_csid 0 means "unmastered". Reporting a computed value here
 * would be a readback asserting state the executive does not hold (INV-6).
 *
 * is_local_master is (master_csid == local_csid), surfaced so a test need not
 * know the CSID value.
 */
struct vms_resmaster_args {
    char     resnam[32];        /* in: resource name (null-terminated) */
    uint32_t found;             /* return: 1 if a resource block exists */
    uint32_t local_csid;        /* return: this node's CSID */
    uint32_t dir_csid;          /* return: the directory node for resnam, or 0
                                 * when the executive cannot resolve one (see
                                 * the note above) -- never a computed value */
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
 * over SCS from a REMOTE node (decoded by the cat-02 codec) is marshalled
 * into the executive through this ioctl so it reaches the kernel lock manager's
 * cross-node handler (vms_lock_dlm_xnode_dispatch). Rung 1 is the TRANSPORT
 * only: the message reaches the handler decoded and the handler returns
 * SS$_UNSUPPORTED -- it does NOT grant, queue, dequeue, or deliver a blocking
 * AST (that is rung 2). INV-6: no fabricated cross-node grant; a cross-node op
 * honestly fails, exactly as dlm_resolve_master() already does for the SEND side.
 *
 * `op` carries a DLM message kind as a plain byte so this header takes no
 * dependency on the cluster codec; the VMS_DLM_OP_* values below MUST match
 * scs_dlm.h's SCS_DLM_OP_* (scsd.c static-asserts they do).
 */
#define VMS_DLM_OP_ENQ      1u   /* lock/convert request  -> master  */
#define VMS_DLM_OP_GRANT    2u   /* status response       <- master  */
#define VMS_DLM_OP_DEQ      3u   /* dequeue request       -> master  */
#define VMS_DLM_OP_BLKAST   4u   /* blocking-AST notify   <- master  */
#define VMS_DLM_OP_REBUILD  5u   /* remaster lock-rebuild -> new master (H10b,
                                  * vms-dca9): a surviving holder re-registers its
                                  * cross-node lock on the new master after the old
                                  * master departs. OVMX design choice (Rule 8);
                                  * MUST match SCS_DLM_OP_REBUILD in scs_dlm.h. */
#define VMS_DLM_OP_DLKSRCH  6u   /* distributed deadlock search (H11, vms-ec75): the
                                  * VICTIM leg is dispatched here -- it aborts a queued
                                  * cross-node waiter named (req_csid,req_lkid) and
                                  * completes it with SS$_DEADLOCK. OVMX design choice
                                  * (Rule 8); MUST match SCS_DLM_OP_DLKSRCH in
                                  * scs_dlm.h. The SEARCH legs are orchestrated in scsd
                                  * over the two readback ioctls (GET_GRANTED +
                                  * DLM_ENUM_WAITS) and never enter the executive. */

/* DLKSRCH phase, carried in vms_dlm_xnode_args.flags (mirror of SCS_DLM_DLK_* in
 * scs_dlm.h). Only VICTIM ever reaches the executive dispatch. */
#define VMS_DLM_DLK_SEARCH_HOLDER   0u
#define VMS_DLM_DLK_SEARCH_RESOURCE 1u
#define VMS_DLM_DLK_VICTIM          2u

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
    /*
     * BLKAST WIRE (DLM epic vms-7fa rung H6, vms-76d). The holder-side blocking-AST
     * delivery, the symmetric mirror of the requester-side GRANT RECEIVE (H5):
     *   blocking_req_lkid  - return(ENQ): the blocking cross-node HOLDER's OWN
     *                        (requester-side) lock handle -- the value a BLKAST
     *                        must carry in req_lkid so the holder node finds its
     *                        ORIGIN record. 0 when nothing blocks across nodes.
     *   blkastadr/blkastprm - in(GRANT receive): when a GRANT establishes/updates a
     *                        holder-side ORIGIN record, the holder's blocking-AST
     *                        routine + parameter, remembered on the record so a
     *                        later BLKAST can fire a REAL user-mode AST. 0 = none.
     *   blkast_delivered   - return(BLKAST receive): 1 iff a genuine user-mode
     *                        blocking AST was queued to the holder's process
     *                        (INV-6: 0/SS$_UNSUPPORTED when there is no such
     *                        holder record or no blkastadr -- never a faked AST).
     */
    uint32_t blocking_req_lkid;
    uint64_t blkastadr;
    uint64_t blkastprm;
    uint32_t blkast_delivered;
    uint32_t pad_blkast;
};
_Static_assert(sizeof(struct vms_dlm_xnode_args) == 120,
               "vms_dlm_xnode_args changed size -- VMS_IOCTL_DLM_XNODE ABI break");
#define VMS_IOCTL_DLM_XNODE _IOWR(VMS_IOC_MAGIC, 0x35, struct vms_dlm_xnode_args)

/*
 * $DLM member departure. The connection manager calls this when it observes a
 * GRACEFUL cluster departure.
 *
 * WHAT IT DOES (revised by FC-P4.3). The membership this node's directory
 * resolves over is the connection manager's CLUB, and a departure is handled by
 * the transition that removes the member: Phase 1 discards the Lock Directory
 * Weight Vector, Phase 2 refills it, and every cached directory answer in the
 * lock engine is re-resolved because the vector's generation changed (Davis
 * p. 6-33; src/kernel-core/vms_dlm_ldwv.h). The static dlm_member_csids insmod
 * vector this ioctl used to filter is GONE with the exec_jhash directory it fed.
 *
 * What remains here is the LOCK STATE the departure orphans: a resource
 * MASTERED on the departed node loses its master (it re-masters on first use),
 * cached directory answers are dropped eagerly, and a proxy LKB still pending
 * at the departed master is ended honestly rather than left asleep forever.
 * Reconstructing the locks themselves is the DLM rebuild, not this.
 *
 * INV-6: this reflects a REAL departure the connection manager observed, never
 * a fabricated membership change; with no /dev/vms there is no executive to
 * tell (honest SS$_NOSUCHDEV).
 */
struct vms_dlm_depart_args {
    uint32_t departed_csid;   /* in: the CSID that left the cluster */
    uint32_t members_live;    /* return: ALWAYS 0 -- the live member count is the
                               * connection manager's fact (CLUSTER_DIAG_CSB),
                               * not the lock engine's; answering with a count
                               * this facility no longer holds would be the
                               * mirrored-membership fabrication FC-P3.9 deleted */
    uint32_t found;           /* return: 1 iff the departed CSID actually mastered
                               * a resource on this node -- a real observation,
                               * not a lookup in a configured list */
    uint32_t status;          /* return: SS$_ status */
};
_Static_assert(sizeof(struct vms_dlm_depart_args) == 16,
               "vms_dlm_depart_args changed size -- VMS_IOCTL_DLM_MEMBER_DEPART ABI break");
#define VMS_IOCTL_DLM_MEMBER_DEPART _IOWR(VMS_IOC_MAGIC, 0x36, struct vms_dlm_depart_args)
_Static_assert(VMS_IOCTL_DLM_MEMBER_DEPART == 0xC0105636u,
               "VMS_IOCTL_DLM_MEMBER_DEPART encodes differently than the reference build");

/*
 * $DLM granted-lock readback (rd vms-dca9, DLM rung H10b). Reports the FIRST
 * remote-held granted lock on a resource so a test can VALUE-VERIFY a rebuilt
 * cross-node lock -- the holder's CSID, its OWN lock handle (req_lkid) and the
 * granted mode -- not just that n_granted rose. GET_RESMASTER exposes the count
 * + remote_holder_csid but neither the mode nor the holder's handle; this ioctl
 * closes that gap so the H10b harness can assert the rebuilt lock EQUALS the one
 * the holder really held pre-departure (h8/h9-style value equality). INV-6: real
 * res->granted state; found=0 (all fields 0) when no such lock exists, never a
 * plausible default.
 */
struct vms_dlm_granted_args {
    char     resnam[32];        /* in: resource name (null-terminated) */
    uint32_t found;             /* return: 1 iff a REMOTE-held granted lock exists */
    uint32_t n_granted;         /* return: total granted locks on the resource */
    uint32_t holder_csid;       /* return: the first remote holder's CSID (req_csid) */
    uint32_t holder_req_lkid;   /* return: that holder's OWN lock handle (req_lkid) */
    uint32_t granted_mode;      /* return: that lock's granted mode (LCK_K_*) */
    uint32_t status;            /* return: SS$_ status */
};
_Static_assert(sizeof(struct vms_dlm_granted_args) == 56,
               "vms_dlm_granted_args changed size -- VMS_IOCTL_DLM_GET_GRANTED ABI break");
#define VMS_IOCTL_DLM_GET_GRANTED _IOWR(VMS_IOC_MAGIC, 0x37, struct vms_dlm_granted_args)
_Static_assert(VMS_IOCTL_DLM_GET_GRANTED == 0xC0385637u,
               "VMS_IOCTL_DLM_GET_GRANTED encodes differently than the reference build");

/*
 * $DLM pending-wait enumeration (rd vms-ec75, DLM rung H11) -- the HOME authority
 * for distributed deadlock search. Enumerates THIS node's outstanding cross-node
 * requests that are still PENDING (granted_mode == NL): each is a requester-side
 * PROXY LKB (FC-P4.4; formerly a vms_dlm_origin record) that a master's reply
 * completed. For the edge-chase, "what is CSID H waiting for?" is answered by
 * asking H's home node to run this: every returned entry names a resource H waits
 * on (resnam), the node mastering it (master_csid), and H's own requester-side
 * handle for the wait (req_lkid) -- exactly one outgoing wait-for edge. INV-6: a
 * READ of real proxy-LKB state; count=0 when nothing is pending, never a
 * fabricated edge. This surfaces EXISTING executive state, so no wait-for graph
 * is stored or guessed.
 */
#define VMS_DLM_ENUM_WAITS_MAX 8u   /* entries returned per call (a chase visits few) */
struct vms_dlm_wait_ent {
    char     resnam[32];        /* resource this node waits on (its proxy's RSB name) */
    uint32_t master_csid;       /* the node mastering that resource                  */
    uint32_t req_lkid;          /* this node's requester-side handle for the wait    */
    uint32_t req_csid;          /* this node's own CSID (the waiter)                 */
    uint32_t granted_mode;      /* LCK_K_NLMODE while pending (always NL here)        */
};
struct vms_dlm_enum_waits_args {
    uint32_t count;             /* return: pending waits filled (<= VMS_DLM_ENUM_WAITS_MAX) */
    uint32_t total;             /* return: total pending origins (may exceed count)  */
    uint32_t status;            /* return: SS$_ status                               */
    uint32_t pad;               /* zero                                              */
    struct vms_dlm_wait_ent ent[VMS_DLM_ENUM_WAITS_MAX];
};
_Static_assert(sizeof(struct vms_dlm_enum_waits_args) == 16 + 48 * 8,
               "vms_dlm_enum_waits_args changed size -- VMS_IOCTL_DLM_ENUM_WAITS ABI break");
#define VMS_IOCTL_DLM_ENUM_WAITS _IOWR(VMS_IOC_MAGIC, 0x38, struct vms_dlm_enum_waits_args)
_Static_assert(VMS_IOCTL_DLM_ENUM_WAITS == 0xC1905638u,
               "VMS_IOCTL_DLM_ENUM_WAITS encodes differently than the reference build");

/*
 * Enumerate this node's STANDING cluster-registrable system locks (vms-1f4, the
 * enumeration seam of faithful cluster DLM registration vms-3eb). These are the
 * locks the executive holds for the node's LIFE -- today the per-volume
 * "F11B$v<label>" lock a faithful MOUNT holds from $MOUNT to $DISMOUNT (vms-25e),
 * later the mount (MOU$) and clusterwide-logical (LNM$CWLOGICALS) locks -- that
 * the connection manager (scsd) must register to the coordinator during a
 * directory rebuild. Each entry gives the resource name and this node's LOCAL
 * lock handle, which becomes the op-0x01 requester lkid on the wire.
 *
 * INV-6: a READ of REAL lock-manager state -- one entry per lock the executive
 * genuinely holds (a nonzero vol_lkid on a mounted volume). count=0 when the node
 * holds no standing locks; never a fabricated lock. This is the honest boundary
 * scsd registers FROM: it can only announce to the cluster what the executive
 * actually holds.
 */
#define VMS_DLM_ENUM_STANDING_MAX 16u   /* standing system locks returned per call */
struct vms_dlm_standing_ent {
    char     resnam[32];        /* the standing lock's resource name (e.g. "F11B$vOVMXSYS") */
    uint32_t lkid;              /* this node's LOCAL handle for it (the op-0x01 req_lkid)   */
    uint32_t mode;              /* granted mode (LCK_K_NLMODE for the volume presence lock) */
};
struct vms_dlm_enum_standing_args {
    uint32_t count;             /* return: standing locks filled (<= VMS_DLM_ENUM_STANDING_MAX) */
    uint32_t total;             /* return: total standing locks the node holds (may exceed count) */
    uint32_t status;            /* return: SS$_ status */
    uint32_t pad;               /* zero */
    struct vms_dlm_standing_ent ent[VMS_DLM_ENUM_STANDING_MAX];
};
_Static_assert(sizeof(struct vms_dlm_enum_standing_args) == 16 + 40 * 16,
               "vms_dlm_enum_standing_args changed size -- VMS_IOCTL_DLM_ENUM_STANDING ABI break");
#define VMS_IOCTL_DLM_ENUM_STANDING _IOWR(VMS_IOC_MAGIC, 0x3c, struct vms_dlm_enum_standing_args)
_Static_assert(VMS_IOCTL_DLM_ENUM_STANDING == 0xC290563Cu,
               "VMS_IOCTL_DLM_ENUM_STANDING encodes differently than the reference build");

/*
 * Cluster membership is the CONNECTION MANAGER's, and the connection manager
 * is executive-resident (FC-P3.8/FC-P3.9, docs/design-faithful-cluster-
 * executive.md SS3.4). One row per system the CLUB knows about.
 *
 * WHAT THIS USED TO BE. rd vms-551 gave vms.ko a module-global
 * `vms_cluster_members[]` block that a USERSPACE daemon (scsd) POPULATED
 * through VMS_IOCTL_CLUSTER_MEMBER_SET/_CLEAR and SHOW CLUSTER read back --
 * an executive-shaped mirror of a userspace fact, which is the shape the
 * operator's 2026-09-02 reset ruling retired. FC-P3.9 DELETED both mutators,
 * the block and the daemon: the only remaining member ioctl is this READ, and
 * it projects the connection manager's own CLUB and CSB table
 * (src/kernel-core/vms_cnxman.c). Nothing outside the executive can write
 * membership any more, because nothing can: there is no setter.
 *
 * HONEST ROWS, NOT A MEMBER COUNT. A CSB exists from the moment this node
 * OBSERVES another connection manager (p. 7-23, state NEW) -- long before the
 * cluster has assigned anyone a CSID. So a row here means "a system this
 * node's CLUB holds a block for", and `state` carries WHICH of the ten CSB
 * connectivity states it is in. A NEW row is not a member and never claims to
 * be; `csid` is 0 until the cluster assigns one (integration note E30), which
 * is the honest "not yet learned", never "node zero".
 *
 * NOTMEMBER != NOSUCHDEV, and this struct is what keeps them distinct:
 * the executive reachable with n_members==0 is SS$_NORMAL (a genuine
 * NOTMEMBER view); the executive UNREACHABLE (no /dev/vms) is the transport
 * failure SS$_NOSUCHDEV a caller sees from KIF_CALL itself, never from these
 * args. Never conflate the two (vms-8d4 precedent).
 */
struct vms_cluster_member {
    uint32_t csid;               /* cluster system id, 0 = not yet assigned */
    uint32_t sysid;              /* SCSSYSTEMID */
    char     scsnode[16];        /* SCSNODE name, NUL-padded ("" until learned) */
    char     state[16];          /* the CSB connectivity state: "NEW", "MEMBER", ... */
};
#define VMS_CLUSTER_MEMBER_MAX 96u   /* VMScluster tops out at 96 nodes */

/*
 * VMS_IOCTL_CLUSTER_MEMBER_GET (rd vms-551; re-pointed at the CLUB/CSB table
 * by FC-P3.9, integration note E35). SHOW CLUSTER's read: copies out the live
 * view (up to VMS_CLUSTER_MEMBER_MAX rows) + the live count.
 * n_members==0 is a valid SS$_NORMAL (NOTMEMBER), never an error -- see the
 * NOTMEMBER != NOSUCHDEV note above. 3848 bytes total, under NetBSD's
 * one-page IOCPARM_MAX (4096), so this rides the same pre-copy _IOWR path as
 * the other DLM ioctls above (no IOC_VOID big-io shape needed).
 */
struct vms_cluster_member_get_args {
    uint32_t n_members;           /* return: live member count (0 == NOTMEMBER) */
    uint32_t status;               /* return: SS$_ status (always SS$_NORMAL)   */
    struct vms_cluster_member members[VMS_CLUSTER_MEMBER_MAX];  /* return: the view */
};
_Static_assert(sizeof(struct vms_cluster_member_get_args) == 3848,
               "vms_cluster_member_get_args changed size -- VMS_IOCTL_CLUSTER_MEMBER_GET ABI break");
#define VMS_IOCTL_CLUSTER_MEMBER_GET _IOWR(VMS_IOC_MAGIC, 0x3b, struct vms_cluster_member_get_args)
_Static_assert(VMS_IOCTL_CLUSTER_MEMBER_GET == 0xCF08563Bu,
               "VMS_IOCTL_CLUSTER_MEMBER_GET encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_DIAG_PORT (FC-P0.9, rd docs/plan-faithful-cluster-
 * executive.md). The port's (PEA0:, PEDRIVER role) diagnostics view --
 * SDA's SHOW PORT / SCACP SHOW CHANNEL equivalent. `row` names WHICH of the
 * port's three real projections (vms_pe_snapshot / _channel_snapshot /
 * _vc_snapshot, src/kernel-core/vms_pe.c) the call fills; `index` walks
 * channels/circuits for the CHANNEL/VC rows and is ignored for the PORT row.
 * Only the ONE struct `row` names is meaningful on a given call -- named,
 * never guessed by a caller scanning every field for a nonzero one.
 *
 * status carries the honest SS$_NOSUCHDEV before the port has ever come up
 * (no CLUSTER_START yet, VAXCLUSTER=0, or a channel/VC index past the
 * table's high-water mark) -- the negctl this ioctl's plan row requires.
 *
 * The three row structs mirror src/kernel-core/vms_cluster_snapshot.h's
 * vms_pe_view / vms_pe_channel_view / vms_pe_vc_view byte-for-byte (same
 * "ONE facility source, duplicated struct declaration" shape
 * VMS_IOCTL_CLUSTER_MEMBER_GET above already uses for vms_cluster_member):
 * this header must stay includable with no kernel-core dependency, so the
 * layout is copied here rather than shared by #include, and the
 * _Static_asserts below pin the two copies to the same size.
 */
#define VMS_CLUSTER_DIAG_PORT_ROW      0u  /* the port-wide view */
#define VMS_CLUSTER_DIAG_PORT_CHANNEL  1u  /* one channel row, by `index` */
#define VMS_CLUSTER_DIAG_PORT_VC       2u  /* one virtual-circuit row, by `index` */

struct vms_pe_view_wire {
    uint8_t  port_open;
    uint8_t  hwaddr_valid;
    uint8_t  hwaddr[6];
    uint8_t  link_up;
    uint8_t  pad0[3];
    uint32_t mtu;
    uint32_t max_pktsz;
    uint32_t n_channels;
    uint32_t n_vcs;
    uint32_t rx_frames;
    uint32_t rx_drops_nobuf;
    uint32_t rx_drops_badclass;
    uint32_t tx_frames;
    uint32_t tx_errors;
};
_Static_assert(sizeof(struct vms_pe_view_wire) == 48,
               "vms_pe_view_wire changed size -- must match vms_pe_view");

struct vms_pe_channel_view_wire {
    uint8_t  remote_mac[6];
    uint8_t  state;
    uint8_t  remote_sysid_valid;
    uint32_t remote_sysid_lo;
    uint32_t remote_sysid_hi;
    uint32_t last_rx_ms;
    uint32_t hello_tx;
    uint32_t hello_rx;
    uint32_t verified_pktsz;
};
_Static_assert(sizeof(struct vms_pe_channel_view_wire) == 32,
               "vms_pe_channel_view_wire changed size -- must match vms_pe_channel_view");

struct vms_pe_vc_view_wire {
    uint32_t peer_sysid_lo;
    uint32_t peer_sysid_hi;
    uint8_t  state;
    uint8_t  pad0[3];
    uint32_t send_seq;
    uint32_t recv_seq;
    uint32_t recv_ack;
    uint32_t peer_recv_ack;
    uint32_t unacked;
    uint32_t retransmits;
    uint32_t incarnation_lo;
    uint32_t incarnation_hi;
    uint32_t timvcfail_ms_left;
    uint32_t credits_send;
    uint32_t credits_receive;
    /* FC-P1.6: appended, never inserted -- see vms_cluster_snapshot.h's
     * vms_pe_vc_view for the field-by-field rationale (both real pe_vc
     * counters, INV-6). */
    uint32_t rx_gaps;
    uint8_t  down_reason;
    uint8_t  pad1[3];
};
_Static_assert(sizeof(struct vms_pe_vc_view_wire) == 64,
               "vms_pe_vc_view_wire changed size -- must match vms_pe_vc_view");

struct vms_cluster_diag_port_args {
    uint32_t row;                       /* in: VMS_CLUSTER_DIAG_PORT_*        */
    uint32_t index;                     /* in: channel/vc index; else ignored */
    uint32_t status;                    /* return: SS$_ status                */
    uint32_t pad0;
    struct vms_pe_view_wire         port;      /* valid iff row == _ROW     */
    struct vms_pe_channel_view_wire channel;   /* valid iff row == _CHANNEL */
    struct vms_pe_vc_view_wire      vc;        /* valid iff row == _VC      */
};
_Static_assert(sizeof(struct vms_cluster_diag_port_args) == 160,
               "vms_cluster_diag_port_args changed size -- VMS_IOCTL_CLUSTER_DIAG_PORT ABI break");
#define VMS_IOCTL_CLUSTER_DIAG_PORT _IOWR(VMS_IOC_MAGIC, 0x3d, struct vms_cluster_diag_port_args)
_Static_assert(VMS_IOCTL_CLUSTER_DIAG_PORT == 0xC0A0563Du,
               "VMS_IOCTL_CLUSTER_DIAG_PORT encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_DIAG_CONN (FC-P2.4, docs/plan-faithful-cluster-
 * executive.md). SCS's diagnostics view -- SDA's SHOW CONNECTIONS equivalent.
 * `row` names WHICH of the two real projections (vms_scs_snapshot /
 * vms_scs_cdt_snapshot, src/kernel-core/vms_scs.c) the call fills; `index`
 * walks the CDL for the CDT row and is ignored for the SCS row. Exactly the
 * shape VMS_IOCTL_CLUSTER_DIAG_PORT above already uses, for the same reasons.
 *
 * THE CDT ROW IS THE SDA `SHOW CONNECTIONS` DECODER RING (wire spec
 * docs/cluster-protocol-spec.md SS3, `sda-scs-extract-vax1.txt`), column for
 * column, so a lab comparison against a real VAX is a field-by-field match and
 * not an interpretation:
 *
 *     Local SYSAP        <- local_name  (16 bytes, blank-padded as the wire
 *                                        carries it)
 *     Remote             <- peer_sysid_lo/_hi + remote_name
 *     Local Con. ID      <- local_conid
 *     Remote Con. ID     <- remote_conid, and ONLY when remote_conid_valid;
 *                           the reference extract's own `Remote Con. ID
 *                           00000000` for a connection that never bound one is
 *                           reproduced by the flag being CLEAR, not by
 *                           printing a zero that could be mistaken for a
 *                           handle (INV-6, rule 2 of vms_cluster_snapshot.h)
 *     Credit (Send/Recv) <- credit_send / credit_receive (+ credit_pending,
 *                           the ledger's third counter, which SDA does not
 *                           print but the executive really holds)
 *     State              <- `state`, an enum vms_scs_cdt_state ordinal whose
 *                           NAME comes from scs_cdt_state_name()
 *     MTYPE              <- msgtype, spec SS4(m)'s abs-30 phase byte
 *
 * Every one of those is a projection of a live struct scs_cdt taken under the
 * fork mutex; nothing here is a frame count standing in for state, and a CDT
 * the executive does not hold is SS$_NOSUCHDEV with an all-zero row.
 *
 * The two row structs mirror src/kernel-core/vms_cluster_snapshot.h's
 * vms_scs_view / vms_scs_cdt_view byte-for-byte -- same "ONE facility source,
 * duplicated struct declaration" shape as the CLUSTER_DIAG_PORT rows, because
 * this header must stay includable with no kernel-core dependency.
 */
#define VMS_CLUSTER_DIAG_CONN_ROW 0u  /* the SCS-wide view */
#define VMS_CLUSTER_DIAG_CONN_CDT 1u  /* one CDT row, by `index` */

struct vms_scs_view_wire {
    uint32_t n_sbs;
    uint32_t n_cdts;
    uint32_t n_sysaps;
    uint32_t conid_seq;
    uint32_t conid_epoch;
    uint32_t dir_lookups_served;
    uint32_t dir_lookups_sent;
    uint32_t credit_stalls;
};
_Static_assert(sizeof(struct vms_scs_view_wire) == 32,
               "vms_scs_view_wire changed size -- must match vms_scs_view");

struct vms_scs_cdt_view_wire {
    uint32_t local_conid;
    uint32_t remote_conid;
    uint8_t  remote_conid_valid;
    uint8_t  state;
    uint8_t  pad0[2];
    uint32_t peer_sysid_lo;
    uint32_t peer_sysid_hi;
    uint8_t  local_name[16];    /* VMS_SCS_PROCNAME_LEN */
    uint8_t  remote_name[16];
    uint16_t credit_send;
    uint16_t credit_receive;
    uint16_t credit_pending;
    uint16_t pad1;
    uint32_t msgs_sent;
    uint32_t msgs_received;
    uint8_t  msgtype;
    uint8_t  pad2[3];
};
_Static_assert(sizeof(struct vms_scs_cdt_view_wire) == 72,
               "vms_scs_cdt_view_wire changed size -- must match vms_scs_cdt_view");

struct vms_cluster_diag_conn_args {
    uint32_t row;                       /* in: VMS_CLUSTER_DIAG_CONN_*        */
    uint32_t index;                     /* in: CDL index; ignored for _ROW    */
    uint32_t status;                    /* return: SS$_ status                */
    uint32_t pad0;
    struct vms_scs_view_wire     scs;   /* valid iff row == _ROW              */
    struct vms_scs_cdt_view_wire cdt;   /* valid iff row == _CDT              */
};
_Static_assert(sizeof(struct vms_cluster_diag_conn_args) == 120,
               "vms_cluster_diag_conn_args changed size -- VMS_IOCTL_CLUSTER_DIAG_CONN ABI break");
/*
 * NR 0x69, not 0x40: the cluster block 0x39-0x3f is FULL (MEMBER_SET through
 * CLUSTER_START), and 0x40-0x4d belong to the register/process groups. 0x69 is
 * the next unused number in this magic. The encoded value below was RECOMPUTED
 * for this struct's size, the way FC-P1.6 had to recompute
 * VMS_IOCTL_CLUSTER_DIAG_PORT's when its VC row grew: _IOWR folds sizeof(type)
 * into the command word, so an appended field silently changes the ioctl
 * NUMBER, and this assert is what turns that into a build failure instead of an
 * ENOTTY on a booted node.
 */
#define VMS_IOCTL_CLUSTER_DIAG_CONN _IOWR(VMS_IOC_MAGIC, 0x69, struct vms_cluster_diag_conn_args)
_Static_assert(VMS_IOCTL_CLUSTER_DIAG_CONN == 0xC0785669u,
               "VMS_IOCTL_CLUSTER_DIAG_CONN encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_DIAG_CSB (FC-P3.8, docs/plan-faithful-cluster-
 * executive.md). The connection manager's own SDA equivalent (SHOW CLUSTER's
 * underlying CLUB/CSB table): `row` names WHICH of the two real projections
 * (cnxman_get_club / cnxman_get_csb, src/kernel-core/vms_cnxman.c) the call
 * fills; `index` walks the CSB table for the CSB row and is ignored for the
 * CLUB row. Same "row selects one of two real snapshots" shape as
 * VMS_IOCTL_CLUSTER_DIAG_PORT/_CONN above, for the same reasons.
 *
 * Both rows are PURE PROJECTIONS of a live struct vms_club / struct vms_csb,
 * taken under the fork mutex (cnxman_club_project / cnxman_csb_project,
 * src/kernel-core/vms_cnxman_csb.c); a CSB the executive does not hold is
 * SS$_NOSUCHDEV with an all-zero row, never a placeholder member (INV-6).
 * `csb.csid_valid` clear is the honest "not yet learned" the connection
 * manager reads as NEW, not a fabricated CSID 0 (integration note E30: this
 * is the routine case until the op-06 layout is lab-pinned).
 *
 * The two row structs mirror src/kernel-core/vms_cluster_snapshot.h's
 * vms_club_view / vms_csb_view byte-for-byte -- same "ONE facility source,
 * duplicated struct declaration" shape as the sibling DIAG ioctls, because
 * this header must stay includable with no kernel-core dependency.
 */
#define VMS_CLUSTER_DIAG_CSB_CLUB 0u  /* the CLUB-wide view */
#define VMS_CLUSTER_DIAG_CSB_CSB  1u  /* one CSB row, by `index` */

struct vms_club_view_wire {
    uint32_t local_csid;
    uint8_t  local_csid_valid;
    uint8_t  state;
    uint8_t  quorum_lost;
    uint8_t  pad0;
    uint32_t epoch;
    uint32_t cluster_nodes;
    uint16_t cevotes;
    uint16_t quorum;
    uint16_t expected_votes;
    uint16_t pad1;
    uint32_t bitmap[4];         /* VMS_CLUB_BITMAP_WORDS */
    uint32_t bitmap_slots_seen;
    uint8_t  transition_active;
    uint8_t  transition_class;
    uint8_t  barrier_step;
    uint8_t  coordinator_valid;
    uint32_t coordinator_csid;
    uint32_t outstanding_rebuild;
    uint32_t ftime_lo;
    uint32_t ftime_hi;
    uint32_t fsysid_lo;
    uint32_t fsysid_hi;
    uint32_t reformations;
};
_Static_assert(sizeof(struct vms_club_view_wire) == 76,
               "vms_club_view_wire changed size -- must match vms_club_view");

struct vms_csb_view_wire {
    uint32_t csid;
    uint8_t  csid_valid;
    uint8_t  state;
    uint8_t  is_member;
    uint8_t  is_selected;
    uint8_t  status_rcvd;
    uint8_t  scsnode_len;
    uint8_t  scsnode[8];        /* VMS_SCSNODE_MAX + 2 */
    uint16_t votes;
    uint8_t  votes_valid;
    uint8_t  lockdirwt;
    uint8_t  lockdirwt_valid;
    uint8_t  pad0;
    uint32_t peer_sysid_lo;
    uint32_t peer_sysid_hi;
    uint32_t sw_version;
    uint32_t cdt_conid;
    uint32_t incarnation_lo;
    uint32_t incarnation_hi;
    uint32_t last_status_ms;
};
_Static_assert(sizeof(struct vms_csb_view_wire) == 52,
               "vms_csb_view_wire changed size -- must match vms_csb_view");

struct vms_cluster_diag_csb_args {
    uint32_t row;                       /* in: VMS_CLUSTER_DIAG_CSB_*         */
    uint32_t index;                     /* in: CSB index; ignored for _CLUB   */
    uint32_t status;                    /* return: SS$_ status                */
    uint32_t pad0;
    struct vms_club_view_wire club;     /* valid iff row == _CLUB             */
    struct vms_csb_view_wire  csb;      /* valid iff row == _CSB              */
};
_Static_assert(sizeof(struct vms_cluster_diag_csb_args) == 144,
               "vms_cluster_diag_csb_args changed size -- VMS_IOCTL_CLUSTER_DIAG_CSB ABI break");
#define VMS_IOCTL_CLUSTER_DIAG_CSB _IOWR(VMS_IOC_MAGIC, 0x6a, struct vms_cluster_diag_csb_args)
_Static_assert(VMS_IOCTL_CLUSTER_DIAG_CSB == 0xC090566Au,
               "VMS_IOCTL_CLUSTER_DIAG_CSB encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_SETCLUEVT (FC-P3.8, docs/plan-faithful-cluster-
 * executive.md). $SETCLUEVT's executive-side registration: this process asks
 * to be told, by completion AST, when CNXMAN detects a real membership ADD or
 * REMOVE (cluevtdef.h CLUEVT$C_ADD/_REMOVE, OR'd into `event_mask`).
 * `event_mask == 0` (or `astadr == 0`) deregisters -- the SAME call, never a
 * second ioctl, mirroring $SETCLUEVT's own re-arm-by-recall shape. One
 * registration per node (src/kernel-core/vms_cnxman.c: `struct vms_cnxman`
 * keeps a single slot, last caller wins); delivery is a real completion AST
 * queued on the caller's own PSL_C_USER queue (vms_ioctl_deliverast drains
 * it), and process death clears the slot (vms_cnxman_proc_gone(), called from
 * proc teardown) so a delivery can never reach freed memory.
 */
struct vms_cluster_setcluevt_args {
    uint32_t event_mask;    /* in: CLUEVT$C_ADD | CLUEVT$C_REMOVE, 0 = clear */
    uint32_t status;        /* return: SS$_ status                          */
    uint64_t astadr;        /* in: AST routine address, 0 = clear           */
    uint64_t astprm;        /* in: AST parameter                            */
};
_Static_assert(sizeof(struct vms_cluster_setcluevt_args) == 24,
               "vms_cluster_setcluevt_args changed size -- VMS_IOCTL_CLUSTER_SETCLUEVT ABI break");
#define VMS_IOCTL_CLUSTER_SETCLUEVT _IOWR(VMS_IOC_MAGIC, 0x6b, struct vms_cluster_setcluevt_args)
_Static_assert(VMS_IOCTL_CLUSTER_SETCLUEVT == 0xC018566Bu,
               "VMS_IOCTL_CLUSTER_SETCLUEVT encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_GETSYI (FC-P3.9, docs/plan-faithful-cluster-executive.md
 * SS3.5's "VMS_IOCTL_CLUSTER_GET_CLUB -> $GETSYI CLUSTER_MEMBER/CLUSTER_NODES/
 * CLUSTER_VOTES/CLUSTER_QUORUM/CLUSTER_FSYSID/CLUSTER_FTIME/NODE_CSID").
 * $GETSYI's cluster item codes, projected from the CONNECTION MANAGER's own
 * CLUB by cluster_api_getsyi_project() (FC-P3.7, src/kernel-core/
 * vms_cluster_api.c) -- the one function that owns this projection, so
 * F$GETSYI and SHOW CLUSTER cannot answer differently about the same node.
 *
 * This is BYTE-FOR-BYTE struct vms_getsyi_cluster_view (vms_cluster_api.h)
 * plus the `status` return, the same "one facility source, two struct
 * declarations" shape the CLUSTER_DIAG_* rows use; vms_devtab.c asserts the
 * two sizes agree and memcpys.
 *
 * INV-6: every `_valid` companion travels with its value. A caller that finds
 * `node_csid_valid == 0` must leave the F$GETSYI item UNRETRIEVED -- it must
 * never print `node_csid` (which is 0, meaning "the cluster has not assigned
 * this node a CSID yet", never "node zero"). SS$_NOSUCHDEV before
 * VMS_IOCTL_CLUSTER_START, with every field left zeroed.
 */
struct vms_cluster_getsyi_args {
    uint32_t status;                /* return: SS$_ status                    */
    uint32_t cluster_nodes;         /* return: SYI$_CLUSTER_NODES             */
    uint32_t cluster_fsysid_lo;     /* return: SYI$_CLUSTER_FSYSID low word   */
    uint32_t cluster_fsysid_hi;     /* return: ... high word                  */
    uint32_t cluster_ftime_lo;      /* return: SYI$_CLUSTER_FTIME low word    */
    uint32_t cluster_ftime_hi;      /* return: ... high word                  */
    uint32_t node_csid;             /* return: SYI$_NODE_CSID (0 = unassigned)*/
    uint16_t cluster_votes;         /* return: SYI$_CLUSTER_VOTES (ours)      */
    uint16_t cluster_quorum;        /* return: SYI$_CLUSTER_QUORUM            */
    uint8_t  cluster_member;        /* return: SYI$_CLUSTER_MEMBER, 0 or 1    */
    uint8_t  cluster_fsysid_valid;  /* return: 0 = the CLUB has not learned it*/
    uint8_t  cluster_ftime_valid;   /* return: 0 = the CLUB has not learned it*/
    uint8_t  node_csid_valid;       /* return: 0 = this node is still NEW     */
};
_Static_assert(sizeof(struct vms_cluster_getsyi_args) == 36,
               "vms_cluster_getsyi_args changed size -- VMS_IOCTL_CLUSTER_GETSYI ABI break");
#define VMS_IOCTL_CLUSTER_GETSYI _IOWR(VMS_IOC_MAGIC, 0x6c, struct vms_cluster_getsyi_args)
_Static_assert(VMS_IOCTL_CLUSTER_GETSYI == 0xC024566Cu,
               "VMS_IOCTL_CLUSTER_GETSYI encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_DIAG_JOIN (E69, docs/cluster-integration-notes.md). The
 * CONNECTION MANAGER's JOIN TRANSITION RING -- a read-only window into the
 * executive-resident transcript src/kernel-core/vms_cnxman_diag.c records, plus
 * the join FSM's own live state, projected together under the fork mutex by
 * cnxman_get_join_diag() (src/kernel-core/vms_cnxman.c).
 *
 * WHY IT EXISTS. The executive has no console log (src/ovmx_init/ovmx_init.c:
 * 1399 -- its output is not OPA0:), so a join that stalls at a point the wire
 * does not expose cannot be diagnosed at all: three consecutive promotion walls
 * (E67, E68, the E68 re-fire) were read off a pcap by luck, and the remaining
 * gap -- OVMX receives the members' PARAMS and never answers with its own
 * MODEL/PARAMS burst -- shows nothing on the wire by construction. This ioctl
 * is how the lab reads what the join FSM actually did.
 *
 * READ-ONLY AND DISPATCH-ALWAYS, exactly like its three CLUSTER_DIAG_*
 * siblings (E47): the handler takes `proc` only to match the ioctl signature
 * and never touches it, so a caller that has never registered a VMS process
 * still gets the executive's real answer, and a connection manager that is not
 * up is the honest SS$_NOSUCHDEV with an all-zero view (never an empty
 * transcript a reader could mistake for "the join did nothing").
 *
 * Walk the ring by re-issuing with `first` advanced by `view.n_rows`;
 * `n_rows == 0` is the end. `view.recorded - view.count` is exactly how many
 * records the wrap dropped, so a truncated transcript says so rather than
 * looking complete -- and a row's `repeat`/`t_last_ms` carry how many times the
 * identical fact repeated back to back (kernel-core vms_cnxman_diag.h SS4b: an
 * identical fact coalesces into the record it repeats, which is what stops a
 * once-a-second watchdog erasing the join drive it is waiting on).
 *
 * The two row structs mirror src/kernel-core/vms_cnxman_diag.h's
 * struct cnxman_diag_rec / struct cnxman_diag_view byte-for-byte -- the same
 * "ONE facility source, duplicated struct declaration" shape as the sibling
 * DIAG ioctls, because this header must stay includable with no kernel-core
 * dependency, and vms_devtab.c's _Static_asserts pin the two copies together.
 */
#define VMS_CLUSTER_DIAG_JOIN_ROWS 32u   /* == CNXMAN_DIAG_ROWS */

struct cnxman_diag_rec_wire {
    uint32_t seq;
    uint32_t t_ms;
    uint32_t t_last_ms;
    uint32_t repeat;
    uint8_t  kind;
    uint8_t  state;
    uint8_t  new_state;
    uint8_t  event;
    uint8_t  detail;
    uint8_t  cat;
    uint8_t  op;
    uint8_t  rx;
    int32_t  rc;
    uint32_t aux;
};
_Static_assert(sizeof(struct cnxman_diag_rec_wire) == 32,
               "cnxman_diag_rec_wire changed size -- must match cnxman_diag_rec");

struct cnxman_diag_view_wire {
    uint32_t count;
    uint32_t recorded;
    uint32_t first;
    uint32_t n_rows;
    uint8_t  join_state;
    uint8_t  join_failure;
    uint8_t  enabled;
    uint8_t  pad0;
    uint32_t ignored_events;
    struct cnxman_diag_rec_wire row[VMS_CLUSTER_DIAG_JOIN_ROWS];
};
_Static_assert(sizeof(struct cnxman_diag_view_wire) == 1048,
               "cnxman_diag_view_wire changed size -- must match cnxman_diag_view");

struct vms_cluster_diag_join_args {
    uint32_t first;                       /* in: first HELD record to copy   */
    uint32_t status;                      /* return: SS$_ status             */
    struct cnxman_diag_view_wire view;    /* return: the window + live state */
};
_Static_assert(sizeof(struct vms_cluster_diag_join_args) == 1056,
               "vms_cluster_diag_join_args changed size -- VMS_IOCTL_CLUSTER_DIAG_JOIN ABI break");
/*
 * NR 0x6d: the next unused number in this magic (0x6c is CLUSTER_GETSYI just
 * above). 1056 bytes is under NetBSD's one-page IOCPARM_MAX, so this rides the
 * same pre-copy _IOWR path as every other cluster diagnostic. The encoded value
 * below was computed for THIS struct's size -- _IOWR folds sizeof(type) into
 * the command word, so appending a field silently changes the ioctl NUMBER, and
 * this assert is what turns that into a build failure instead of an ENOTTY on a
 * booted node.
 */
#define VMS_IOCTL_CLUSTER_DIAG_JOIN _IOWR(VMS_IOC_MAGIC, 0x6d, struct vms_cluster_diag_join_args)
_Static_assert(VMS_IOCTL_CLUSTER_DIAG_JOIN == 0xC420566Du,
               "VMS_IOCTL_CLUSTER_DIAG_JOIN encodes differently than the reference build");

/*
 * VMS_IOCTL_SYSGEN_LOAD (FC-P0.10, docs/plan-faithful-cluster-executive.md).
 * STARTUP.EXE's own case of SYSBOOT: hands the cluster SYSGEN parameters and
 * the CLUSTER_AUTHORIZE record it read off SYS$SYSTEM:OVMXVMSSYS.PAR
 * (sysgen_params.h, cluster_authorize.h) into the executive's ONE
 * struct vms_cluster (vms_cluster_node(), FC-P0.9's singleton) so every
 * later cluster layer reads real loaded state, never a compiled-in default.
 * Issued once, before VMS_IOCTL_CLUSTER_START (FC-P0.11) -- reproducing
 * SYSBOOT's ordering (vms_cluster.h section 2's header comment: "on VMS
 * these are in the executive before SYSINIT forms or joins").
 *
 * Field-for-field this is struct vms_cluster_params (vms_cluster.h) plus the
 * `status` return. (struct vms_cluster_params also carries params_valid's
 * companion in struct vms_cluster -- a flag the EXECUTIVE sets on commit and
 * that deliberately has no field here, so no caller can assert it.)
 * The dispatcher (vms_ioctl_sysgen_load, vms_devtab.c)
 * copies each field explicitly rather than a byte-identical memcpy, because
 * -- unlike CLUSTER_DIAG_PORT's pure snapshot reads -- this ioctl performs
 * the negctl validation the plan row requires: VAXCLUSTER >= 1 with no
 * SCSNODE loaded is SS$_BADPARAM, logged, and struct vms_cluster.params is
 * left at its prior (honest, zeroed) state -- never a fabricated identity
 * (INV-6). This header stays includable with no kernel-core dependency, so
 * the field layout is duplicated here rather than shared by #include, same
 * discipline as the CLUSTER_DIAG_PORT row structs above.
 *
 * SCSSYSTEMID is split into _lo/_hi uint32_t halves rather than one uint64_t
 * -- the same "no raw 64-bit field in a wire struct" discipline
 * vms_pe_vc_view_wire's peer_sysid_lo/hi already uses, because a struct built
 * entirely from <=4-byte fields lays out IDENTICALLY on ILP32 (elf32-vax) and
 * LP64 (x86_64/NetBSD-amd64): a bare uint64_t does not (the VAX cross-compile
 * gate caught exactly this drift; see docs/design-faithful-cluster-executive.md's
 * "Word width" leak-table entry).
 */
struct vms_sysgen_load_args {
    /* ---- identity (fatal if absent with vaxcluster >= 1) ---- */
    uint8_t  scsnode[8];            /* in: VMS_SCSNODE_MAX(6)+2, blank/NUL padded */
    uint8_t  scsnode_len;           /* in: significant chars, 0..6 (0 = unset)    */
    uint8_t  pad0;
    uint32_t scssystemid_lo;        /* in: SCSSYSTEMID low word, 0 = unset        */
    uint32_t scssystemid_hi;        /* in: SCSSYSTEMID high word (48-bit id: 0 today) */

    /* ---- membership / quorum arithmetic ---- */
    uint16_t votes;                 /* in: VOTES                                  */
    uint16_t expected_votes;        /* in: EXPECTED_VOTES                         */
    uint16_t qdskvotes;             /* in: QDSKVOTES                              */
    uint16_t recnxinterval;         /* in: RECNXINTERVAL, seconds                 */
    uint16_t timvcfail;             /* in: TIMVCFAIL, its SYSGEN unit             */
    uint16_t cluster_credits;       /* in: CLUSTER_CREDITS                        */

    /* ---- roles ---- */
    uint8_t  vaxcluster;            /* in: VAXCLUSTER, 0 = never, 1 = if present, 2 = always */
    uint8_t  lockdirwt;             /* in: LOCKDIRWT                              */
    uint8_t  alloclass;             /* in: ALLOCLASS                              */
    uint8_t  mscp_load;             /* in: MSCP_LOAD                              */
    uint8_t  mscp_serve_all;        /* in: MSCP_SERVE_ALL                         */
    uint8_t  pad1[3];

    uint32_t niscs_max_pktsz;       /* in: NISCS_MAX_PKTSZ (clamped by the port)  */

    /* ---- DISK_QUORUM (empty = none) ---- */
    uint8_t  disk_quorum[16];       /* in: quorum-disk device name                */
    uint8_t  disk_quorum_len;       /* in: significant chars, 0 = none            */
    uint8_t  pad2;

    /* ---- CLUSTER_AUTHORIZE (group + password) ---- */
    uint16_t auth_group;            /* in: CLUSTER_AUTHORIZE group, 0 if !valid   */
    uint8_t  auth_password[32];     /* in: CLUSTER_AUTHORIZE password (VMS_CLUSTER_PWD_LEN) */
    uint8_t  auth_password_len;     /* in: significant bytes                      */
    uint8_t  auth_valid;            /* in: 1 = a real CLUSTER_AUTHORIZE.DAT was read */

    /*
     * ---- this node's OWN software identity (NOT a SYSGEN parameter) ----
     * The one field here that does not come from OVMXVMSSYS.PAR. The SCS START
     * body advertises an 8-byte software version at abs 72 (spec SS4(g)); the
     * value is OVMX's, and its SSOT (OVMX_CLUSTER_SW_VERSION,
     * src/libvms/include/ovmx_identity.h) is USERLAND -- kernel-core cannot
     * include it and may hold no version literal of its own (INV-1). So the
     * boot CARRIES it down here, exactly as it carries SCSNODE: STARTUP.EXE
     * reads the SSOT, this ioctl commits it, and vms_pe.c reads it back out of
     * committed executive state to fill the wire field. Never a constant in the
     * executive, and never echoed from a peer's START (a real VAX's "VMS V7.3"
     * is that VAX's identity; repeating it is a masquerade, INV-0).
     *
     * sw_version_len 0 = no token supplied: the port then advertises zeros and
     * counts it (pe_fsm.vc_sw_version_absent), never a fabricated version.
     */
    uint8_t  sw_version[8];         /* in: the SSOT's token, blank/NUL padded     */
    uint8_t  sw_version_len;        /* in: significant chars, 0 = not supplied    */
    uint8_t  pad3;

    uint32_t status;                /* return: SS$_ status                        */
};
_Static_assert(sizeof(struct vms_sysgen_load_args) == 112,
               "vms_sysgen_load_args changed size -- VMS_IOCTL_SYSGEN_LOAD ABI break");
#define VMS_IOCTL_SYSGEN_LOAD _IOWR(VMS_IOC_MAGIC, 0x3e, struct vms_sysgen_load_args)
_Static_assert(VMS_IOCTL_SYSGEN_LOAD == 0xC070563Eu,
               "VMS_IOCTL_SYSGEN_LOAD encodes differently than the reference build");

/*
 * VMS_IOCTL_CLUSTER_START (FC-P0.11; join semantics added by FC-P3.9,
 * docs/plan-faithful-cluster-executive.md). SYSINIT's own ordering, in one
 * call (design SS3.5 step 2): start the fork thread, bring PEA0: up on ETH0:,
 * start SCS on that port, then start the CONNECTION MANAGER, which forms or
 * joins per VAXCLUSTER. STARTUP.EXE issues it BEFORE the system disk is
 * mounted, exactly as VMS joins before mounting.
 *
 * Issued once, after VMS_IOCTL_SYSGEN_LOAD, from STARTUP.EXE's boot path
 * (ovmx_init.c) -- gated there on VAXCLUSTER != 0 (cluster_boot_gate.h): the
 * executive's own vms_pe_start() (FC-P0.9) applies the SAME gate internally
 * (SS$_NOSUCHDEV for VAXCLUSTER=0), so a stray call with the port not wanted
 * is refused twice over, never silently upgraded to a running port.
 *
 * Takes no `in:` fields: the port starts from vms_cluster_node()'s already-
 * loaded struct vms_cluster.params (VMS_IOCTL_SYSGEN_LOAD's own commit) --
 * never a second, possibly different, copy of SYSGEN state riding this call.
 *
 * `cluster_state` is READ BACK from the executive's own struct vms_cluster
 * after the call, not composed from `status`: it is `enum vms_cluster_state`
 * (src/kernel-core/vms_cluster.h) -- OFF / PORT_UP / JOINING / MEMBER /
 * STANDALONE. STARTUP.EXE renders the operator line from THIS value, so the
 * console can never announce a membership the executive does not hold
 * (INV-6). A node with no connection manager to join through is JOINING (with
 * VAXCLUSTER=2, "waiting to form or join") or STANDALONE (VAXCLUSTER=1) --
 * never MEMBER.
 */
struct vms_cluster_start_args {
    uint32_t port_up;               /* return: 1 = PEA0: is up after this call */
    uint32_t status;                /* return: SS$_ status                     */
    uint32_t cluster_state;         /* return: enum vms_cluster_state          */
};
_Static_assert(sizeof(struct vms_cluster_start_args) == 12,
               "vms_cluster_start_args changed size -- VMS_IOCTL_CLUSTER_START ABI break");
#define VMS_IOCTL_CLUSTER_START _IOWR(VMS_IOC_MAGIC, 0x3f, struct vms_cluster_start_args)
_Static_assert(VMS_IOCTL_CLUSTER_START == 0xC00C563Fu,
               "VMS_IOCTL_CLUSTER_START encodes differently than the reference build");

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

/*
 * VMS_IOCTL_REGISTER_SUBPROCESS - register a GENUINELY NEW VMS process that
 * INHERITS its creator's executive identity (vms-19e9, "$CREPRC identity
 * propagation").
 *
 * WHY THIS EXISTS, and why it is NOT VMS_IOCTL_REGISTER_CONTINUE. SPAWN /
 * $CREPRC create a SUBPROCESS: a genuinely new VMS process, with its OWN
 * distinct VMS PID, that inherits the CREATOR's UIC, user name and privileges
 * (OpenVMS System Services Reference, $CREPRC: the created process gets the
 * creator's privileges/UIC/user name by default). OVMX fork()s the child, and
 * the child touches the executive BEFORE image activation -- so it used to
 * register via VMS_IOCTL_REGISTER (a fresh, capable()-DERIVED identity: empty
 * user name, no SETPRV), then try to STAMP the creator's identity onto itself
 * with VMS_IOCTL_SETIDENT. That stamp is a self-declaration, and the setident
 * guard correctly REFUSES it for a non-root child (the runtime's interactive
 * DCL runs at the user's UIC after LOGINOUT's credential drop): SS$_NOPRIV,
 * surfaced as %DCL-F-CREPRC. SPAWN was dead in the booted runtime.
 *
 * The fix is inheritance by CONTINUATION, not by self-declaration: the
 * executive derives the child's identity from its UNFORGEABLE real_parent (the
 * SPAWNing DCL) -- exactly as VMS_IOCTL_REGISTER_CONTINUE does for image
 * activation -- so no privileged name is ever self-declared. The ONE
 * difference from _CONTINUE, and the reason this is a separate opcode: a
 * subprocess is a DISTINCT VMS process, so it is MINTED A FRESH VMS PID rather
 * than sharing its parent's. Sharing the PID (as _CONTINUE does, because DCL
 * and the image it activates ARE one VMS process) would make $GETJPI-by-PID
 * ambiguous between DCL and its subprocess -- which is precisely what
 * lib$spawn's wait resolves the child's backing Linux pid through.
 *
 * NOTHING is read from args (output-only, like VMS_IOCTL_REGISTER); the struct
 * returns the FRESH VMS PID.
 *
 * SECURITY. Identical posture to _CONTINUE: the identity is the parent's
 * CURRENT masks (a setident'd-DOWN parent cannot resurrect a privilege in its
 * child), and the caller cannot pick its parent -- the executive reads
 * real_parent, never anything the caller declares. This is not a path to
 * borrow an unrelated privileged process's identity, and it does NOT weaken
 * the VMS_IOCTL_SETIDENT guard, which still refuses a self-declared privileged
 * name (test_syssvc_creprc_inherit.c pins both halves).
 *
 * NR 0x42 with the 8-byte struct -> command word 0xC0085642, distinct from
 * VMS_IOCTL_GETJPI's 0xC1205642 (same NR, larger struct): the same NR-sharing
 * the register family already carries (REGISTER_CONTINUE 0x41 shares its NR
 * with SETPRN), the encoded size keeping the words apart.
 */
#define VMS_IOCTL_REGISTER_SUBPROCESS \
                            _IOWR(VMS_IOC_MAGIC, 0x42, struct vms_register_args)

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
    /*
     * DVI$_MSCP_SERVED (dvidef.h 0x0073, "Device is MSCP served"): 1 for a
     * disk this node reaches through the MSCP disk class driver on another
     * cluster member (FC-P7.1). Took the struct's trailing pad word, so the
     * layout and the 72-byte ABI guard below are unchanged.
     */
    uint32_t mscp_served;
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
/*
 * THE JIB QUOTA BLOCK RIDES SETIDENT (vms-14a). Identity and the authorized
 * quota set arrive together, from the same SYSUAF record LOGINOUT just
 * authenticated, exactly as they do on OpenVMS where LOGINOUT copies the
 * account's quota cells into the JIB while establishing the process. quota is
 * used only when quota_valid == 1; a caller with no quota to establish (a
 * $CREPRC subprocess that does not re-read SYSUAF) passes quota_valid == 0 and
 * the executive leaves VMS_PI_V_QUOTA clear on that process (honest omission,
 * INV-6). Growing this struct deliberately moves VMS_IOCTL_SETIDENT's request
 * number (the size folds into the _IOC encoding) -- the kernel module and
 * every userspace client are rebuilt together from this one header, so it is a
 * compile-time event, exactly like the procinfo growth documented below.
 */
struct vms_ident_args {
    char     username[VMS_USERNAME_SIZE]; /* authenticated user name */
    uint32_t uic;                         /* (group << 16) | member */
    uint32_t status;                      /* return: SS$_ status */
    uint64_t authorized_privs;            /* SYSUAF uaf$q_priv */
    uint32_t quota_valid;                 /* 1 = quota below is sourced */
    uint32_t quota_pad;                   /* keep the quota block 4-aligned/size stable */
    struct vms_jib_quota quota;           /* authorized JIB quota set (SYSUAF) */
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

/*
 * VMS_IOCTL_SPAWN_NOTIFY - arm a /NOWAIT subprocess-exit completion (vms-e9a
 * B1, docs/design-libspawn-ovmx.md §3b). The CALLER (the parent of a subprocess
 * it created via $CREPRC / LIB$SPAWN/NOWAIT) asks the executive to notify it
 * when that subprocess records its image completion status ($EXIT ->
 * VMS_IOCTL_SETEXIT). This is the executive-resident half of LIB$SPAWN's
 * efn/astadr/astprm completion contract: a /NOWAIT spawn returns immediately,
 * and the parent later learns the subprocess finished because the executive
 * SET the parent's event flag and/or QUEUED the parent's completion AST at the
 * child's exit -- cross-process delivery no per-process fake could carry (Rule
 * 9 / INV-6).
 *
 * child_vms_pid names the subprocess (the VMS PID $CREPRC handed back). efn is
 * the parent event flag to set on the child's exit, or VMS_EF_NONE when no flag
 * was requested. astadr/astprm are the completion AST routine + parameter (both
 * opaque userspace values, queued into the parent's AST queue at the parent's
 * current access mode), or astadr == 0 for no AST. The registration is one-shot
 * (delivered once, at the child's exit) and mirrors the mailbox write-attention
 * AST (vms-9003): the AST lands in the parent's own executive AST queue and is
 * drained via $SETAST/DELIVERAST, the same 4-level queue $DCLAST uses.
 *
 * If the child has ALREADY recorded its exit status when the parent arms
 * (a fast subprocess that finished before the parent got here), the executive
 * delivers the notification IMMEDIATELY and sets completed = 1, so no
 * completion is ever lost to that race. A cross-process arm is AUTHORIZED, NOT
 * FREE: gated by vms_proc_may_read() (same UIC group, or WORLD) exactly like
 * $WAKE/$GETJPI, SS$_NONEXPR for no such process, SS$_NOPRIV when refused.
 */
#define VMS_EF_NONE 0xFFFFFFFFu           /* efn sentinel: no completion flag */

struct vms_spawn_notify_args {
    uint32_t child_vms_pid;   /* in:  VMS PID of the /NOWAIT-spawned subprocess */
    uint32_t efn;             /* in:  parent EF to set on exit, or VMS_EF_NONE  */
    uint64_t astadr;          /* in:  parent completion AST routine (0 = none)  */
    uint64_t astprm;          /* in:  parameter passed to the completion AST    */
    uint32_t status;          /* out: SS$_ status of the arm operation          */
    uint8_t  completed;       /* out: 1 = child had already exited (delivered)  */
    uint8_t  pad[3];
};

/*
 * System memory statistics ($GETSYI-style, the reader behind SHOW MEMORY's
 * "Physical Memory Usage" section -- rd vms-a3cd). System-wide, so unlike the
 * $GETJPI process row it carries no per-process identity and needs no target
 * selector. The executive reports two figures it can source honestly from the
 * host VM system: total managed physical memory and current free memory, both
 * in BYTES -- arch-neutral, because the kernel converts its own page count with
 * its own PAGE_SIZE, so no VMS/host page-size skew crosses the wire. Fields VMS
 * also shows but OVMX has no faithful source for (the Modified page list, VIO
 * cache, pool, paging-file usage) are NOT carried here -- the renderer omits
 * those sections rather than fabricate them (INV-6).
 */
struct vms_syi_meminfo {
    uint64_t total_bytes;    /* out: total managed physical memory (bytes) */
    uint64_t free_bytes;     /* out: current free memory (bytes) */
    uint32_t fields_valid;   /* out: VMS_SYIMEM_V_* -- which figures are real */
    uint32_t reserved;       /* must be zero */
};

#define VMS_SYIMEM_V_PHYS 0x00000001u  /* total_bytes/free_bytes are measured */

struct vms_getsyi_mem_args {
    struct vms_syi_meminfo info;   /* out: the memory figures */
    uint32_t status;               /* return: SS$_ status */
    uint32_t reserved;             /* must be zero */
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
/* /NOWAIT subprocess-exit completion arm (vms-e9a B1, LIB$SPAWN efn/astadr) */
#define VMS_IOCTL_SPAWN_NOTIFY _IOWR(VMS_IOC_MAGIC, 0x4D, struct vms_spawn_notify_args)
/* System-info facility ($GETSYI-style; SHOW MEMORY physical section, vms-a3cd) */
#define VMS_IOCTL_GETSYIMEM _IOWR(VMS_IOC_MAGIC, 0x68, struct vms_getsyi_mem_args)

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
_Static_assert(sizeof(struct vms_ident_args) == 104,
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
_Static_assert(sizeof(struct vms_spawn_notify_args) == 32,
               "vms_spawn_notify_args layout changed: VMS_IOCTL_SPAWN_NOTIFY ABI break");
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
_Static_assert(VMS_IOCTL_SETIDENT == 0xC0685644u,
               "VMS_IOCTL_SETIDENT encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_SETTERM == 0xC0085645u,
               "VMS_IOCTL_SETTERM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ESTABLISH_SYSTEM == 0xC0085646u,
               "VMS_IOCTL_ESTABLISH_SYSTEM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_REGISTER == 0xC0085640u,
               "VMS_IOCTL_REGISTER encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_REGISTER_CONTINUE == 0xC0085641u,
               "VMS_IOCTL_REGISTER_CONTINUE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_REGISTER_SUBPROCESS == 0xC0085642u,
               "VMS_IOCTL_REGISTER_SUBPROCESS encodes differently here than on the reference build");
_Static_assert(sizeof(struct vms_getsyi_mem_args) == 32,
               "vms_getsyi_mem_args layout changed: VMS_IOCTL_GETSYIMEM ABI break");
_Static_assert(VMS_IOCTL_GETSYIMEM == 0xC0205668u,
               "VMS_IOCTL_GETSYIMEM encodes differently here than on the reference build");

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

/* ================================================================
 * L2 (raw datalink) socket surface (vms-7eb, auth slice of vms-1e4).
 *
 * The ioctl structures and request numbers live in vms_l2.h, included here
 * for the same reason as vms_lnm.h / vms_mbx.h / vms_bg.h / vms_acp.h above:
 * one frozen definition for every consumer, kernel and userspace, on either
 * architecture. Gives a NON-ROOT VMS process kernel-owned raw L2 I/O for the
 * SCS cluster wire (ethertype 0x6007), gated on VMS_PRV_M_PHY_IO below.
 * ================================================================ */
#include "vms_l2.h"

#endif /* _VMS_IOCTL_H */
