/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_devtab_nb.h - the /dev/vms WIRE CONTRACT for the executive-resident
 * DEVICE TABLE facility (src/kernel-core/vms_devtab.c), NetBSD twin.
 * rd vms-618, epic vms-8e8.
 *
 * Byte-identical to the device-table section of src/kernel/vms_ioctl.h -- the
 * same rule vms_mbx_nb.h / vms_proctab_nb.h / vms_lock_nb.h / vms_lnm_nb.h /
 * vms_acp_nb.h already follow. Values are COPIED VERBATIM from that header, not
 * independently re-derived, so the Linux and NetBSD builds of the SAME shared
 * facility source can never answer a given request with two different layouts
 * or two different numbers. The _Static_asserts at the bottom are the mechanical
 * guard: they are the SAME assertions src/kernel/vms_ioctl.h makes, so an ILP32
 * VAX layout drift (or an ioctl renumber) is a hard compile error here, on the
 * substrate whose 32-bit width is the thing actually in question.
 *
 * WHY THIS HEADER EXISTS NOW. The device table was the last executive facility
 * NOT in the NetBSD `vms' module -- so $ALLOC / $DALLOC (VMS_IOCTL_ALLOC 0x55 /
 * DALLOC 0x56) answered ENOTTY on NetBSD/vax, and DCL's cmd_mount(), which
 * calls vms_kif_alloc() BEFORE vms_kif_acp_mount(), could never mount a device
 * there. The fix is the facility PORT (vms_devtab.c joins the module's SRCS),
 * not a substrate-local $ALLOC: an ALLOC that reported success without a real
 * executive-resident table would be exactly the per-process facade INV-6 exists
 * to kill.
 *
 * Clean-room (CLAUDE.md Rule 8): OVMX's own contract over public NetBSD kernel
 * headers. No VSI/HPE or NetBSD source is copied.
 */

#ifndef OVMX_VMS_DEVTAB_NB_H
#define OVMX_VMS_DEVTAB_NB_H

#include <sys/types.h>
#include <sys/ioccom.h>

/*
 * VMS_DEVNAM_SIZE (16) and VMS_BACKING_SIZE (16) are defined by the mailbox and
 * ACP twins this header composes with; guard them the same way those do so the
 * include order does not matter.
 */
#ifndef VMS_DEVNAM_SIZE
#define VMS_DEVNAM_SIZE 16
#endif
#ifndef VMS_BACKING_SIZE
#define VMS_BACKING_SIZE 16
#endif
/*
 * The executive's PRIVATE record of the host network interface a LAN unit
 * fronts (never surfaced to a VMS program -- INV-4). Long enough for a Linux
 * IFNAMSIZ / a NetBSD if_xname (both 16).
 */
#ifndef VMS_NETIF_SIZE
#define VMS_NETIF_SIZE 16
#endif

/* ================================================================
 * Terminal characteristics (VMS_TTC_*) -- the bit vector the device table
 * carries for a DC$_TERM unit and SET/SHOW TERMINAL reads. Bit positions
 * copied verbatim from src/kernel/vms_ioctl.h.
 * ================================================================ */
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

/* ================================================================
 * The arg structs the facility copies. Layouts verbatim from
 * src/kernel/vms_ioctl.h; see that header for the per-field provenance
 * (owner_pid vs `allocated' are two DIFFERENT things, both oracle-measured --
 * docs/oracle/vax73-terminal-device.md section 7).
 * ================================================================ */

/* One row of the executive device table, as handed to userspace. */
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

/* $ALLOC / $DALLOC: allocate a device to this process, and give it back. */
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

/*
 * $DASSGN: give a channel back. Already defined by vms_proctab_nb.h (the
 * VMS_IOCTL_DASSGN wiring landed with vms-329, before the device table was
 * ported), so this header does NOT redefine it -- it is the same struct and the
 * same request number.
 */

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

/* Cursor-driven enumeration of the device table (the reader behind SHOW DEVICE). */
struct vms_devscan_args {
	uint32_t index;             /* in: cursor; out: cursor for next call */
	uint32_t status;            /* return: SS$_ status */
	struct vms_devinfo info;    /* out: the row at the incoming cursor */
};

/* Modify terminal characteristics THROUGH AN ASSIGNED CHANNEL. */
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

/* Record this process's terminal, named by a channel it already holds. */
struct vms_setterm_args {
	uint32_t chan;              /* channel assigned to the terminal */
	uint32_t status;            /* return: SS$_ status */
};

/* ================================================================
 * Request numbers. VMS_IOC_MAGIC is 'V', identical to the Linux reference
 * build (VMS_ACP_IOC_MAGIC / VMS_PROCTAB_IOC_MAGIC in the sibling twins are the
 * same letter for the same reason: one /dev/vms request space).
 * ================================================================ */
#ifndef VMS_DEVTAB_IOC_MAGIC
#define VMS_DEVTAB_IOC_MAGIC 'V'
#endif

#define VMS_IOCTL_SETTERM   _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x45, struct vms_setterm_args)
#define VMS_IOCTL_ASSIGN    _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x50, struct vms_assign_args)
#define VMS_IOCTL_GETDVI    _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x52, struct vms_getdvi_args)
#define VMS_IOCTL_DEVSCAN   _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x53, struct vms_devscan_args)
#define VMS_IOCTL_TTSETMODE _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x54, struct vms_setmode_args)
#define VMS_IOCTL_ALLOC     _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x55, struct vms_alloc_args)
#define VMS_IOCTL_DALLOC    _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x56, struct vms_alloc_args)

/*
 * $GETDVI volume items of a mounted disk (vms-e6f) -- SHOW DEVICE's mount state,
 * ODS-2 volume label, size and free-block count. The Linux twin's full rationale
 * is in src/kernel/vms_ioctl.h; this is the SAME struct/nr re-made under the
 * 32-bit VAX compiler so the ABI guards below catch any width drift. The handler
 * (vms_ioctl_acp_getvol) is shared (src/kernel-core/vmsfs_acp.c).
 */
#ifndef VMS_GETVOL_LABEL_SIZE
#define VMS_GETVOL_LABEL_SIZE 16
#endif
struct vms_getvol_args {
	char     devnam[VMS_DEVNAM_SIZE];   /* in: unit name, e.g. "DKA0:" */
	uint32_t status;                    /* out: SS$_ status */
	uint32_t mounted;                   /* out: 1 = a mounted ODS-2 volume */
	uint32_t volsize;                   /* out: DVI$_MAXBLOCK (blocks) */
	uint32_t freeblocks;                /* out: DVI$_FREEBLOCKS (valid iff free_valid) */
	uint32_t free_valid;                /* out: 1 = freeblocks read this call */
	uint32_t cluster;                   /* out: DVI$_CLUSTER */
	uint32_t transcnt;                  /* out: file-class channels assigned (Trans Count) */
	char     volnam[VMS_GETVOL_LABEL_SIZE]; /* out: NUL-terminated ODS-2 label */
};

#define VMS_IOCTL_GETVOL    _IOWR(VMS_DEVTAB_IOC_MAGIC, 0x58, struct vms_getvol_args)

/* ================================================================
 * ABI guards -- the SAME assertions src/kernel/vms_ioctl.h makes, re-made here
 * under the 32-bit VAX compiler (the whole point of a NetBSD twin).
 * ================================================================ */
_Static_assert(sizeof(struct vms_devinfo) == 72,
               "struct vms_devinfo changed size -- kernel and userspace would disagree on device attribute offsets");
_Static_assert(sizeof(struct vms_alloc_args) == 24,
               "struct vms_alloc_args changed size -- $ALLOC/$DALLOC would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_assign_args) == 24,
               "struct vms_assign_args changed size -- $ASSIGN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_getdvi_args) == 88,
               "struct vms_getdvi_args changed size -- $GETDVI would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_devscan_args) == 80,
               "struct vms_devscan_args changed size -- $DEVICE_SCAN would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_setmode_args) == 40,
               "struct vms_setmode_args changed size -- IO$_SETMODE would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_setterm_args) == 8,
               "struct vms_setterm_args changed size -- VMS_IOCTL_SETTERM ABI break");
_Static_assert(sizeof(struct vms_getvol_args) == 60,
               "struct vms_getvol_args changed size -- $GETDVI volume items would decode at the wrong offsets");

_Static_assert(VMS_IOCTL_SETTERM == 0xC0085645u,
               "VMS_IOCTL_SETTERM encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_ASSIGN == 0xC0185650u,
               "VMS_IOCTL_ASSIGN encodes differently here than on the reference build");
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
_Static_assert(VMS_IOCTL_GETVOL == 0xC03C5658u,
               "VMS_IOCTL_GETVOL encodes differently here than on the reference build");

#endif /* OVMX_VMS_DEVTAB_NB_H */
