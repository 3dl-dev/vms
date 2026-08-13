/*
 * vms_ast_nb.h - the shared /dev/vms AST-DELIVERY contract for the OVMX/NetBSD
 * substrate (rd vms-9bb, epic vms-8e8; docs/design-netbsd-executive-core.md).
 *
 * P4-A compiles the SAME AST facility source -- src/kernel-core/vms_ast.c --
 * into the NetBSD `vms' pseudo-device that it compiles into the Linux vms.ko,
 * exactly as vms_eflag_nb.h did for the event-flag facility (P2c). This header
 * is the ONE wire contract for $DCLAST/$SETAST/DELIVERAST on NetBSD, included
 * IDENTICALLY by every side of the /dev/vms boundary:
 *   - the in-kernel `vms' pseudo-device      (src/kernel-netbsd/vms_netbsd.c),
 *   - the shared facility, via the NetBSD struct header
 *     (src/kernel-netbsd/vms_internal.h, which includes this file for the arg
 *     structs the facility copies in/out), and
 *   - the userspace test program that reaches it through the transport seam.
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument structs below are the SAME
 * structs the Linux executive's ioctl surface uses (src/kernel/vms_ioctl.h:
 * struct vms_ast_args / vms_setast_args) -- byte-for-byte identical layouts --
 * because ONE facility source (src/kernel-core/vms_ast.c) copies them in and out
 * on BOTH substrates.
 *
 * DIRECTION BITS ARE SUBSTRATE-NATIVE, NR BYTES ARE IDENTICAL. Like vms_eflag_nb.h,
 * this header PREFERS the substrate's own _IO* macros. That matters here in a way
 * it did not for event flags: the AST commands are NOT all _IOWR. $SETAST is
 * _IOWR (in+out), but $DCLAST is _IOW (in-only) and DELIVERAST is _IOR (out-only),
 * and on NetBSD the generic cdevsw ioctl path reads the DIRECTION bits to decide
 * whether to copyin before / copyout after the driver. The NR byte (0x10/0x11/0x12),
 * the carried struct and the magic byte are byte-identical to vms_ioctl.h, so the
 * command IDENTITY is identical across substrates; only the 2 direction bits follow
 * each substrate's ABI (Linux IOC_WRITE==bit30 vs NetBSD IOC_IN==bit31), which is
 * required for the NetBSD framework to move the bytes at all. For _IOWR that
 * yields the identical request NUMBER too; for _IOW/_IOR the number differs by the
 * direction bits by design -- the userspace tool and the module both derive it from
 * THIS header, so they always agree.
 *
 * THE _IOW STATUS CAVEAT (honest, not a fake). Because $DCLAST is _IOW (IOC_IN
 * only), the framework copies the caller's request IN but does NOT copy the
 * facility's answer OUT -- so the args.status the facility writes is not returned
 * to userspace on NetBSD (the driver never sees the user address, so it cannot
 * copy it out itself). This is a property of the shared request number, not a
 * fabrication: nothing invents a status. A $DCLAST's EFFECT is observable through
 * DELIVERAST (which IS _IOR and returns the queued entry), so the facility is
 * fully testable; only $DCLAST's immediate status longword is not conveyed. Same
 * for $SETMODE in vms_access_nb.h. See vms_netbsd.c's dispatch comment.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The AST SEMANTICS are the publicly documented
 * OpenVMS $DCLAST/$SETAST behaviour; the argument layouts are OVMX's own, shared
 * with the Linux executive. No NetBSD or VSI source is copied.
 */

#ifndef _VMS_AST_NB_H
#define _VMS_AST_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_ping.h / vms_eflag_nb.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros (identical dance to vms_eflag_nb.h). On
 * NetBSD they come from <sys/ioccom.h> (kernel) or <sys/ioctl.h> (userspace);
 * the fallback matches vms_ioctl.h's Linux-style encoding. */
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
#  define _IOW(type, nr, size) \
        (((1U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
#  define _IOR(type, nr, size) \
        (((2U) << 30) | ((sizeof(size)) << 16) | ((type) << 8) | (nr))
# endif
#endif

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V'): one /dev/vms
 * contract, one magic space. */
#define VMS_AST_IOC_MAGIC 'V'

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_ioctl.h. The shared
 * facility (src/kernel-core/vms_ast.c) copies exactly these in and out.
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

/* ================================================================
 * Request numbers -- SAME NR bytes, structs and magic as src/kernel/vms_ioctl.h;
 * the direction class ($DCLAST=_IOW, $SETAST=_IOWR, DELIVERAST=_IOR) also matches
 * vms_ioctl.h, realized through the substrate's own macros so the NetBSD cdevsw
 * framework moves the bytes correctly (see the DIRECTION BITS note above).
 * ================================================================ */
#define VMS_IOCTL_DCLAST      _IOW(VMS_AST_IOC_MAGIC,  0x10, struct vms_ast_args)
#define VMS_IOCTL_SETAST      _IOWR(VMS_AST_IOC_MAGIC, 0x11, struct vms_setast_args)
#define VMS_IOCTL_DELIVERAST  _IOR(VMS_AST_IOC_MAGIC,  0x12, struct vms_ast_args)

#endif /* _VMS_AST_NB_H */
