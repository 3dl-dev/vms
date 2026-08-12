/*
 * vms_eflag_nb.h - the shared /dev/vms COMMON EVENT FLAG CLUSTER contract
 * (OVMX/NetBSD P2c, rd vms-4b4, parent vms-dd8, epic vms-8e8;
 * docs/design-ovmx-netbsd-syskrnl.md).
 *
 * P2c is the phase that PROVES the executive is REAL on the NetBSD substrate:
 * ONE VMS executive facility -- event flags -- implemented IN the in-kernel
 * `vms' pseudo-device (src/kernel-netbsd/vms_netbsd.c), holding SYSTEM-WIDE
 * SHARED state in kernel memory. This header is the ONE wire contract for that
 * facility, included IDENTICALLY by both sides of the /dev/vms boundary:
 *   - the in-kernel `vms' pseudo-device (src/kernel-netbsd/vms_netbsd.c), and
 *   - the userspace test program that reaches it through the transport seam
 *     (tests/netbsd/guest/vmseflag.c via src/libvmssys/kif_transport_netbsd.c).
 *
 * WHAT FACILITY THIS IS. On OpenVMS an event flag cluster is 32 flags. The two
 * LOCAL clusters (EFN 0..63) are PER-PROCESS; the two COMMON clusters (cluster
 * 2 = EFN 64..95, cluster 3 = EFN 96..127) are the SYSTEM-WIDE SHARED flags
 * that different processes use to signal one another. This P2c slice implements
 * exactly the COMMON (shared) clusters -- the flags that MUST live in the
 * executive, not in any one process, and whose cross-process visibility is the
 * INV-6-decisive property (CLAUDE.md Rule 9): a real executive shares the
 * state; a per-process fake reports success while sharing nothing. The LOCAL
 * per-process clusters are a DIFFERENT facility, not built in this phase; the
 * driver rejects an EFN outside the common range with an honest SS$_ILLEFC
 * rather than faking per-process local state (INV-6 -- never a silent fake).
 *
 * WHY A SEPARATE HEADER (not src/kernel/vms_ioctl.h). Exactly as vms_ping.h
 * documents for the ping contract: vms_ioctl.h is the Linux executive's full
 * ioctl surface -- it keys on __KERNEL__ (Linux), carries the whole executive
 * struct set with its _Static_asserts, and is not portable to a NetBSD KERNEL
 * translation unit; and P2c must not perturb the Linux build at all. So this
 * contract lives here, self-contained and portable to {NetBSD kernel, NetBSD
 * userspace, Linux userspace}, in the SAME magic space ('V') as vms_ioctl.h and
 * vms_ping.h so it slots into the one /dev/vms contract.
 *
 * SAME COMMAND NUMBERS AS THE LINUX EXECUTIVE. The command numbers below --
 * SETEF 0x20, CLREF 0x21, READEF 0x25 -- are the SAME as the Linux executive's
 * event-flag ioctls (src/kernel/vms_ioctl.h): one /dev/vms contract, one magic
 * space. The argument struct is P2c's OWN (a self-contained 16-byte struct,
 * like vms_ping.h keeps its own), so the _IOWR size field differs from Linux's;
 * that is fine because on the NetBSD substrate BOTH sides of the boundary
 * include THIS header, so the request numbers agree. A later phase can unify the
 * struct layouts across substrates; P2c is NetBSD-scoped and self-contained.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). The event-flag SEMANTICS (set/clear/read a
 * common cluster; previous-state-was-set/clear status) are the PUBLICLY
 * documented OpenVMS $SETEF/$CLREF/$READEF behaviour, matched against the Linux
 * executive as the in-tree semantic reference. The NetBSD kernel implementation
 * is written from the public kmutex(9)/module(9)/cdevsw(9) interfaces; no NetBSD
 * or VSI source is copied.
 */

#ifndef _VMS_EFLAG_NB_H
#define _VMS_EFLAG_NB_H

/* Fixed-width types: NetBSD kernel exposes them via <sys/types.h>; userspace
 * (any OS) via <stdint.h>. Same split as vms_ping.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros -- identical resolution dance as
 * vms_ping.h, so this header is self-sufficient wherever it is included. On
 * NetBSD they come from <sys/ioccom.h> (kernel) or <sys/ioctl.h> (userspace). */
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

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V') and vms_ping.h:
 * one /dev/vms contract, one magic space. */
#define VMS_EFLAG_IOC_MAGIC 'V'

/*
 * The event-flag argument struct. Exactly four uint32_t = 16 bytes on every ABI
 * OVMX targets, so the _IOWR size field (and therefore the request number) is
 * stable across architectures. One struct serves all three ioctls.
 *
 *   efn    IN : the common event flag number (VMS_EF_COMMON_LO..HI).
 *   state  OUT: READEF returns the naming cluster's full 32-bit state; the
 *               caller tests its own bit. Undefined for SETEF/CLREF.
 *   status OUT: VMS status code, odd = success (CLAUDE.md Rule 4):
 *               SETEF/CLREF -> SS$_WASSET (9) or SS$_WASCLR (1) = the flag's
 *               PREVIOUS state, exactly as $SETEF/$CLREF report it; READEF ->
 *               SS$_NORMAL (1); an EFN outside the common range -> SS$_ILLEFC.
 *   pad    -- keeps the struct at a fixed 16 bytes.
 */
struct vms_eflag_args {
    uint32_t efn;
    uint32_t state;
    uint32_t status;
    uint32_t pad;
};

#define VMS_IOCTL_SETEF  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x20, struct vms_eflag_args)
#define VMS_IOCTL_CLREF  _IOWR(VMS_EFLAG_IOC_MAGIC, 0x21, struct vms_eflag_args)
#define VMS_IOCTL_READEF _IOWR(VMS_EFLAG_IOC_MAGIC, 0x25, struct vms_eflag_args)

/*
 * Common event flag cluster range (OpenVMS): cluster 2 = EFN 64..95,
 * cluster 3 = EFN 96..127. These are the SYSTEM-WIDE SHARED flags. An EFN
 * outside this range is not a common flag and is rejected SS$_ILLEFC: the
 * LOCAL per-process clusters (0..63) are a different facility this P2c slice
 * does not build, and 128+ is illegal on VMS.
 */
#define VMS_EF_COMMON_LO 64u
#define VMS_EF_COMMON_HI 127u

/*
 * VMS status codes used by this contract. Grounded to the in-tree source of
 * truth src/libvms/include/ssdef.h (CLAUDE.md Rule 4 / Source-of-Truth
 * hierarchy) -- not invented here:
 *   SS$_NORMAL   1   (odd = success)
 *   SS$_WASCLR   1   previous state was clear (== SS$_NORMAL on VMS)
 *   SS$_WASSET   9   previous state was set
 *   SS$_ILLEFC 236   illegal event flag cluster (%SYSTEM-F-ILLEFC)
 *   SS$_NOSUCHDEV 2680  the honest "no /dev/vms" verdict the probe reports
 *
 * Guarded with #ifndef so this header composes with vms_ping.h (which defines
 * the same SS$ macros to the same values) in a translation unit that includes
 * both -- the in-kernel driver includes both.
 */
#ifndef VMS_SS_NORMAL
#define VMS_SS_NORMAL 1u
#endif
#ifndef VMS_SS_WASCLR
#define VMS_SS_WASCLR 1u
#endif
#ifndef VMS_SS_WASSET
#define VMS_SS_WASSET 9u
#endif
#ifndef VMS_SS_ILLEFC
#define VMS_SS_ILLEFC 236u
#endif
#ifndef VMS_SS_NOSUCHDEV
#define VMS_SS_NOSUCHDEV 2680u
#endif

#endif /* _VMS_EFLAG_NB_H */
