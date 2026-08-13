/*
 * vms_access_nb.h - the shared /dev/vms ACCESS-MODE + PRIVILEGE contract for the
 * OVMX/NetBSD substrate (rd vms-9bb, epic vms-8e8;
 * docs/design-netbsd-executive-core.md).
 *
 * P4-A compiles the SAME access-mode facility source -- src/kernel-core/vms_access.c
 * -- into the NetBSD `vms' pseudo-device that it compiles into the Linux vms.ko,
 * exactly as vms_eflag_nb.h did for event flags (P2c) and vms_ast_nb.h does for
 * ASTs. This header is the ONE wire contract for $SETMODE/$GETMODE/$SETPRV/
 * $CHKPRIV plus the paired ENTER_IMAGE/IMAGE_RUNDOWN transition, included
 * IDENTICALLY by every side of the /dev/vms boundary (the pseudo-device, the
 * shared facility via vms_internal.h, and the userspace test program).
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument structs and the PSL$C_ access
 * modes and PRV$M_ privilege bits below are the SAME the Linux executive uses
 * (src/kernel/vms_ioctl.h) -- byte-for-byte identical -- because ONE facility
 * source (src/kernel-core/vms_access.c) reads them on BOTH substrates. See
 * vms_ast_nb.h's DIRECTION BITS and _IOW STATUS notes: they apply here too
 * ($SETMODE is _IOW, so its return status is not conveyed on NetBSD, but its
 * EFFECT is observable via $GETMODE, which is _IOR and returns mode+privs).
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The access-mode/privilege SEMANTICS are the
 * publicly documented OpenVMS behaviour; PSL$C_KERNEL..USER (0..3) and the PRV$V_
 * bit positions are grounded to public $PSLDEF/$PRVDEF documentation and the
 * in-tree oracle (src/kernel/vms_ioctl.h, docs/oracle/vax73-privileges.md). The
 * argument layouts are OVMX's own, shared with the Linux executive. No NetBSD or
 * VSI source is copied.
 */

#ifndef _VMS_ACCESS_NB_H
#define _VMS_ACCESS_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros (identical dance to vms_eflag_nb.h). */
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

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V'). */
#define VMS_ACCESS_IOC_MAGIC 'V'

/* ================================================================
 * Access modes -- PSL$C_KERNEL..USER, values 0..3, byte-identical to
 * src/kernel/vms_ioctl.h (public $PSLDEF; see that header's oracle note).
 * ================================================================ */
#define PSL_C_KERNEL    0
#define PSL_C_EXEC      1
#define PSL_C_SUPER     2
#define PSL_C_USER      3

/* ================================================================
 * Privilege bits the access-mode facility ENFORCES -- the subset of
 * src/kernel/vms_ioctl.h's oracle-pinned $PRVDEF table that vms_access.c and
 * vms_ast.c actually gate on (CMKRNL/CMEXEC for mode raise + $DCLAST at an inner
 * mode; SETPRV for widening in $SETPRV). Bit positions match vms_ioctl.h exactly.
 * ================================================================ */
#define VMS_PRV_V_CMKRNL     0
#define VMS_PRV_V_CMEXEC     1
#define VMS_PRV_V_SETPRV    14

#define VMS_PRV_M_CMKRNL    (1ULL << VMS_PRV_V_CMKRNL)
#define VMS_PRV_M_CMEXEC    (1ULL << VMS_PRV_V_CMEXEC)
#define VMS_PRV_M_SETPRV    (1ULL << VMS_PRV_V_SETPRV)

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_ioctl.h.
 * ================================================================ */

struct vms_mode_args {
	uint8_t  mode;          /* target access mode (0-3) */
	uint8_t  pad[3];
	uint32_t status;        /* return: SS$_ status */
};

struct vms_getmode_args {
	uint8_t  mode;          /* return: current mode */
	uint8_t  pad[3];
	uint64_t cur_privs;     /* return: current privileges */
	uint64_t perm_privs;    /* return: permanent privileges */
};

struct vms_priv_args {
	uint64_t mask;          /* privilege mask to set/clear/check */
	uint64_t prev;          /* return: previous privilege mask */
	uint32_t enable;        /* 1=enable, 0=disable */
	uint32_t permanent;     /* 1=permanent, 0=temporary */
	uint32_t status;        /* return: SS$_ status */
	uint32_t pad;
};

struct vms_modexfer_args {
	uint8_t  prev_mode;     /* return: mode before this transition */
	uint8_t  new_mode;      /* return: mode after this transition */
	uint8_t  pad[2];
	uint32_t status;        /* return: SS$_ status */
};

/* ================================================================
 * Request numbers -- SAME NR bytes, structs, magic and direction class as
 * src/kernel/vms_ioctl.h, realized through the substrate's own macros so the
 * NetBSD cdevsw framework moves the bytes correctly (see vms_ast_nb.h).
 * ================================================================ */
#define VMS_IOCTL_SETMODE        _IOW(VMS_ACCESS_IOC_MAGIC,  0x01, struct vms_mode_args)
#define VMS_IOCTL_GETMODE        _IOR(VMS_ACCESS_IOC_MAGIC,  0x02, struct vms_getmode_args)
#define VMS_IOCTL_SETPRV         _IOWR(VMS_ACCESS_IOC_MAGIC, 0x03, struct vms_priv_args)
#define VMS_IOCTL_CHKPRIV        _IOWR(VMS_ACCESS_IOC_MAGIC, 0x04, struct vms_priv_args)
#define VMS_IOCTL_ENTER_IMAGE    _IOWR(VMS_ACCESS_IOC_MAGIC, 0x66, struct vms_modexfer_args)
#define VMS_IOCTL_IMAGE_RUNDOWN  _IOWR(VMS_ACCESS_IOC_MAGIC, 0x67, struct vms_modexfer_args)

#endif /* _VMS_ACCESS_NB_H */
