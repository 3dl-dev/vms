/*
 * vms_mbx_nb.h - the shared /dev/vms MAILBOX (MBAn:) contract for the
 * OVMX/NetBSD substrate (rd vms-d7a, parent vms-f8a, epic vms-8e8;
 * docs/design-ovmx-netbsd-syskrnl.md, docs/design-netbsd-executive-core.md).
 *
 * P4-A (vms-d7a) compiles the SAME mailbox facility source --
 * src/kernel-core/vms_mbx.c -- into the NetBSD `vms' pseudo-device that it
 * compiles into the Linux vms.ko, exactly as P2c did for event flags
 * (vms_eflag_nb.h). A mailbox is a device one process creates and another
 * opens; the facility holds the mailbox table, its message queues and its
 * buffer-quota accounting in module-global KERNEL memory, so a message one
 * process $QIO-writes to MBAn: is readable by a DIFFERENT process that
 * $ASSIGNed the same MBAn: -- the INV-6-decisive property (CLAUDE.md Rule 9).
 *
 * This header is the ONE wire contract for that facility on NetBSD, included
 * IDENTICALLY by every side of the /dev/vms boundary:
 *   - the in-kernel `vms' pseudo-device      (src/kernel-netbsd/vms_netbsd.c),
 *   - the shared facility itself, via the NetBSD struct twin
 *     (src/kernel-netbsd/vms_internal.h, which includes this file for the arg
 *     structs the facility copies in/out and for VMS_DEVNAM_SIZE / the sizing
 *     constants it clamps to), and
 *   - the userspace test program that reaches it through the transport seam.
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument STRUCTS below are the SAME
 * structs the Linux executive's ioctl surface uses (src/kernel/vms_mbx.h) --
 * byte-for-byte identical layouts (the _Static_asserts here freeze them) --
 * because ONE facility source (src/kernel-core/vms_mbx.c) copies them in and out
 * on BOTH substrates. The wire STRUCT is identical; only the ioctl-number
 * ENCODING of the two large ops differs on NetBSD (see below), which is a
 * substrate transport detail, not feature drift (Linux _IOWR and the NetBSD
 * encoding already differ inherently).
 *
 * THE COPY MODEL, TWO SHAPES ON NetBSD.
 *   - CONTROL ops (create/assign/delmbx/set_wrtattn, all <= 40 bytes) are _IOWR:
 *     the cdevsw framework pre-copies the argument into a kernel buffer, hands it
 *     to the driver, and copies the answer back out. The driver passes that
 *     buffer straight to the facility, whose exec_copyin/exec_copyout are
 *     in-kernel copies on this backend (the vms_eflag_nb.h COPY MODEL note).
 *   - MESSAGE-TRANSFER ops (WRITE/READ) CANNOT use that path: NetBSD's generic
 *     cdevsw ioctl caps an argument at IOCPARM_MAX == NBPG == one page (4096 on
 *     amd64) -- sys/kern/sys_generic.c rejects `size > IOCPARM_MAX' with ENOTTY
 *     BEFORE d_ioctl is entered (sys/sys/ioccom.h: `#define IOCPARM_MAX NBPG').
 *     vms_mbx_write_args (4112) and vms_mbx_read_args (4116) each carry an inline
 *     VMS_MBX_IOCTL_MAXLEN(4096)-byte buffer, so they exceed one page. Per the
 *     orchestrator's ruling (Option B), these two are encoded IOC_VOID on NetBSD:
 *     the framework then passes the raw user pointer through (sys_generic.c:
 *     `*(void **)data = SCARG(uap, data)'), and the DRIVER (vms_netbsd.c's
 *     d_ioctl) does the copyin into a kernel buffer / copyout of the answer
 *     itself, handing that kernel buffer to the SAME facility (so its exec_copyin
 *     stays an in-kernel copy). The SHARED VMS_MBX_IOCTL_MAXLEN and the Linux ABI
 *     / frozen asserts are UNTOUCHED. This does not fabricate anything (INV-6): a
 *     real copy of real caller data occurs; a bad user address is rejected by the
 *     driver's copyin.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The mailbox SEMANTICS are the publicly
 * documented OpenVMS $CREMBX/$DELMBX/$ASSIGN/$QIO(mailbox) behaviour; the
 * argument layouts are OVMX's own, shared with the Linux executive
 * (src/kernel/vms_mbx.h). No NetBSD or VSI source is copied.
 */

#ifndef _VMS_MBX_NB_H
#define _VMS_MBX_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_ping.h / vms_eflag_nb.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros (identical dance to vms_eflag_nb.h).
 * Both _IOWR (the four control ioctls) and _IO (the IOC_VOID WRITE/READ, see the
 * COPY MODEL note above) are needed. */
#if !defined(_IOWR) || !defined(_IO)
# if defined(__NetBSD__)
#  if defined(_KERNEL)
#   include <sys/ioccom.h>
#  else
#   include <sys/ioctl.h>
#  endif
# else
#  ifndef _IOWR
#   define _IOWR(type, nr, size) \
        (((3U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#  endif
#  ifndef _IO
#   define _IO(type, nr) \
        (((0U) << 30) | ((type) << 8) | (nr))
#  endif
# endif
#endif

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V'), vms_ping.h and
 * vms_eflag_nb.h: one /dev/vms contract, one magic space. */
#define VMS_MBX_IOC_MAGIC 'V'

/* Device-name field width -- matches src/kernel/vms_ioctl.h's VMS_DEVNAM_SIZE
 * (the shared value both substrates and userspace compile). Guarded so this
 * header composes with any other /dev/vms contract header that also defines it. */
#ifndef VMS_DEVNAM_SIZE
#define VMS_DEVNAM_SIZE 16
#endif

/* A single ioctl transfer, in either direction. OVMX design cap (NOT a VMS
 * value), byte-identical to src/kernel/vms_mbx.h. See the IOCPARM_MAX note in
 * this file's header: this 4096 is exactly what pushes the write/read arg
 * structs past NetBSD's one-page ioctl limit. */
#define VMS_MBX_IOCTL_MAXLEN   4096u

/* OVMX defaults used when the caller passes 0 for MAXMSG/BUFQUO (design choice
 * standing in for VMS's site-tunable DEFMBXMXMSG/DEFMBXBUFQUO). Both clamped to
 * VMS_MBX_IOCTL_MAXLEN by the facility. Byte-identical to src/kernel/vms_mbx.h. */
#define VMS_MBX_DEFAULT_MAXMSG 1024u
#define VMS_MBX_DEFAULT_BUFQUO 4096u

/* Read modifier carried in vms_mbx_read_args.flags: IO$M_NOW (a read of an empty
 * mailbox completes at once with SS$_ENDOFFILE rather than blocking -- public
 * VSI OpenVMS I/O User's Reference, Mailbox Driver). */
#define VMS_MBX_READ_NOW  0x00000001u

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_mbx.h. The shared
 * facility (src/kernel-core/vms_mbx.c) copies exactly these in and out.
 * ================================================================ */

struct vms_mbx_create_args {
	uint32_t permanent;     /* in: 1 = permanent (PRMMBX), 0 = temporary (TMPMBX) */
	uint32_t maxmsg;        /* in: 0 => VMS_MBX_DEFAULT_MAXMSG */
	uint32_t bufquo;        /* in: 0 => VMS_MBX_DEFAULT_BUFQUO */
	uint32_t chan;          /* out: this process's channel number */
	uint32_t unit;          /* out: MBAn: unit number */
	uint32_t status;        /* out: SS$_ status */
	char     devnam[VMS_DEVNAM_SIZE]; /* out: "MBAn:" */
};

struct vms_mbx_assign_args {
	char     devnam[VMS_DEVNAM_SIZE]; /* in */
	uint32_t chan;                    /* out */
	uint32_t status;                  /* out */
};

struct vms_mbx_delmbx_args {
	uint32_t chan;      /* in: any channel assigned to the mailbox */
	uint32_t status;    /* out */
};

struct vms_mbx_write_args {
	uint32_t chan;      /* in */
	uint32_t len;       /* in: bytes in data (<= VMS_MBX_IOCTL_MAXLEN) */
	uint32_t status;    /* out */
	uint32_t pad;
	char     data[VMS_MBX_IOCTL_MAXLEN];
};

struct vms_mbx_read_args {
	uint32_t chan;      /* in */
	uint32_t bufsz;     /* in: caller's buffer size (<= VMS_MBX_IOCTL_MAXLEN) */
	uint32_t flags;     /* in: VMS_MBX_READ_* modifiers (IO$M_NOW) */
	uint32_t len;       /* out: actual message length */
	uint32_t status;    /* out */
	char     data[VMS_MBX_IOCTL_MAXLEN];
};

struct vms_mbx_wrtattn_args {
	uint32_t chan;      /* in: this process's channel to the mailbox */
	uint32_t acmode;    /* in: access mode to deliver the AST at (0-3) */
	uint64_t astadr;    /* in: write-attention AST routine (opaque address) */
	uint64_t astprm;    /* in: parameter passed to the AST routine */
	uint32_t status;    /* out: SS$_ status */
	uint32_t pad;
};

/* ================================================================
 * Request numbers. The four CONTROL ops are _IOWR carrying the SAME structs and
 * NR bytes as src/kernel/vms_mbx.h, so their numbers are identical across
 * substrates (framework pre-copy path). The two MESSAGE-TRANSFER ops (WRITE 0x72,
 * READ 0x73) exceed NetBSD's one-page IOCPARM_MAX, so per the Option B ruling
 * they are encoded IOC_VOID (_IO, no size) on NetBSD and the driver does the
 * copyin/copyout itself -- see the COPY MODEL note at the top. The NR bytes match
 * Linux (0x72/0x73); only the direction/size fields of the command word differ,
 * which is the substrate transport detail the ruling accepts. The wire STRUCTS
 * (frozen by the _Static_asserts below) are unchanged.
 * ================================================================ */
#define VMS_IOCTL_MBX_CREATE      _IOWR(VMS_MBX_IOC_MAGIC, 0x70, struct vms_mbx_create_args)
#define VMS_IOCTL_MBX_ASSIGN      _IOWR(VMS_MBX_IOC_MAGIC, 0x71, struct vms_mbx_assign_args)
#define VMS_IOCTL_MBX_WRITE       _IO(VMS_MBX_IOC_MAGIC, 0x72)   /* IOC_VOID: driver copyin/out struct vms_mbx_write_args */
#define VMS_IOCTL_MBX_READ        _IO(VMS_MBX_IOC_MAGIC, 0x73)   /* IOC_VOID: driver copyin/out struct vms_mbx_read_args */
#define VMS_IOCTL_MBX_DELMBX      _IOWR(VMS_MBX_IOC_MAGIC, 0x74, struct vms_mbx_delmbx_args)
#define VMS_IOCTL_MBX_SET_WRTATTN _IOWR(VMS_MBX_IOC_MAGIC, 0x75, struct vms_mbx_wrtattn_args)

/*
 * Freeze the shared layouts -- see src/kernel/vms_mbx.h's identical asserts:
 * both sides of /dev/vms compile these structs separately and pass them by raw
 * address, so a size drift is an ABI break. These MUST match vms_mbx.h exactly.
 */
_Static_assert(sizeof(struct vms_mbx_create_args) == 40,
               "vms_mbx_create_args changed size -- VMS_IOCTL_MBX_CREATE ABI break");
_Static_assert(sizeof(struct vms_mbx_assign_args) == 24,
               "vms_mbx_assign_args changed size -- VMS_IOCTL_MBX_ASSIGN ABI break");
_Static_assert(sizeof(struct vms_mbx_delmbx_args) == 8,
               "vms_mbx_delmbx_args changed size -- VMS_IOCTL_MBX_DELMBX ABI break");
_Static_assert(sizeof(struct vms_mbx_write_args) == 16 + VMS_MBX_IOCTL_MAXLEN,
               "vms_mbx_write_args changed size -- VMS_IOCTL_MBX_WRITE ABI break");
_Static_assert(sizeof(struct vms_mbx_read_args) == 20 + VMS_MBX_IOCTL_MAXLEN,
               "vms_mbx_read_args changed size -- VMS_IOCTL_MBX_READ ABI break");
_Static_assert(sizeof(struct vms_mbx_wrtattn_args) == 32,
               "vms_mbx_wrtattn_args changed size -- VMS_IOCTL_MBX_SET_WRTATTN ABI break");

#endif /* _VMS_MBX_NB_H */
