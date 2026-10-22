/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_l2_nb.h - the executive L2 (raw datalink) wire contract on the NetBSD
 * substrate (rd vms-9394).
 *
 * Unlike the other *_nb.h twins, this header COPIES NOTHING: it puts in scope
 * what src/kernel/vms_l2.h relies on (the integer typedefs, _IOWR and
 * VMS_IOC_MAGIC -- vms_l2.h's own "relies on ... already being in scope"
 * contract) and then includes THAT file, so the struct vms_l2_*_args layouts,
 * the VMS_IOCTL_L2_* request numbers and the station-address policy
 * (vms_l2_station.h, which vms_l2.h includes) have exactly one definition for
 * the Linux vms.ko, the NetBSD vms.kmod and every userspace consumer. vms_l2.h's
 * own _Static_asserts pin the four request words to the reference encoding
 * (0xC0305692 ...); NetBSD's <sys/ioccom.h> _IOWR encodes the same direction
 * bits, 13-bit length and group/number bytes for these sizes (all <= 2072,
 * under IOCPARM_MAX), so those asserts are checked again here, on this
 * substrate, at compile time.
 *
 * The two headers are reached through the module build's -idirafter
 * src/kernel (src/kernel-netbsd/Makefile, tools/cross-vax/build-*-vax.sh):
 * searched AFTER this directory and src/kernel-core, so the NetBSD twin
 * vms_internal.h and every *_nb.h still win, and only names that exist
 * nowhere else -- vms_l2.h, vms_l2_station.h -- come from src/kernel.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own ABI; PHY_IO's bit position is the
 * public $PRVDEF value, the same one src/kernel/vms_ioctl.h carries.
 */
#ifndef _VMS_L2_NB_H
#define _VMS_L2_NB_H

#include <sys/types.h>
#include <sys/ioccom.h>    /* _IOWR */

#ifndef VMS_IOC_MAGIC
#define VMS_IOC_MAGIC 'V'  /* == src/kernel/vms_ioctl.h; vms_l2.h's asserts
                            * fail the build if it ever differed */
#endif

/* PHY_IO ("may do physical I/O") -- the privilege VMS_IOCTL_L2_OPEN gates on.
 * Byte-identical to src/kernel/vms_ioctl.h (public $PRVDEF bit 22). */
#ifndef VMS_PRV_V_PHY_IO
#define VMS_PRV_V_PHY_IO    22
#endif
#ifndef VMS_PRV_M_PHY_IO
#define VMS_PRV_M_PHY_IO    (1ULL << VMS_PRV_V_PHY_IO)
#endif

#include "vms_l2.h"        /* the ONE definition (src/kernel, via -idirafter) */

#endif /* _VMS_L2_NB_H */
