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
/* Logical-name (LNM$SYSTEM/GROUP/JOB) wire contract (rd vms-72da): the
 * DEFINE/DELETE/GETSCOPE arg structs the facility copies, the read-only arena
 * struct the char device's mmap publishes, the VMS_LNM_* sizing/table ids, and
 * the SYSNAM/GRPNAM/SYSPRV/GRPPRV privilege bits lnm_priv_check() gates on.
 * Byte-identical to src/kernel/vms_lnm.h. lnm is the LAST facility to join the
 * NetBSD module's SRCS (the arena seam was contract-only through vms-d61). */
#include "vms_lnm_nb.h"
/* Files-11 (ODS-2) ACP wire contract (rd vms-6a7f, epic vms-208): the
 * $MOUNT/$DISMOUNT/$ASSIGN/IO$_ACCESS/IO$_DEACCESS/READVBLK/WRITEVBLK arg
 * structs src/kernel-core/vmsfs_acp.c copies, VMS_ACP_NAME_SIZE and
 * VMS_ACP_ACCTL_WRITE. Byte-identical to src/kernel/vms_acp.h. This is the
 * elf32-vax cross-build's compile-coverage rung; the real NetBSD/VAX kmod
 * driver wiring (d_ioctl dispatch) is a later re-target (vms-d5d). */
#include "vms_acp_nb.h"
/* Device-table wire contract (rd vms-618): the $ASSIGN/$DASSGN/$ALLOC/$DALLOC/
 * $GETDVI/$DEVICE_SCAN/IO$_SETMODE arg structs src/kernel-core/vms_devtab.c
 * copies, the VMS_TTC_* terminal characteristics, VMS_NETIF_SIZE and the
 * VMS_DVI_SEL_ / VMS_TTSET_ selectors. Byte-identical to src/kernel/vms_ioctl.h.
 * The device table is the LAST executive facility to join the NetBSD module's
 * SRCS; before it did, VMS_IOCTL_ALLOC answered ENOTTY and DCL's cmd_mount()
 * (which $ALLOCs before it $MOUNTs) could not mount a device on NetBSD/vax. */
#include "vms_devtab_nb.h"

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
/* Logical-name subset (rd vms-72da). Values match src/kernel/vms_internal.h
 * exactly. SS__IVLOGNAM (340) comes from vms_proctab_nb.h. SUPERSEDE/NOLOGNAM
 * are public $SSDEF; SS__EXLNMQUOTA is the ONE value not yet in ssdef.h --
 * ORACLE-PINNED (lab-1 F$MESSAGE, design docs/design-logical-name-placement.md
 * §4.2, vms-556) exactly as src/kernel/vms_internal.h pins it. */
#define SS__SUPERSEDE   844        /* SS$_SUPERSEDE (a name was superseded) */
#define SS__NOLOGNAM    444        /* SS$_NOLOGNAM (no such logical name) */
#define SS__EXLNMQUOTA  8780       /* oracle-pinned lab-1 F$MESSAGE (arena full) */
/*
 * Files-11 (ODS-2) ACP subset (rd vms-6a7f, epic vms-208): the six SS$ codes
 * src/kernel-core/vmsfs_acp.c returns that were not yet in this twin. Values
 * are copied VERBATIM from src/kernel/vms_internal.h -- NOT independently
 * re-derived here -- so the Linux and NetBSD builds of the SAME shared
 * facility source can never answer a given fault with two different
 * numbers. Each is that Linux header's existing single-lineage
 * src/libvms/include/ssdef.h value (see its own per-constant provenance
 * comment there); SS__NOSUCHFILE in particular carries a FILED discrepancy
 * there (2320 oracle vs 2696 this tree, under reconciliation) -- this twin
 * intentionally tracks whatever src/kernel/vms_internal.h currently ships
 * (2696) so the two substrates agree with each other while that
 * reconciliation is open, rather than picking a value neither side used.
 */
#define SS__ACCVIO      0x0000000C /* SS$_ACCVIO (access violation) */
#define SS__DEVNOTMOUNT 2688       /* SS$_DEVNOTMOUNT (device not mounted / not ODS-2) */
#define SS__NOSUCHFILE  2696       /* SS$_NOSUCHFILE (IO$_ACCESS resolve miss) */
#define SS__FILNOTACC   2744       /* SS$_FILNOTACC (IO$_DEACCESS w/o access) */
#define SS__DEVICEFULL  2664       /* SS$_DEVICEFULL (extend cannot allocate) */
#define SS__DEVALLOC    2112       /* SS$_DEVALLOC (device already allocated to another user) */
/*
 * Device-table subset (rd vms-618). Values copied VERBATIM from
 * src/kernel/vms_internal.h -- same provenance rule as the ACP codes above: the
 * Linux and NetBSD builds of the SAME shared facility source
 * (src/kernel-core/vms_devtab.c) must never answer a given refusal with two
 * different numbers. SS$_DEVNOTALLOC is what the oracle returned for DEALLOCATE
 * of a device this process does not have ALLOCATED (%SYSTEM-W-DEVNOTALLOC,
 * docs/oracle/vax73-terminal-device.md section 7); SS$_NOMOREDEV terminates a
 * $DEVICE_SCAN.
 */
#define SS__DEVNOTALLOC 2136       /* SS$_DEVNOTALLOC (device not allocated) */
#define SS__NOMOREDEV   2648       /* SS$_NOMOREDEV (device scan exhausted) */

/*
 * IO$_ACPCONTROL / wildcard $SEARCH exhaustion (rd vms-a0b, epic vms-208).
 * SS__NOMOREFILES copied VERBATIM from src/kernel/vms_internal.h (oracle-
 * pinned == 2352, vms-a0b) -- same provenance rule as the six codes above:
 * the Linux and NetBSD builds of the SAME shared facility source
 * (src/kernel-core/vmsfs_acp.c) must never answer wildcard exhaustion with
 * two different numbers.
 */
#define SS__NOMOREFILES 2352       /* SS$_NOMOREFILES (wildcard $SEARCH exhausted) */

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
#ifndef VMS_PRV_V_MOUNT
#define VMS_PRV_V_MOUNT   17             /* execute the MOUNT ACP function (PRV$V_MOUNT) */
#endif
#ifndef VMS_PRV_M_MOUNT
#define VMS_PRV_M_MOUNT  (1ULL << VMS_PRV_V_MOUNT)
#endif

/*
 * VMS_PRV_M_ENFORCED (rd vms-72da) -- the privilege mask a PRIVILEGED process is
 * seeded with, BYTE-IDENTICAL to src/kernel/vms_ioctl.h. It MUST include SYSNAM:
 * the executive gates LNM$SYSTEM DEFINE/DELETE on SYSNAM|SYSPRV (vms_lnm.c
 * lnm_priv_check), and PID 1 (ovmx_init) seeds SYS$SYSTEM / SYS$SYSROOT /
 * SYS$SYSDEVICE through lnm_setup_defaults BEFORE any $SETIDENT, so a privileged
 * proc must ALREADY hold SYSNAM or the boot's system logicals never get created.
 * The netbsd proc seed previously hand-listed only CMKRNL|CMEXEC|SETPRV, omitting
 * SYSNAM -- so SYS$SYSTEM never resolved on netbsd and the vax boot halted at
 * %OVMX-F-EXECINIT. Component bits: CMKRNL/CMEXEC/SETPRV (vms_access_nb.h), WORLD
 * (vms_proctab_nb.h), SYSNAM/GRPNAM (vms_lnm_nb.h), MOUNT (above) -- all in scope
 * here, since vms_internal.h includes those twins above.
 */
#ifndef VMS_PRV_M_ENFORCED
#define VMS_PRV_M_ENFORCED  (VMS_PRV_M_CMKRNL | VMS_PRV_M_CMEXEC | \
                             VMS_PRV_M_SETPRV | VMS_PRV_M_WORLD | \
                             VMS_PRV_M_SYSNAM | VMS_PRV_M_GRPNAM | \
                             VMS_PRV_M_MOUNT)
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

/* pr_warn (Linux) -- same NetBSD kernel printf. The device table (rd vms-618)
 * uses it for its out-of-memory unit-creation warnings. */
#ifndef pr_warn
#define pr_warn(fmt, ...)  printf(fmt, ##__VA_ARGS__)
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
	uint32_t            req_csid;       /* cluster CSID this lock is held FOR;
	                                     * 0 = a local process owns it, non-zero =
	                                     * a cross-node grant on behalf of a remote
	                                     * node (vms-e8f1). Set from
	                                     * vms_enq_args.owner_csid at creation. */
	uint32_t            req_lkid;       /* the REMOTE requester's own lock handle
	                                     * for a cross-node grant (vms-6ca, H5).
	                                     * 0 for a local lock. Set from the wire
	                                     * ENQ's req_lkid so a later deferred GRANT
	                                     * can name the requester's original
	                                     * request when this lock's release grants
	                                     * a queued cross-node waiter. */
	uint32_t            parent_id;      /* the lkid of this lock's PARENT lock, or
	                                     * 0 for a root (parentless) lock. Set from
	                                     * vms_enq_args.parid at creation; reported
	                                     * by GETLKI. RMS record locks (vms-0dd)
	                                     * carry the file-access lock (vms-50e) as
	                                     * parent. Purely additive (existing $ENQ
	                                     * passes parid=0); the auto-release cascade
	                                     * is a follow-on (vms-489). Mirror of the
	                                     * Linux vms_internal.h field. */
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
 * DLM directory membership vector (rd vms-1bba, the "DB" rung). A CONTROLLED,
 * STATIC configuration input supplied at load time (the Linux rind exposes it
 * as a module_param_array; this NetBSD substrate defines the symbols with a
 * cluster-of-one default). dlm_directory_csid() hashes a resource name across
 * this vector to pick the directory node, so every node given the SAME vector
 * resolves the SAME directory/master for a name. NOT the live membership feed
 * (that is the 0.4 "DC" successor); an honest controlled input, never
 * fabricated live state. dlm_member_count == 0 -> cluster-of-one on the local
 * CSID (single-node behaviour preserved). Same order required on every node.
 */
#define VMS_DLM_MAX_MEMBERS 16
extern uint32_t dlm_member_csids[VMS_DLM_MAX_MEMBERS];
extern int      dlm_member_count;

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

	/* Authorized JIB quota set (vms-14a) -- byte-twin of the Linux struct
	 * vms_proc field. Stamped by the SHARED kernel-core/vms_proctab.c
	 * vms_ioctl_setident() under hash_lock+mode_lock, read by its
	 * proc_fill_info(). quota is meaningful only when quota_valid == 1;
	 * otherwise proc_fill_info leaves VMS_PI_V_QUOTA clear (honest omission,
	 * INV-6). OVMX shows the configured quota, it does not enforce/charge it. */
	uint8_t             quota_valid;      /* 1 = quota below is sourced */
	struct vms_jib_quota quota;           /* authorized JIB quota set (SYSUAF) */

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
	/*
	 * Job-tree id -- the LNM$JOB scope key derive_scope_key() reads (vms_lnm.c,
	 * rd vms-72da). On Linux the glue stamps it at registration from the parent
	 * (vms_proc_parent_job_id() in vms_module.c) so a SPAWN tree shares one
	 * LNM$JOB. The NetBSD glue does NOT yet derive a job tree, so this stays 0,
	 * HONESTLY -- exactly as terminal/p0/p1 stay empty until their glue joins
	 * SRCS. Consequence, stated not hidden: LNM$JOB on NetBSD currently collapses
	 * to a single scope (key 0). LNM$SYSTEM (scope 0 by definition) and LNM$GROUP
	 * (scope = real uic>>16) are unaffected -- those are the tables PROVISION's
	 * STARTUP uses. Wiring per-process job trees on NetBSD is a later item.
	 */
	uint32_t            job_id;                     /* LNM$JOB scope key (0 on NetBSD until job glue joins) */
	char                prcnam[VMS_PRCNAM_SIZE];   /* process name ("" if unnamed) */
	char                username[VMS_USERNAME_SIZE]; /* "" until $SETIDENT stamps it */
	/*
	 * The job's terminal. "" until VMS_IOCTL_SETTERM records one, which the
	 * executive only does from a channel this process already holds to a
	 * device of class DC$_TERM (vms_devtab.c) -- so the name is a device name
	 * out of the executive's own table, never a string the process supplied.
	 * Written and read under vms_proc_hash_lock, alongside prcnam/uic/username.
	 */
	char                terminal[VMS_DEVNAM_SIZE];

	/* P0 program / P1 control region extents (structural; $GETJPI reports them,
	 * VMS_IOCTL_P0_MAP/P1_MAP would record them -- not in this module's SRCS, so
	 * both pairs stay zero). Carried so proc_fill_info compiles and reports them
	 * absent, exactly as on Linux before an image is mapped. */
	uint64_t            p0_base;
	uint64_t            p0_limit;
	uint64_t            p1_base;
	uint64_t            p1_limit;

	/*
	 * Image completion $STATUS and CLI invocation context (vms-f60d) --
	 * the NetBSD twin of the fields the shared facility
	 * (src/kernel-core/vms_proctab.c) records via VMS_IOCTL_SETEXIT /
	 * VMS_IOCTL_SETCLI and reads via VMS_IOCTL_GETEXIT / VMS_IOCTL_GETCLI.
	 * exit_status is the process's image completion condition value ($STATUS
	 * longword; bit<0> success, bits<2:0> severity); cli_present is the
	 * cliflag and cli_command the invoking DCL command line, inherited from
	 * the invoking CLI's PCB at REGISTER_CONTINUE time. Same field set and
	 * meaning as src/kernel/vms_internal.h's Linux struct vms_proc, so the
	 * one shared facility source compiles and behaves identically here.
	 */
	uint32_t            exit_status;
	uint8_t             has_exit_status;
	uint8_t             cli_present;
	uint16_t            cli_length;
	char                cli_command[VMS_CLI_CMDLINE_SIZE];

	/*
	 * /NOWAIT subprocess-exit completion registration (vms-e9a B1). Lives on
	 * the CHILD's PCB; vms_ioctl_setexit() delivers it (parent EF + AST) when
	 * this process records its exit. Same field set and meaning as
	 * src/kernel/vms_internal.h's Linux struct vms_proc, so the one shared
	 * facility source (src/kernel-core/vms_proctab.c) compiles identically here.
	 * Guarded by vms_proc_hash_lock, alongside exit_status.
	 */
	uint8_t             compl_armed;
	uint8_t             compl_acmode;
	uint32_t            compl_parent_pid;
	uint32_t            compl_efn;
	uint64_t            compl_astadr;
	uint64_t            compl_astprm;

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

	/*
	 * DEVICE channels (rd vms-618) -- this process's channels to rows of the
	 * executive device table (struct vms_device below), on the SAME chan_lock/
	 * next_chan number space as the mailbox and file channels, because on real
	 * VMS a process's channels are ONE number space regardless of device kind.
	 * vms_ioctl_dassgn() (vms_devtab.c) checks this list FIRST and falls back
	 * to the file-class (ACP) and mailbox lists, so $DASSGN is the one ioctl
	 * that releases any channel kind.
	 */
	exec_list_head_t    channels;         /* struct vms_channel (below) */

	/* Files-11 (ODS-2) ACP file-class channels (rd vms-6a7f, epic vms-208) --
	 * a separate list on the SAME chan_lock/next_chan space, mirroring
	 * mbx_channels exactly (struct vms_acp_chan is defined in the shared
	 * src/kernel-core/vmsfs_acp.c). Added here so vmsfs_acp.c's `proc->
	 * file_channels' resolves under the NetBSD substrate twin the same way it
	 * already does under src/kernel/vms_internal.h's Linux struct vms_proc.
	 * Scope note: this field lands with the elf32-vax COMPILE-coverage rung
	 * (vms-6a7f); the NetBSD glue does not yet call vms_acp_release_all() in
	 * this build's process-teardown path (mirrored in vms_netbsd.c). */
	exec_list_head_t    file_channels;    /* struct vms_acp_chan (defined in vmsfs_acp.c) */

	/* Deferred-free head (P4-A, rd vms-ca7). The reaper unlinks a dead PCB under
	 * vms_proc_hash_lock (exec_hash_del_rcu) and reclaims it through
	 * exec_free_deferred(&proc->rcu, ...) -- immediate on NetBSD (no lockless
	 * readers), the blessed grace-period fallback. Mirrors Linux's kfree_rcu. */
	exec_rcu_head_t     rcu;
};

/* ================================================================
 * THE EXECUTIVE DEVICE TABLE (rd vms-618) -- src/kernel-core/vms_devtab.c.
 *
 * A VMS device is a thing the EXECUTIVE knows about: the driver enters a unit
 * in the I/O database at boot and from that moment it exists for every process
 * on the node. Ownership, allocation and reference count are properties of the
 * DEVICE, not of the process doing the asking -- which is the whole reason
 * these rows live here and not in a process's own memory (CLAUDE.md Rule 11 /
 * INV-6: a per-process device table passes every single-process test and is
 * still a facade; the decisive check is A-allocates / B-is-refused).
 *
 * Field names and types mirror src/kernel/vms_internal.h exactly (exec_* in
 * place of the Linux spinlock_t/list_head) so the shared facility source
 * compiles unchanged on this substrate.
 * ================================================================ */
struct vms_device {
	exec_list_head_t    list;           /* in vms_device_list */
	char                devnam[VMS_DEVNAM_SIZE];
	uint32_t            devclass;       /* DC$_ device class */
	uint32_t            devtype;        /* device type code; 0 = Unknown */
	uint32_t            shareable;      /* 1 = "shareable" in the status clause */

	/*
	 * OWNERSHIP AND ALLOCATION ARE TWO DIFFERENT THINGS, both oracle-measured
	 * (docs/oracle/vax73-terminal-device.md section 7): a channel to a
	 * NON-shareable device makes the assigner the OWNER with no allocation;
	 * $ALLOC sets `allocated' and is the only thing that does; an allocation
	 * outlives the channel until $DALLOC or the owner's death. refcnt is the
	 * device's "Reference count": one per assigned channel plus one for an
	 * outstanding allocation.
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
	 * Disk backing (devclass == DC$_DISK). The NATIVE block device this unit
	 * was entered from -- on this substrate "ra1c"/"ra2c", the MSCP disks under
	 * SIMH, entered by vms_blockdev_netbsd_register_units() from the
	 * device-native unit map (vms-47d: the VMS name is a label, the path is the
	 * real device). Empty and zero for every non-disk device.
	 */
	char                backing[VMS_BACKING_SIZE];
	uint32_t            backing_major;
	uint32_t            backing_minor;

	/*
	 * Ethernet backing (devclass == DC$_SCOM). The executive's PRIVATE record
	 * of which real interface ETH0: fronts; NEVER surfaced to a VMS program
	 * (INV-4). Empty/zero on this substrate today -- exec_netdev_primary is
	 * still a contract-only twin here, so no ETH0: unit is entered at all,
	 * which is the honest "this node has no enumerated NIC" state, not a fake
	 * device.
	 */
	char                netif[VMS_NETIF_SIZE];
	uint32_t            link_up;

	/*
	 * Every channel currently assigned to this device, by any process: the
	 * device has to know this to decide when IMPLICIT ownership ends (when the
	 * owner has no channel left, not when any channel is returned).
	 */
	exec_list_head_t    chanlist;       /* of vms_channel.devlink */

	exec_lock_t         lock;
};

/* A process's handle on a device. */
struct vms_channel {
	exec_list_head_t    list;           /* in vms_proc->channels */
	exec_list_head_t    devlink;        /* in vms_device->chanlist */
	uint32_t            chan;
	pid_t               owner_linux_pid;/* process holding this channel */
	struct vms_device  *dev;
	uint8_t             acmode;         /* access mode $ASSIGN was issued from */
};

/* ================================================================
 * Facility entry points -- DEFINED in src/kernel-core/vms_eflag.c, called by the
 * NetBSD `vms' pseudo-device dispatch (src/kernel-netbsd/vms_netbsd.c).
 * ================================================================ */
void vms_eflag_init(void);
void vms_eflag_cleanup(void);
void vms_proc_release_common_ef(struct vms_proc *proc);

/* Cross-process completion-EF primitive (vms-e9a B1): set a resolvable event
 * flag on an arbitrary target proc, waking its waiters. 0 if set, -1 if the efn
 * does not resolve on `target`. Shared source in src/kernel-core/vms_eflag.c. */
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
/* In-kernel volume-synchronization lock for the Files-11 ACP (vms-233): an
 * EX-mode $ENQ on a per-volume resource, enqueued directly in the caller's
 * ioctl context (no /dev/vms round-trip). Release with a zero lkid is a no-op. */
uint32_t vms_lock_acp_vol_ex(struct vms_proc *proc, const char *resnam,
                             uint32_t *lkid_out);
uint32_t vms_lock_acp_vol_release(struct vms_proc *proc, uint32_t lkid);
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

/* ----------------------------------------------------------------
 * LOGICAL-NAME facility (LNM$SYSTEM/GROUP/JOB, rd vms-72da) -- DEFINED in
 * src/kernel-core/vms_lnm.c, the LAST executive facility to join the NetBSD
 * module's SRCS. vms_lnm_init/cleanup bracket the module lifetime: init
 * allocates the ONE read-only-publishable arena through the exec_arena seam
 * (uvm_km_alloc(UVM_KMF_WIRED) on NetBSD) BEFORE /dev/vms exists; cleanup frees
 * it. The char device's d_mmap (vms_netbsd.c vms_mmap) publishes the arena's
 * wired pages read-only using vms_lnm_arena_base/_size. The three ioctl entry
 * points MUTATE the tables (translation is a zero-syscall mmap read, no ioctl).
 * ---------------------------------------------------------------- */
int    vms_lnm_init(void);
void   vms_lnm_cleanup(void);
void  *vms_lnm_arena_base(void);
size_t vms_lnm_arena_size(void);
/* DEFINED in the uvm glue TU vms_lnm_arena_netbsd.c (rd vms-72da): a load-time
 * console self-check that the arena the executive wrote is the one d_mmap
 * publishes (pmap_extract + magic readback). */
void   vms_lnm_arena_selftest(void);

/* ----------------------------------------------------------------
 * CLUSTER SEAM self-test (FC-P0.4, families SS14..SS18 of
 * exec_kbackend.h) -- DEFINED in tests/netbsd/guest/cluster_seam.c,
 * the R3 substrate-contract test. Follows vms_lnm_arena_selftest's
 * exact pattern: called once from vms_netbsd.c's MODULE_CMD_INIT,
 * exercises the real exec_lan_, exec_kthread_, exec_timer_,
 * exec_time_now_vms and exec_ticks_ms bindings directly (no ioctl --
 * vms_pe.c does not call the seam until FC-P0.9) and prints PASS/
 * FAIL/SKIP lines to the console for the harness to grep.
 * ---------------------------------------------------------------- */
void   vms_cluster_seam_selftest(void);

long   vms_ioctl_lnm_define(struct vms_proc *proc, unsigned long arg);
long   vms_ioctl_lnm_delete(struct vms_proc *proc, unsigned long arg);
long   vms_ioctl_lnm_getscope(struct vms_proc *proc, unsigned long arg);

/* ----------------------------------------------------------------
 * Files-11 (ODS-2) ACP (rd vms-6a7f, epic vms-208) -- DEFINED in
 * src/kernel-core/vmsfs_acp.c, added to THIS cross-build's compile-coverage
 * SRCS (tools/cross-vax/build-vms-module-vax.sh) but NOT (yet) to
 * src/kernel-netbsd/Makefile's real SRCS or to vms_netbsd.c's ioctl dispatch --
 * wiring it into the live NetBSD/vax `vms' module is a later re-target
 * (vms-d5d). Declared here only so vmsfs_acp.c type-checks against this real
 * contract twin.
 * ---------------------------------------------------------------- */
void vms_acp_init(void);
void vms_acp_cleanup(void);
long vms_ioctl_acp_mount(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_dmount(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_assign(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_access(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_deaccess(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_readvb(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_writevb(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_acpcontrol(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_fileop(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_acp_getvol(struct vms_proc *proc, unsigned long arg);  /* vms-e6f: $GETDVI volume items */
/*
 * VMS_IOCTL_DISK_RESOLVE handler (rd vms-f60) -- DEFINED in the quarantined
 * vms_blockdev_netbsd.c, NOT in the shared src/kernel-core/vms_devtab.c, whose
 * twin is compiled out here by -DOVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE. See that
 * guard's comment in vms_devtab.c for WHY the substrate keeps its own resolver
 * (it must lazily OPEN and cache the backing vnode for the ACP's block I/O, and
 * must NOT hold the INITIALIZE target open -- NetBSD allows one open of a block
 * device, so an eager bind at module init would EBUSY-block INITIALIZE.EXE).
 * Lets INITIALIZE.EXE name a VMS disk unit and get back the real backing device.
 */
long vms_ioctl_disk_resolve(struct vms_proc *proc, unsigned long arg);
int  vms_acp_dassgn(struct vms_proc *proc, uint32_t chan);
void vms_acp_release_all(struct vms_proc *proc);
/*
 * Internal (non-ioctl) resolve of a canonical disk-unit name to its backing
 * (major,minor) so the Files-11 ACP's $MOUNT can read the home block/SCB.
 * DEFINED on this substrate in vms_blockdev_netbsd.c, where it ALSO opens the
 * backing device vnode once and caches it for the volume's life -- which is
 * what exec_blockdev_read_block/write_block need and what the shared
 * vms_devtab.c twin (a pure table read) cannot do. See the
 * OVMX_DEVTAB_SUBSTRATE_DISK_RESOLVE guard in vms_devtab.c.
 */
uint32_t vms_devtab_disk_backing(const char *devnam,
                                 uint32_t *major_out, uint32_t *minor_out);

/*
 * Record ONE genuine device I/O error against the disk unit whose backing block
 * device is (major,minor) -- the WRITER for the per-device errcnt SHOW ERROR and
 * F$GETDVI(...,"ERRCNT") read (rd vms-5f82). Called from the ACP block-I/O path
 * (kernel-core/vmsfs_acp.c) only on a real failure return from
 * exec_blockdev_read_block/write_block; never speculatively (INV-6). Defined in
 * the shared kernel-core, on every substrate.
 */
void vms_devtab_note_io_error(uint32_t major, uint32_t minor);

/*
 * Transient twin of the above for INITIALIZE.EXE (rd vms-f60, vms_blockdev_
 * netbsd.c): resolves a VMS disk-unit name to the backing block-device NAME +
 * (major,minor) WITHOUT caching the vnode. Unlike $MOUNT's disk_backing, the
 * executive here only names the device; INITIALIZE opens and writes it itself.
 */
uint32_t vms_devtab_disk_resolve(const char *devnam, char *backing,
                                 size_t backing_sz, uint32_t *major_out,
                                 uint32_t *minor_out);

/*
 * Close every backing device vnode the ACP $MOUNT opened + cached
 * (vms_blockdev_netbsd.c). Called at module detach (vms-d5d) -- see
 * vms_netbsd.c's MODULE_CMD_FINI.
 */
void vms_blockdev_netbsd_release_all(void);

/*
 * Enter this node's DISK units in the executive device table (rd vms-618).
 * DEFINED in vms_blockdev_netbsd.c, which owns the device-native unit map
 * (VMS unit name -> real NetBSD block device); it calls vms_devtab_add_disk()
 * below for each unit that actually resolves. Called once, from
 * vms_modcmd(MODULE_CMD_INIT) right after vms_devtab_init(), the way a VMS
 * driver enters its units during system initialization -- before /dev/vms
 * exists, so no process can be reading the table.
 */
void vms_blockdev_netbsd_register_units(void);

/* ----------------------------------------------------------------
 * DEVICE TABLE facility (rd vms-618) -- DEFINED in src/kernel-core/vms_devtab.c,
 * the LAST executive facility to join this module's SRCS. vms_devtab_init/
 * cleanup bracket the module lifetime (the table is module-global: the console
 * terminal OPA0: is created at init, and the substrate's disk units are entered
 * right after by vms_blockdev_netbsd_register_units()); vms_proc_release_channels
 * gives back a dying process's channels AND any device it had allocated.
 * ---------------------------------------------------------------- */
int  vms_devtab_init(void);
void vms_devtab_cleanup(void);
/* Enter ONE disk unit, for a substrate whose disks the shared probe cannot
 * enumerate (see vms_devtab.c). Returns 0 on success, -ENOMEM on failure. */
int  vms_devtab_add_disk(const char *devnam, const char *backing,
                         uint32_t backing_major, uint32_t backing_minor);
void vms_proc_release_channels(struct vms_proc *proc);
long vms_ioctl_assign(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dassgn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_alloc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_dalloc(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getdvi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_devscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setterm(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_ttsetmode(struct vms_proc *proc, unsigned long arg);

/* ----------------------------------------------------------------
 * Cross-facility image-rundown release helpers. vms_ioctl_image_rundown()
 * (vms_access.c) calls all three; vms_proc_rundown_asts is DEFINED (vms_ast.c),
 * vms_proc_rundown_locks is DEFINED (vms_lock.c) and -- as of the device-table
 * port (rd vms-618) -- vms_proc_rundown_channels is DEFINED too, in
 * vms_devtab.c. All three now link to their real facility definitions; the WEAK
 * no-op stub vms_netbsd.c used to carry for the channel one is gone.
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

/* Facility-provided (src/kernel-core/vms_proctab.c): deliver a /NOWAIT spawn
 * completion for a subprocess reclaimed WITHOUT a recorded exit (SIGKILL/crash),
 * synthesizing an abnormal $STATUS (vms-2a4). Caller holds vms_proc_hash_lock and
 * has already unlinked `child`. Called from the reaper; on NetBSD the reaper is
 * the only mid-life claim point (there is no channel-release free path). */
void vms_proc_deliver_abnormal_completion(struct vms_proc *child);
bool vms_proc_may_read(const struct vms_proc *caller, const struct vms_proc *target);

long vms_ioctl_setprn(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getjpi(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_procscan(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setident(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_establish_system(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_hiber(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_wake(struct vms_proc *proc, unsigned long arg);
/* $EXIT/$STATUS + CLI invocation context (vms-f60d), shared facility in
 * src/kernel-core/vms_proctab.c; contract in vms_proctab_nb.h. */
long vms_ioctl_setexit(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getexit(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_setcli(struct vms_proc *proc, unsigned long arg);
long vms_ioctl_getcli(struct vms_proc *proc, unsigned long arg);
/* /NOWAIT subprocess-exit completion arm (vms-e9a B1). */
long vms_ioctl_spawn_notify(struct vms_proc *proc, unsigned long arg);

#endif /* _VMS_INTERNAL_H */
