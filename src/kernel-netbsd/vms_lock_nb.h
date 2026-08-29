/*
 * vms_lock_nb.h - the shared /dev/vms LOCK MANAGER (DLM) contract for the
 * OVMX/NetBSD substrate (rd vms-ff7, parent vms-8e8; docs/design-ovmx-netbsd-
 * syskrnl.md, docs/design-netbsd-executive-core.md).
 *
 * P4-A (vms-ff7, the LAST NetBSD executive backend) compiles the SAME lock-
 * manager facility source -- src/kernel-core/vms_lock.c -- into the NetBSD `vms'
 * pseudo-device that it compiles into the Linux vms.ko, exactly as P2c did for
 * event flags (vms_eflag_nb.h), P4-A for ASTs/access (vms_ast_nb.h/
 * vms_access_nb.h), mailboxes (vms_mbx_nb.h) and the process table
 * (vms_proctab_nb.h). The lock manager holds its resource database, its lock-ID
 * red-black tree and every lock's granted/waiting queues in module-global KERNEL
 * memory, so a lock one process $ENQs on a named resource blocks a DIFFERENT
 * process that $ENQs an incompatible mode on the same resource -- the INV-6-
 * decisive property (CLAUDE.md Rule 9). A per-process userspace fake could report
 * a grant while sharing nothing; this cannot, because there is exactly one
 * resource block and it lives in the kernel.
 *
 * This header is the ONE wire contract for that facility on NetBSD, included
 * IDENTICALLY by every side of the /dev/vms boundary:
 *   - the in-kernel `vms' pseudo-device      (src/kernel-netbsd/vms_netbsd.c),
 *   - the shared facility itself, via the NetBSD struct twin
 *     (src/kernel-netbsd/vms_internal.h, which includes this file for the arg
 *     structs the facility copies in/out and the LCK_* mode/flag constants), and
 *   - the userspace test program that reaches it through the transport seam.
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument STRUCTS and the LCK_*
 * constants below are the SAME the Linux executive's ioctl surface uses
 * (src/kernel/vms_ioctl.h) -- byte-for-byte identical layouts (the
 * _Static_asserts here freeze them) -- because ONE facility source
 * (src/kernel-core/vms_lock.c) copies them in and out on BOTH substrates.
 *
 * THE COPY MODEL. All five lock ioctls are _IOWR and small (<= 104 bytes, well
 * under NetBSD's one-page IOCPARM_MAX), so they ride the cdevsw framework's
 * pre-copy path: NetBSD copies the caller's argument into a kernel buffer, hands
 * it to the driver, and copies the answer back out. The driver passes that buffer
 * straight to the facility, whose exec_copyin/exec_copyout are in-kernel copies on
 * this backend (the vms_eflag_nb.h COPY MODEL note). None of them carries an
 * inline page-sized buffer, so -- unlike the mailbox WRITE/READ transfer ops --
 * none needs the IOC_VOID big-io shape; the request numbers are byte-identical to
 * Linux (magic 'V', NR 0x30-0x34).
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The lock SEMANTICS are the publicly documented
 * OpenVMS $ENQ/$DEQ/$GETLKI distributed-lock-manager behaviour (VSI OpenVMS
 * System Services Reference; the DLM directory/mastering model from the IDSM
 * lock-management chapter); the argument layouts are OVMX's own, shared with the
 * Linux executive (src/kernel/vms_ioctl.h). No NetBSD or VSI source is copied.
 */

#ifndef _VMS_LOCK_NB_H
#define _VMS_LOCK_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_ping.h / vms_eflag_nb.h / vms_mbx_nb.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IOWR macro (identical dance to vms_mbx_nb.h). All
 * five lock ioctls are _IOWR (framework pre-copy path). */
#if !defined(_IOWR)
# if defined(__NetBSD__)
#  if defined(_KERNEL)
#   include <sys/ioccom.h>
#  else
#   include <sys/ioctl.h>
#  endif
# else
#  define _IOWR(type, nr, size) \
        (((3U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
# endif
#endif

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V'), vms_ping.h and
 * the other _nb.h contracts: one /dev/vms contract, one magic space. */
#define VMS_LOCK_IOC_MAGIC 'V'

/* ================================================================
 * Lock modes and $ENQ flags -- byte-identical to src/kernel/vms_ioctl.h. The
 * shared facility (src/kernel-core/vms_lock.c) tests exactly these. Guarded so
 * this header composes with any other /dev/vms contract header on a build that
 * also pulls vms_ioctl.h (userspace).
 * ================================================================ */

/* VMS lock modes (6-mode compatibility). */
#ifndef LCK_K_NLMODE
#define LCK_K_NLMODE    0   /* Null */
#define LCK_K_CRMODE    1   /* Concurrent Read */
#define LCK_K_CWMODE    2   /* Concurrent Write */
#define LCK_K_PRMODE    3   /* Protected Read */
#define LCK_K_PWMODE    4   /* Protected Write */
#define LCK_K_EXMODE    5   /* Exclusive */
#endif

/* $ENQ flags. LCK_M_SYNC is an OVMX design choice (not a real $LCKDEF bit):
 * request that the kernel ENQ/CONVERT ioctl BLOCK in-kernel until granted (or a
 * deadlock is detected), how sys$enqw's synchronous wait is realized without a
 * userspace poll. Byte-identical to src/kernel/vms_ioctl.h. */
#ifndef LCK_M_CONVERT
#define LCK_M_CONVERT   0x01   /* Convert existing lock */
#define LCK_M_NOQUEUE   0x02   /* Don't queue if not granted */
#define LCK_M_SYSTEM    0x04   /* System-wide resource */
#define LCK_M_VALBLK    0x08   /* Lock has value block */
#define LCK_M_SYNC      0x10   /* Block in-kernel until granted (sync ENQ) */
#endif

/* Lock value block size (bytes). Byte-identical to src/kernel/vms_ioctl.h. */
#ifndef LCK_VALBLK_SIZE
#define LCK_VALBLK_SIZE 16
#endif

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_ioctl.h. The shared
 * facility (src/kernel-core/vms_lock.c) copies exactly these in and out.
 * ================================================================ */

struct vms_enq_args {
	uint32_t efn;               /* in: event flag for completion */
	uint32_t lkmode;            /* in: requested lock mode (0-5) */
	uint32_t flags;             /* in: LCK_M_* flags */
	uint32_t parid;             /* in: parent lock ID (0 for root) */
	char     resnam[32];        /* in: resource name (null-terminated) */
	uint64_t astadr;            /* in: completion AST address */
	uint64_t astprm;            /* in: AST parameter */
	uint64_t blkastadr;         /* in: blocking AST address */
	uint32_t lkid;              /* return: lock ID (or input for convert) */
	uint32_t lk_status;         /* return: lock status (granted mode in LKSB) */
	uint8_t  valblk[LCK_VALBLK_SIZE]; /* in/out: lock value block */
	uint32_t status;            /* return: SS$_ status */
	uint32_t owner_csid;        /* in: cluster CSID that OWNS this lock; 0 = the
	                             * local node. Cross-node DLM dispatch sets it to
	                             * the remote requester's CSID (vms-e8f1). Was a
	                             * reserved pad -- same size, no ABI change. */
};

struct vms_deq_args {
	uint32_t lkid;              /* in: lock ID to dequeue */
	uint8_t  valblk[LCK_VALBLK_SIZE]; /* in: value block to write back */
	uint32_t flags;             /* in: dequeue flags */
	uint32_t status;            /* return: SS$_ status */
};

struct vms_getlki_args {
	uint32_t lkid;              /* in: lock ID to query */
	uint32_t granted_mode;      /* return: current granted mode */
	uint32_t requested_mode;    /* return: requested mode (if waiting) */
	uint32_t parent_id;         /* return: parent lock ID */
	char     resnam[32];        /* return: resource name */
	uint8_t  valblk[LCK_VALBLK_SIZE]; /* return: value block */
	uint32_t status;            /* return: SS$_ status */
	uint32_t pad;
};

struct vms_resmaster_args {
	char     resnam[32];        /* in: resource name (null-terminated) */
	uint32_t found;             /* return: 1 if a resource block exists */
	uint32_t local_csid;        /* return: this node's CSID */
	uint32_t dir_csid;          /* return: directory node CSID for resnam */
	uint32_t master_csid;       /* return: mastering node CSID; 0 = unmastered */
	uint32_t is_local_master;   /* return: 1 if mastered by this node */
	uint32_t n_granted;         /* return: granted locks on the resource */
	uint32_t status;            /* return: SS$_ status */
	uint32_t remote_holder_csid;/* return: CSID a remote-held granted lock is held
	                             * FOR; 0 if all grants are local (vms-e8f1). Was a
	                             * reserved pad -- same size, no ABI change. */
};

/*
 * vms-94c (DLM epic vms-7fa rung 1): the cross-node DLM RECEIVE dispatch, mirror
 * of src/kernel/vms_ioctl.h -- byte-identical, because vms_lock.c is SHARED
 * kernel-core (Linux vms.ko AND NetBSD vms.kmod), and vms_lock_dlm_xnode_dispatch
 * references struct vms_dlm_xnode_args and the VMS_DLM_OP_* op values here. The
 * VMS_DLM_OP_* values MUST equal scs_dlm.h's SCS_DLM_OP_* (scsd.c static-asserts
 * that on the Linux/userspace side). See vms_ioctl.h for the full provenance note.
 */
#define VMS_DLM_OP_ENQ      1u   /* lock/convert request  -> master  */
#define VMS_DLM_OP_GRANT    2u   /* status response       <- master  */
#define VMS_DLM_OP_DEQ      3u   /* dequeue request       -> master  */
#define VMS_DLM_OP_BLKAST   4u   /* blocking-AST notify   <- master  */

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
	uint32_t status;            /* return: SS$_ status. granted=SS$_NORMAL;
	                             * queued=VMS_DLM_STS_QUEUED (0); NOQUEUE decline=
	                             * SS$_NOTQUEUED; response ops=SS$_UNSUPPORTED. */
	uint32_t queued;            /* return: 1 = queued on the master (blocked) */
	uint32_t blocking_csid;     /* return: cross-node holder to BLKAST (0 = none) */
	uint32_t blocking_master_lkid; /* return: that holder's master lock handle */
	/* BLKAST WIRE (DLM epic vms-7fa rung H6, vms-76d) -- mirror of src/kernel/
	 * vms_ioctl.h. blocking_req_lkid: the blocking holder's REQUESTER-side lock
	 * handle (BLKAST target names the holder's ORIGIN record). blkastadr/blkastprm:
	 * in(GRANT receive) the holder's blocking-AST routine/param, remembered on the
	 * origin record. blkast_delivered: return(BLKAST receive) 1 iff a real user-mode
	 * blocking AST was queued to the holder proc (INV-6: never a faked AST). */
	uint32_t blocking_req_lkid;
	uint64_t blkastadr;
	uint64_t blkastprm;
	uint32_t blkast_delivered;
	uint32_t pad_blkast;
};

/*
 * The `status` an ENQ dispatch returns when the request was QUEUED (blocked) on
 * the master rather than granted (contention rung vms-904c) -- 0, the VMS
 * lock-status-block "no completion posted" state; NOT SS$_NORMAL, NOT
 * SS$_NOTQUEUED. Mirrors src/kernel/vms_ioctl.h.
 */
#define VMS_DLM_STS_QUEUED  0u

/* $DLM member departure (rd vms-2bf, DLM rung H10a). MUST match src/kernel/
 * vms_ioctl.h byte-for-byte. See there for the semantics + INV-6 contract:
 * scsd reports a graceful departure, the executive shrinks live membership and
 * re-resolves the directory over the survivors. */
struct vms_dlm_depart_args {
	uint32_t departed_csid;   /* in: the CSID that left the cluster */
	uint32_t members_live;    /* return: live directory-member count after shrink */
	uint32_t found;           /* return: 1 iff departed_csid was a configured member */
	uint32_t status;          /* return: SS$_ status */
};

/* $DLM granted-lock readback (rd vms-dca9, DLM rung H10b). MUST match
 * src/kernel/vms_ioctl.h byte-for-byte -- the value-verify readback for a
 * rebuilt cross-node lock (holder CSID + its own handle + granted mode). */
struct vms_dlm_granted_args {
	char     resnam[32];        /* in: resource name */
	uint32_t found;             /* return: 1 iff a REMOTE-held granted lock exists */
	uint32_t n_granted;         /* return: total granted locks on the resource */
	uint32_t holder_csid;       /* return: first remote holder's CSID (req_csid) */
	uint32_t holder_req_lkid;   /* return: that holder's own lock handle (req_lkid) */
	uint32_t granted_mode;      /* return: that lock's granted mode */
	uint32_t status;            /* return: SS$_ status */
};

/* ================================================================
 * Request numbers. All five are _IOWR carrying the SAME structs and NR bytes as
 * src/kernel/vms_ioctl.h (0x30-0x34, magic 'V'), so their command words are
 * identical across substrates (framework pre-copy path; none exceeds one page).
 * VMS_IOCTL_CONVERT reuses struct vms_enq_args, exactly as on Linux.
 * ================================================================ */
#define VMS_IOCTL_ENQ           _IOWR(VMS_LOCK_IOC_MAGIC, 0x30, struct vms_enq_args)
#define VMS_IOCTL_DEQ           _IOWR(VMS_LOCK_IOC_MAGIC, 0x31, struct vms_deq_args)
#define VMS_IOCTL_CONVERT       _IOWR(VMS_LOCK_IOC_MAGIC, 0x32, struct vms_enq_args)
#define VMS_IOCTL_GETLKI        _IOWR(VMS_LOCK_IOC_MAGIC, 0x33, struct vms_getlki_args)
#define VMS_IOCTL_GET_RESMASTER _IOWR(VMS_LOCK_IOC_MAGIC, 0x34, struct vms_resmaster_args)
#define VMS_IOCTL_DLM_XNODE     _IOWR(VMS_LOCK_IOC_MAGIC, 0x35, struct vms_dlm_xnode_args)
#define VMS_IOCTL_DLM_MEMBER_DEPART _IOWR(VMS_LOCK_IOC_MAGIC, 0x36, struct vms_dlm_depart_args)
#define VMS_IOCTL_DLM_GET_GRANTED   _IOWR(VMS_LOCK_IOC_MAGIC, 0x37, struct vms_dlm_granted_args)

/*
 * Freeze the shared layouts -- see the other _nb.h contracts' identical asserts:
 * both sides of /dev/vms compile these structs separately and pass them by raw
 * address, so a size drift is an ABI break. These MUST match src/kernel/
 * vms_ioctl.h exactly (measured on amd64/LP64).
 */
_Static_assert(sizeof(struct vms_enq_args) == 104,
               "vms_enq_args changed size -- VMS_IOCTL_ENQ/CONVERT ABI break");
_Static_assert(sizeof(struct vms_deq_args) == 28,
               "vms_deq_args changed size -- VMS_IOCTL_DEQ ABI break");
_Static_assert(sizeof(struct vms_getlki_args) == 72,
               "vms_getlki_args changed size -- VMS_IOCTL_GETLKI ABI break");
_Static_assert(sizeof(struct vms_resmaster_args) == 64,
               "vms_resmaster_args changed size -- VMS_IOCTL_GET_RESMASTER ABI break");
_Static_assert(sizeof(struct vms_dlm_xnode_args) == 120,
               "vms_dlm_xnode_args changed size -- VMS_IOCTL_DLM_XNODE ABI break");
_Static_assert(sizeof(struct vms_dlm_depart_args) == 16,
               "vms_dlm_depart_args changed size -- VMS_IOCTL_DLM_MEMBER_DEPART ABI break");
_Static_assert(sizeof(struct vms_dlm_granted_args) == 56,
               "vms_dlm_granted_args changed size -- VMS_IOCTL_DLM_GET_GRANTED ABI break");

#endif /* _VMS_LOCK_NB_H */
