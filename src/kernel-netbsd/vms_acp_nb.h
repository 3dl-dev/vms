/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_acp_nb.h - the shared /dev/vms Files-11 (ODS-2) ACP contract for the
 * OVMX/NetBSD substrate (rd vms-6a7f, epic vms-208; docs/design-netbsd-
 * executive-core.md, docs/design-files11-acp-executive.md).
 *
 * WHY THIS FILE. src/kernel-core/vmsfs_acp.c -- the substrate-agnostic Files-11
 * ACP channel/mount/IO$_ACCESS/READVBLK/WRITEVBLK/IO$_ACPCONTROL ($SEARCH)
 * handlers -- compiles by
 * `#include "vms_internal.h"` resolving, via -I, to the per-substrate twin. On
 * Linux that twin (src/kernel/vms_internal.h) reaches these argument structs
 * transitively through vms_ioctl.h's `#include "vms_acp.h"` (src/kernel/
 * vms_acp.h). The NetBSD twin (src/kernel-netbsd/vms_internal.h) carries no
 * such vms_ioctl.h, so -- exactly as it already does for the mailbox facility
 * (vms_mbx_nb.h) and event flags (vms_eflag_nb.h) -- it gets its own small
 * contract header, included from vms_internal.h, giving vmsfs_acp.c the SAME
 * struct names it names on Linux. This is the elf32-vax cross-build's compile-
 * coverage rung (vms-6a7f): wiring the real NetBSD/VAX kmod driver (vms_netbsd.c
 * d_ioctl dispatch of these ops) is a LATER re-target (vms-d5d), out of scope
 * here -- this header exists so the handler TUs themselves type-check against
 * the real NetBSD contract rather than a scratch stub.
 *
 * ONE FACILITY SOURCE, TWO SUBSTRATES. The argument STRUCTS below are BYTE-
 * IDENTICAL to src/kernel/vms_acp.h (the _Static_asserts here freeze the same
 * sizes) because the ONE facility source (src/kernel-core/vmsfs_acp.c) copies
 * them in and out on BOTH substrates. Only the ioctl-number ENCODING differs
 * by substrate (the vms_mbx_nb.h precedent) -- a transport detail, not feature
 * drift.
 *
 * CLEAN ROOM (CLAUDE.md Rule 8). Same posture as src/kernel/vms_acp.h: the
 * ODS-2 on-disk structures are byte-authentic (the codec, src/vmsfs/ods2/); the
 * ioctl arg-struct BYTE LAYOUT below is an OVMX design choice, not a claim of
 * VMS byte-fidelity. No NetBSD or VSI/HPE source is copied.
 */

#ifndef _VMS_ACP_NB_H
#define _VMS_ACP_NB_H

/* Fixed-width types: NetBSD kernel via <sys/types.h>; userspace via <stdint.h>.
 * Same split as vms_mbx_nb.h / vms_eflag_nb.h. */
#if defined(_KERNEL)
#include <sys/types.h>
#else
#include <stdint.h>
#endif

/* Prefer the substrate's own _IO* macros -- identical dance to vms_mbx_nb.h. */
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

/* Same magic byte as src/kernel/vms_ioctl.h (VMS_IOC_MAGIC 'V') / vms_mbx_nb.h:
 * one /dev/vms contract, one magic space. */
#define VMS_ACP_IOC_MAGIC 'V'

/* Device-name field width -- matches src/kernel/vms_ioctl.h's VMS_DEVNAM_SIZE.
 * Guarded so this header composes with any other /dev/vms contract header
 * (vms_mbx_nb.h, vms_ping.h) that also defines it. */
#ifndef VMS_DEVNAM_SIZE
#define VMS_DEVNAM_SIZE 16
#endif

/* Backing block-device name field width -- matches src/kernel/vms_ioctl.h's
 * VMS_BACKING_SIZE (16). Guarded like VMS_DEVNAM_SIZE so this header composes
 * with any other /dev/vms contract header that also defines it. */
#ifndef VMS_BACKING_SIZE
#define VMS_BACKING_SIZE 16
#endif

/* FIB$L_ACCTL access-control flag + the IO$_ACCESS name buffer size --
 * byte-identical to src/kernel/vms_acp.h. */
#define VMS_ACP_ACCTL_WRITE   0x00000001u
#define VMS_ACP_NAME_SIZE     80

/* ================================================================
 * Argument structs -- byte-identical to src/kernel/vms_acp.h. The shared
 * facility (src/kernel-core/vmsfs_acp.c) copies exactly these in and out.
 * ================================================================ */

struct vms_acp_mount_args {
	char     devnam[VMS_DEVNAM_SIZE];   /* in: unit name, e.g. "DKA0:" */
	uint32_t status;                    /* out: SS$_ status */
	uint32_t pad;
};

struct vms_acp_dmount_args {
	char     devnam[VMS_DEVNAM_SIZE];   /* in: unit name */
	uint32_t status;                    /* out: SS$_ status */
	uint32_t pad;
};

struct vms_acp_assign_args {
	char     devnam[VMS_DEVNAM_SIZE];   /* in: mounted volume unit name */
	uint32_t chan;                      /* out: file-class channel number */
	uint32_t status;                    /* out: SS$_ status */
};

struct vms_acp_fileattr {
	uint32_t filechar;      /* ATR$C_UCHAR: fh2_filechar (bit 0x2000 = directory) */
	uint32_t efblk;         /* end-of-file VBN (decoded FAT) */
	uint32_t hiblk;         /* highest allocated VBN (decoded FAT) */
	uint16_t ffbyte;        /* first free byte in the EOF block */
	uint16_t fileprot;      /* ATR$C_FPRO: fh2_fileprot (4 nibbles S/O/G/W) */
	uint16_t uic_group;     /* ATR$C_UIC: fh2_fileowner group */
	uint16_t uic_member;    /* ATR$C_UIC: fh2_fileowner member */
	uint16_t revision;      /* fi2_revision (ident area) */
	uint16_t pad0;
	uint8_t  recattr[32];   /* ATR$C_RECATTR: the 32-byte FAT, verbatim */
	uint8_t  credate[8];    /* ATR$C_CREDATE (VMS 64-bit absolute time) */
	uint8_t  revdate[8];    /* ATR$C_REVDATE */
	uint8_t  expdate[8];    /* ATR$C_EXPDATE */
	uint8_t  bakdate[8];    /* ATR$C_BAKDATE */
};

struct vms_acp_access_args {
	uint32_t chan;              /* in: file-class channel ($ASSIGN of the volume) */
	uint32_t acctl;            /* in: FIB$L_ACCTL flags (VMS_ACP_ACCTL_*) */
	uint16_t did_num;          /* in: FIB$W_DID directory FID (0/0/0 => MFD) */
	uint16_t did_seq;
	uint8_t  did_rvn;
	uint8_t  did_nmx;
	uint8_t  fidmode;          /* in: !=0 => open by FID below, ignore name/DID */
	uint8_t  pad0;
	uint16_t fid_num;          /* in (fidmode) / out: FIB$W_FID file number low 16 */
	uint16_t fid_seq;          /* in (fidmode) / out: sequence */
	uint8_t  fid_rvn;          /* in (fidmode) / out: relative volume */
	uint8_t  fid_nmx;          /* in (fidmode) / out: file number high 8 */
	uint16_t version;          /* in: wanted version (0 => highest) */
	uint16_t out_version;      /* out: resolved version */
	uint16_t pad1;
	uint32_t probe_vbn;        /* in: VBN to resolve through the window (0 => none) */
	uint32_t probe_lbn;        /* out: LBN probe_vbn maps to (0 if none/unmapped) */
	uint32_t window_nextents;  /* out: extents in the built window */
	uint32_t total_blocks;     /* out: sum of window extent counts */
	uint32_t first_lbn;        /* out: LBN of the window's first extent (VBN 1) */
	char     name[VMS_ACP_NAME_SIZE]; /* in (P2): "NAME.TYPE" incl type */
	struct vms_acp_fileattr attr;     /* out (P5 subset) */
	uint32_t status;           /* out: SS$_ */
	uint32_t pad2;
};

struct vms_acp_deaccess_args {
	uint32_t chan;             /* in: file-class channel */
	uint32_t status;           /* out: SS$_ */
};

struct vms_acp_rw_args {
	uint32_t chan;        /* in: file-class channel with an accessed file */
	uint32_t vbn;         /* in: starting virtual block number (1-based) */
	uint32_t offset;      /* in: byte offset within the starting block (0..511) */
	uint32_t length;      /* in: transfer length in bytes */
	uint64_t buffer;      /* in: user data buffer (READ: dest; WRITE: src) */
	uint32_t xferred;     /* out: bytes transferred */
	uint32_t new_hiblk;   /* out: highest allocated VBN after (grows on extend) */
	uint32_t new_efblk;   /* out: end-of-file VBN after */
	uint32_t extended;    /* out: blocks newly allocated by an implicit extend */
	uint32_t status;      /* out: SS$_ */
	uint32_t pad;
};

/* IO$_ACPCONTROL subfunctions (the umbrella `func` selector) -- byte-identical
 * to src/kernel/vms_acp.h. */
#define VMS_ACP_CTL_SEARCH    1u    /* wildcard directory context ($SEARCH) */

/* Resultant-name buffer (P3/P4) -- byte-identical to src/kernel/vms_acp.h. */
#define VMS_ACP_RESNAM_SIZE   84

struct vms_acp_acpcontrol_args {
	uint32_t chan;             /* in: file-class channel ($ASSIGN of the volume) */
	uint32_t func;             /* in: VMS_ACP_CTL_* subfunction (SEARCH here) */
	uint16_t did_num;          /* in: FIB$W_DID directory FID (0/0/0 => MFD) */
	uint16_t did_seq;
	uint8_t  did_rvn;
	uint8_t  did_nmx;
	uint8_t  wcc_reset;        /* in: 1 => (re)open ctx with `pattern`; 0 => continue */
	uint8_t  pad0;
	uint16_t fid_num;          /* out: matched file FID number low 16 */
	uint16_t fid_seq;          /* out: sequence */
	uint8_t  fid_rvn;          /* out: relative volume */
	uint8_t  fid_nmx;          /* out: file number high 8 */
	uint16_t out_version;      /* out: matched version */
	uint16_t resnam_len;       /* out: length of resnam (P3), excl. NUL */
	uint16_t pad1;
	char     pattern[VMS_ACP_NAME_SIZE];   /* in (P2): wildcard, e.g. "*.TXT" */
	char     resnam[VMS_ACP_RESNAM_SIZE];  /* out (P4): "NAME.TYPE;VERSION" */
	uint32_t status;           /* out: SS$_ (NORMAL / NOMOREFILES / IVCHAN / ...) */
	uint32_t pad2;
};

/*
 * IO$_CREATE / IO$_DELETE / IO$_MODIFY via a func-dispatched ioctl on nr 0x6F
 * (size-distinct from ACPCONTROL). NetBSD-substrate MIRROR of the same-named
 * surface in src/kernel/vms_acp.h -- byte-identical (all fixed-width, so the
 * layout + the sizeof==344 hold identically on ILP32 and LP64). See vms_acp.h
 * for the FIB/ATR role documentation; retire this twin per vms-02b.
 */
#define VMS_ACP_FOP_CREATE   9u    /* IO$_CREATE */
#define VMS_ACP_FOP_DELETE   3u    /* IO$_DELETE */
#define VMS_ACP_FOP_MODIFY   6u    /* IO$_MODIFY */
#define VMS_ACP_M_CREATE     0x0001u  /* IO$M_CREATE: enter the file in a directory */
#define VMS_ACP_M_ACCESS     0x0002u  /* IO$M_ACCESS: also access it (build a window) */
#define VMS_ACP_M_DELETE     0x0004u  /* IO$M_DELETE: also delete the file (dealloc) */
#define VMS_ACP_M_MOVE       0x0008u  /* IO$M_MOVE: MODIFY renames/moves the file */
#define VMS_ACP_ATTR_PROT    0x01u    /* apply attr.fileprot */
#define VMS_ACP_ATTR_OWNER   0x02u    /* apply attr.uic_group/uic_member */
struct vms_acp_fileop_args {
	uint32_t chan;
	uint32_t func;
	uint32_t modifiers;
	uint32_t acctl;
	uint16_t did_num;
	uint16_t did_seq;
	uint8_t  did_rvn;
	uint8_t  did_nmx;
	uint8_t  fidmode;
	uint8_t  kind;
	uint16_t fid_num;
	uint16_t fid_seq;
	uint8_t  fid_rvn;
	uint8_t  fid_nmx;
	uint16_t version;
	uint16_t out_version;
	uint8_t  attr_ctl;
	uint8_t  pad0;
	uint32_t exsz;
	uint32_t trunc_efblk;
	uint16_t trunc_ffbyte;
	uint16_t pad1;
	uint32_t window_nextents;
	uint32_t total_blocks;
	uint32_t first_lbn;
	uint32_t new_hiblk;
	uint32_t new_efblk;
	uint32_t new_ffbyte;
	uint32_t pad2;
	char     name[VMS_ACP_NAME_SIZE];
	struct vms_acp_fileattr attr;
	uint32_t status;
	uint32_t pad3;
	/* --- MODIFY!VMS_ACP_M_MOVE (rename/move) target, vms-de7 --- */
	uint16_t new_did_num;
	uint16_t new_did_seq;
	uint8_t  new_did_rvn;
	uint8_t  new_did_nmx;
	uint16_t new_version;
	uint16_t pad4;
	uint16_t pad5;
	char     new_name[VMS_ACP_NAME_SIZE];
};

/*
 * Disk-unit resolve (rd vms-f60) -- NetBSD twin of src/kernel/vms_ioctl.h's
 * struct vms_diskresolve_args. INITIALIZE.EXE names a VMS disk unit ("DKA0:")
 * and the executive resolves it to the REAL backing block device the unit
 * labels (device-native, vms-47d). BYTE-IDENTICAL to the Linux struct (all
 * fixed-width -> same layout on ILP32/VAX and LP64), so the ONE userspace
 * client decodes it the same across substrates. Not an ACP file op -- it names
 * a device, it does not touch a mounted volume -- but it shares the /dev/vms
 * magic space, so it lives beside the ACP contract.
 */
struct vms_diskresolve_args {
	char     devnam[VMS_DEVNAM_SIZE];   /* in: disk unit name, e.g. "DKA0:" */
	char     backing[VMS_BACKING_SIZE]; /* out: native block dev, e.g. "ra1c" */
	uint32_t backing_major;             /* out: backing dev_t major */
	uint32_t backing_minor;             /* out: backing dev_t minor */
	uint32_t status;                    /* return: SS$_ status */
	uint32_t pad;
};

/* ================================================================
 * Request numbers -- same NR band as src/kernel/vms_acp.h (0x68-0x6F); the
 * NetBSD _IOWR encoding of type/nr/size legitimately differs in VALUE from
 * Linux's (the vms_mbx_nb.h precedent), so no cross-substrate equality assert.
 * ================================================================ */
#define VMS_IOCTL_ACP_MOUNT     _IOWR(VMS_ACP_IOC_MAGIC, 0x68, struct vms_acp_mount_args)
#define VMS_IOCTL_ACP_DMOUNT    _IOWR(VMS_ACP_IOC_MAGIC, 0x69, struct vms_acp_dmount_args)
#define VMS_IOCTL_ACP_ASSIGN    _IOWR(VMS_ACP_IOC_MAGIC, 0x6A, struct vms_acp_assign_args)
#define VMS_IOCTL_ACP_ACCESS    _IOWR(VMS_ACP_IOC_MAGIC, 0x6B, struct vms_acp_access_args)
#define VMS_IOCTL_ACP_DEACCESS  _IOWR(VMS_ACP_IOC_MAGIC, 0x6C, struct vms_acp_deaccess_args)
#define VMS_IOCTL_ACP_READVBLK  _IOWR(VMS_ACP_IOC_MAGIC, 0x6D, struct vms_acp_rw_args)
#define VMS_IOCTL_ACP_WRITEVBLK _IOWR(VMS_ACP_IOC_MAGIC, 0x6E, struct vms_acp_rw_args)
#define VMS_IOCTL_ACP_ACPCONTROL _IOWR(VMS_ACP_IOC_MAGIC, 0x6F, struct vms_acp_acpcontrol_args)
#define VMS_IOCTL_ACP_FILEOP     _IOWR(VMS_ACP_IOC_MAGIC, 0x6F, struct vms_acp_fileop_args)

/*
 * Disk-unit resolve -- nr 0x57, the SAME (magic,nr,size) as the Linux
 * VMS_IOCTL_DISK_RESOLVE. VMS_ACP_IOC_MAGIC is 'V', identical to the Linux
 * VMS_IOC_MAGIC, and sizeof(struct vms_diskresolve_args) == 48 on both
 * substrates, so _IOWR folds to the IDENTICAL request number 0xC0305657 (unlike
 * the ACP ops, whose Linux/NetBSD numbers may legitimately differ). The
 * cross-substrate equality is asserted below -- one userspace ioctl number
 * reaches both executives.
 */
#define VMS_IOCTL_DISK_RESOLVE   _IOWR(VMS_ACP_IOC_MAGIC, 0x57, struct vms_diskresolve_args)

/*
 * Freeze the shared layouts -- see src/kernel/vms_acp.h's identical asserts:
 * both sides of /dev/vms compile these structs separately and pass them by raw
 * address, so a size drift is an ABI break. These MUST match vms_acp.h exactly.
 */
_Static_assert(sizeof(struct vms_acp_mount_args) == 24,
               "vms_acp_mount_args changed size -- VMS_IOCTL_ACP_MOUNT ABI break");
_Static_assert(sizeof(struct vms_acp_dmount_args) == 24,
               "vms_acp_dmount_args changed size -- VMS_IOCTL_ACP_DMOUNT ABI break");
_Static_assert(sizeof(struct vms_acp_assign_args) == 24,
               "vms_acp_assign_args changed size -- VMS_IOCTL_ACP_ASSIGN ABI break");
_Static_assert(sizeof(struct vms_acp_fileattr) == 88,
               "vms_acp_fileattr changed size -- IO$_ACCESS ATR ABI break");
_Static_assert(sizeof(struct vms_acp_access_args) == 224,
               "vms_acp_access_args changed size -- VMS_IOCTL_ACP_ACCESS ABI break");
_Static_assert(sizeof(struct vms_acp_deaccess_args) == 8,
               "vms_acp_deaccess_args changed size -- VMS_IOCTL_ACP_DEACCESS ABI break");
_Static_assert(sizeof(struct vms_acp_rw_args) == 48,
               "vms_acp_rw_args changed size -- ACP READVBLK/WRITEVBLK ABI break");
_Static_assert(sizeof(struct vms_acp_acpcontrol_args) == 200,
               "vms_acp_acpcontrol_args changed size -- VMS_IOCTL_ACP_ACPCONTROL ABI break");
_Static_assert(sizeof(struct vms_acp_fileop_args) == 344,
               "vms_acp_fileop_args changed size -- VMS_IOCTL_ACP_FILEOP ABI break");
_Static_assert(VMS_IOCTL_ACP_FILEOP != VMS_IOCTL_ACP_ACPCONTROL,
               "FILEOP/ACPCONTROL must stay distinct on nr 0x6F (size-distinct _IOWR)");
_Static_assert(sizeof(struct vms_diskresolve_args) == 48,
               "struct vms_diskresolve_args changed size -- disk unit resolution would decode at the wrong offsets");
/*
 * Unlike the ACP ops, this ioctl number MUST equal the Linux side's: the same
 * userspace INITIALIZE.EXE client issues it against either executive. 'V' magic
 * + nr 0x57 + size 48 fold to 0xC0305657 on both substrates.
 */
_Static_assert(VMS_IOCTL_DISK_RESOLVE == 0xC0305657u,
               "VMS_IOCTL_DISK_RESOLVE encodes differently here than on the Linux reference build");

#endif /* _VMS_ACP_NB_H */
