/*
 * vms_ping.h - the shared /dev/vms VERSION/PING contract (OVMX/NetBSD P2b,
 * rd vms-bfe, parent vms-dd8, epic vms-8e8; docs/design-ovmx-netbsd-syskrnl.md).
 *
 * This is the ONE wire contract for a minimal liveness + ABI handshake against
 * the /dev/vms executive, included IDENTICALLY by both sides of the boundary:
 *   - the in-kernel `vms' pseudo-device (src/kernel-netbsd/vms_netbsd.c), and
 *   - the userspace probe that reaches it through the transport seam
 *     (tests/netbsd/guest/vmsprobe.c via src/libvmssys/kif_transport_netbsd.c).
 *
 * WHY A SEPARATE HEADER (not src/kernel/vms_ioctl.h). vms_ioctl.h is the Linux
 * executive's full ioctl surface: it is written for {Linux kernel, generic
 * userspace} -- it keys on __KERNEL__ (Linux), pulls <stdint.h> in its non-Linux
 * branch, and carries the entire executive struct set with its _Static_asserts.
 * None of that is portable to a NetBSD KERNEL translation unit, and P2b must not
 * perturb the Linux build at all. So the ping contract lives here, self-contained
 * and portable to NetBSD kernel + NetBSD userspace (and Linux userspace), while
 * staying in the SAME magic space ('V') as vms_ioctl.h so it slots into the one
 * /dev/vms contract. A later phase can adopt VMS_IOCTL_PING into the Linux
 * executive from this same definition; P2b is NetBSD-scoped and does not.
 *
 * WIRE-IDENTICAL ACROSS SUBSTRATES. The request number is computed with the
 * substrate's own _IOWR (BSD <sys/ioccom.h> or Linux <sys/ioctl.h>). For an
 * argument struct this small the BSD 13-bit and Linux 14-bit size fields both
 * hold sizeof(struct vms_ping_args), and IOC_INOUT == (_IOC_READ|_IOC_WRITE),
 * so the two encodings coincide: the same request number on either kernel.
 */

#ifndef _VMS_PING_H
#define _VMS_PING_H

/* Fixed-width types: NetBSD kernel exposes them via <sys/types.h>; userspace
 * (any OS) via <stdint.h>. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros. On NetBSD they come from
 * <sys/ioccom.h> (kernel) or <sys/ioctl.h> (userspace); the caller normally
 * includes one already. The fallback below matches vms_ioctl.h's Linux-style
 * encoding, which for a struct this small yields the identical request number. */
#if !defined(_IOWR)
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
# endif
#endif

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V'): one /dev/vms
 * contract, one magic space. */
#define VMS_PING_IOC_MAGIC 'V'

/*
 * The ping argument struct. Exactly four uint32_t = 16 bytes on every ABI
 * OVMX targets, so the _IOWR size field (and therefore the request number) is
 * stable across architectures and across the two substrate encodings.
 *
 *   magic       IN : caller writes VMS_PING_REQ.
 *               OUT: executive replies VMS_PING_ACK (or 0 if it rejected REQ).
 *   abi_version OUT: the /dev/vms ping ABI version the executive speaks.
 *   substrate   OUT: which SYSKRNL answered (VMS_SUBSTRATE_*).
 *   status      OUT: VMS status code, odd = success (CLAUDE.md Rule 4).
 */
struct vms_ping_args {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t substrate;
    uint32_t status;
};

#define VMS_IOCTL_PING _IOWR(VMS_PING_IOC_MAGIC, 0x0f, struct vms_ping_args)

/* Request/answer cookies -- a real round trip proves the executive read the
 * caller's request and wrote its own answer, not that a zeroed buffer came
 * back untouched. */
#define VMS_PING_REQ  0x564D5350u   /* "VMSP" */
#define VMS_PING_ACK  0x504B4F21u   /* "PKO!" */

#define VMS_PING_ABI_VERSION 1u

#define VMS_SUBSTRATE_LINUX  1u
#define VMS_SUBSTRATE_NETBSD 2u

/*
 * VMS status codes used by the ping contract. Grounded to the in-tree source
 * of truth src/libvms/include/ssdef.h (CLAUDE.md Rule 4 / Source-of-Truth
 * hierarchy) -- not invented here:
 *   SS$_NORMAL     1     (odd = success)
 *   SS$_BADPARAM  20     bad request cookie
 *   SS$_NOSUCHDEV 2680   the honest "no /dev/vms" verdict the probe reports
 */
#define VMS_SS_NORMAL     1u
#define VMS_SS_BADPARAM   20u
#define VMS_SS_NOSUCHDEV  2680u

#endif /* _VMS_PING_H */
