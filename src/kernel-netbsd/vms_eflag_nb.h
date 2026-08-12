/*
 * vms_eflag_nb.h - the shared /dev/vms EVENT-FLAG contract for the OVMX/NetBSD
 * substrate (rd vms-4b4, parent vms-dd8, epic vms-8e8;
 * docs/design-ovmx-netbsd-syskrnl.md, docs/design-netbsd-executive-core.md).
 *
 * P2c is the phase that PROVES the executive is REAL on NetBSD by compiling the
 * SAME event-flag facility source -- src/kernel-core/vms_eflag.c -- into the
 * NetBSD `vms' pseudo-device that it compiles into the Linux vms.ko, and showing
 * it holds SYSTEM-WIDE SHARED state in kernel memory (a flag a COMMON event flag
 * cluster carries is visible across processes -- the INV-6-decisive property,
 * CLAUDE.md Rule 9).
 *
 * This header is the ONE wire contract for that facility on NetBSD, included
 * IDENTICALLY by every side of the /dev/vms boundary:
 *   - the in-kernel `vms' pseudo-device      (src/kernel-netbsd/vms_netbsd.c),
 *   - the shared facility itself, via the NetBSD struct header
 *     (src/kernel-netbsd/vms_internal.h, which includes this file for the arg
 *     structs the facility copies in/out), and
 *   - the userspace test program that reaches it through the transport seam
 *     (tests/netbsd/guest/vmseflag.c via src/libvmssys/kif_transport_netbsd.c).
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument structs below are the SAME
 * structs the Linux executive's ioctl surface uses (src/kernel/vms_ioctl.h:
 * struct vms_ef_args / vms_ef_wait_args / vms_ef_read_args / vms_ef_common_args)
 * -- byte-for-byte identical layouts -- because ONE facility source
 * (src/kernel-core/vms_eflag.c) copies them in and out on BOTH substrates. And
 * because the encoding below is _IOWR with those same structs and the same NR
 * bytes as vms_ioctl.h, the request NUMBERS are identical across substrates too.
 *
 * THE COPY MODEL: _IOWR + the framework owns the user boundary. On Linux the
 * module owns its copyin/copyout on the raw user pointer. On NetBSD the generic
 * cdevsw ioctl path PRE-COPIES an _IOWR argument into a kernel buffer, hands the
 * driver that buffer, and copies the driver's answer back out. The driver passes
 * that kernel buffer straight to the shared facility; the facility's
 * exec_copyin/exec_copyout are, on the NetBSD backend, in-kernel copies between
 * that framework buffer and the facility's locals (the ONE real user boundary
 * crossing is the framework's, at the syscall edge). This is the honest,
 * idiomatic NetBSD integration -- it does not fight the cdevsw ABI with an
 * IOC_VOID raw-pointer trick -- and it keeps the shared facility source
 * unchanged. See exec_kbackend_netbsd.h's COPY MODEL note for the detail.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The event-flag SEMANTICS are the publicly
 * documented OpenVMS $SETEF/$CLREF/$WAITFR/$READEF/$ASCEFC/$DACEFC behaviour;
 * the argument layouts are OVMX's own, shared with the Linux executive. No
 * NetBSD or VSI source is copied.
 */

#ifndef _VMS_EFLAG_NB_H
#define _VMS_EFLAG_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_ping.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros (identical dance to vms_ping.h). On
 * NetBSD they come from <sys/ioccom.h> (kernel) or <sys/ioctl.h> (userspace);
 * the fallback matches vms_ioctl.h's Linux-style encoding, which for structs
 * this small yields the identical request number. */
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

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V') and vms_ping.h:
 * one /dev/vms contract, one magic space. */
#define VMS_EFLAG_IOC_MAGIC 'V'

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_ioctl.h. The shared
 * facility (src/kernel-core/vms_eflag.c) copies exactly these in and out.
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

/* ================================================================
 * Request numbers -- _IOWR carrying the SAME structs and NR bytes as
 * src/kernel/vms_ioctl.h, so the numbers are IDENTICAL across substrates. The
 * struct each command carries is exactly the one the shared facility copies for
 * that command (so the framework's pre-copy size matches the facility's copy).
 * ================================================================ */
#define VMS_IOCTL_SETEF   _IOWR(VMS_EFLAG_IOC_MAGIC, 0x20, struct vms_ef_args)
#define VMS_IOCTL_CLREF   _IOWR(VMS_EFLAG_IOC_MAGIC, 0x21, struct vms_ef_args)
#define VMS_IOCTL_WAITFR  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x22, struct vms_ef_args)
#define VMS_IOCTL_WFLOR   _IOWR(VMS_EFLAG_IOC_MAGIC, 0x23, struct vms_ef_wait_args)
#define VMS_IOCTL_WFLAND  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x24, struct vms_ef_wait_args)
#define VMS_IOCTL_READEF  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x25, struct vms_ef_read_args)
#define VMS_IOCTL_ASCEFC  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x26, struct vms_ef_common_args)
#define VMS_IOCTL_DACEFC  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x27, struct vms_ef_args)
#define VMS_IOCTL_DLCEFC  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x28, struct vms_ef_common_args)

/*
 * Common event flag cluster range (OpenVMS): cluster 2 = EFN 64..95, cluster 3
 * = EFN 96..127 -- the SYSTEM-WIDE SHARED flags whose cross-process visibility
 * is the P2c proof. A process must $ASCEFC a named common cluster before it can
 * $SETEF/$READEF a flag in it (efn_resolve in the facility returns SS$_UNASEFC
 * otherwise) -- exactly as OpenVMS requires, and NOT a per-process fake.
 */
#define VMS_EF_COMMON_LO   64u
#define VMS_EF_COMMON_HI  127u

/*
 * Well-known PERMANENT common-cluster names the P2c test associates by. Marking
 * the cluster PERMANENT ($ASCEFC with perm=1) is what makes its flag state
 * outlive the process that set it -- the executive keeps the cluster in kernel
 * memory after the first process exits, so a LATER, DIFFERENT process that
 * associates the same name observes the earlier set. That persistence across
 * distinct processes IS the INV-6 proof.
 */
#define VMS_P2C_CLUSTER2_NAME "OVMX_P2C_CLUSTER2"
#define VMS_P2C_CLUSTER3_NAME "OVMX_P2C_CLUSTER3"

/*
 * VMS status codes used by this contract. Grounded to the in-tree source of
 * truth src/libvms/include/ssdef.h and src/kernel/vms_internal.h (CLAUDE.md
 * Rule 4). Guarded so this header composes with vms_ping.h in a TU that includes
 * both (the driver and tool include both).
 */
#ifndef VMS_SS_NORMAL
#define VMS_SS_NORMAL   1u      /* SS$_NORMAL */
#endif
#ifndef VMS_SS_WASCLR
#define VMS_SS_WASCLR   1u      /* SS$_WASCLR (== SS$_NORMAL on VMS) */
#endif
#ifndef VMS_SS_WASSET
#define VMS_SS_WASSET   9u      /* SS$_WASSET */
#endif
#ifndef VMS_SS_ILLEFC
#define VMS_SS_ILLEFC   236u    /* SS$_ILLEFC */
#endif
#ifndef VMS_SS_UNASEFC
#define VMS_SS_UNASEFC  564u    /* SS$_UNASEFC */
#endif
#ifndef VMS_SS_NOSUCHDEV
#define VMS_SS_NOSUCHDEV 2680u  /* SS$_NOSUCHDEV -- the honest "no /dev/vms" verdict */
#endif

#endif /* _VMS_EFLAG_NB_H */
