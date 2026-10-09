/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_tt_nb.h - the /dev/vms WIRE CONTRACT for the executive terminal CLASS
 * DRIVER (src/kernel-core/vms_tt.c), NetBSD twin. rd vms-f8c, epic vms-4eba.
 *
 * Byte-identical to the terminal-class-driver section of src/kernel/vms_ioctl.h
 * (the rule vms_devtab_nb.h / vms_mbx_nb.h already follow): COPIED VERBATIM,
 * not re-derived, so the Linux and NetBSD builds of the one shared class driver
 * never answer a request with two layouts. The _Static_asserts are the same
 * ones the Linux header makes, so an ILP32 VAX layout drift is a compile error
 * here, on the substrate whose width is the thing in question.
 *
 * Clean-room (AGENTS.md Rule 8): OVMX's own contract over public NetBSD kernel
 * headers. No VSI/HPE or NetBSD source is copied.
 */
#ifndef OVMX_VMS_TT_NB_H
#define OVMX_VMS_TT_NB_H

#ifndef VMS_TT_IOC_MAGIC
#define VMS_TT_IOC_MAGIC 'V'           /* VMS_IOC_MAGIC of the Linux header */
#endif

/* The terminal class driver's read completions (vms_internal.h on Linux):
 * SSDEF "$EQU SS$_TIMEOUT 556", "$EQU SS$_HANGUP 716". */
#ifndef SS__TIMEOUT
#define SS__TIMEOUT     556
#endif
#ifndef SS__HANGUP
#define SS__HANGUP      716
#endif

#define VMS_TT_RD_NOECHO      0x0001u  /* IO$M_NOECHO    */
#define VMS_TT_RD_TIMED       0x0002u  /* IO$M_TIMED     */
#define VMS_TT_RD_PURGE       0x0004u  /* IO$M_PURGE     */
#define VMS_TT_RD_NOFILTR     0x0008u  /* IO$M_NOFILTR   */
#define VMS_TT_RD_TRMNOECHO   0x0010u  /* IO$M_TRMNOECHO */
#define VMS_TT_RD_CVTLOW      0x0020u  /* IO$M_CVTLOW    */
#define VMS_TT_RD_TERMMASK    0x0100u  /* termmask[] is the caller's (P4) */
#define VMS_TT_RD_INISTR      0x0200u  /* inistr/inisz: the initial line (TRM$_INISTRNG) */

struct vms_tt_read_args {
	uint32_t chan;
	uint32_t flags;
	uint64_t buf;
	uint32_t bufsz;
	uint32_t timeout;
	uint64_t prompt;
	uint32_t promptsz;
	uint32_t termmask[8];
	uint32_t status;
	uint32_t count;
	uint32_t term;
	uint32_t termsz;
	uint32_t oflags;
	uint64_t inistr;
	uint32_t inisz;
	uint32_t pad2;
};
#define VMS_TT_RDO_ASTPEND    0x1u

struct vms_tt_write_args {
	uint32_t chan;
	uint32_t flags;
	uint64_t buf;
	uint32_t len;
	uint32_t status;
};

/* VMS_IOCTL_TT_WRITE flags (rd vms-fc4): the bytes are ONE RECORD -- a new line
 * before them, a carriage return after (IO$_WRITEVBLK with P4 carriage control
 * " ", single space) */
#define VMS_TT_WR_RECORD      0x1u

#define VMS_TT_MODE_PASSALL   0x1u
struct vms_tt_mode_args {
	uint32_t chan;
	uint32_t mode;
	uint32_t status;
	uint32_t pad;
};

struct vms_tt_bind_args {
	char     devnam[VMS_DEVNAM_SIZE];
	uint32_t status;
	uint32_t pad;
};

#define VMS_TT_SENSE_BOUND    0x1u
#define VMS_TT_SENSE_READING  0x2u
#define VMS_TT_SENSE_ECHOING  0x4u
#define VMS_TT_SENSE_PASSALL  0x8u
struct vms_tt_sense_args {
	char     devnam[VMS_DEVNAM_SIZE];
	uint32_t state;
	uint32_t status;
};

#define VMS_TT_OOB_CTRLY      1u
#define VMS_TT_OOB_CTRLC      2u
#define VMS_TT_OOB_OUTBAND    3u
struct vms_tt_oobast_args {
	uint32_t chan;
	uint32_t which;
	uint64_t astadr;
	uint64_t astprm;
	uint32_t mask;
	uint32_t acmode;
	uint32_t status;
	uint32_t pad;
};

#define VMS_IOCTL_TT_READ     _IOWR(VMS_TT_IOC_MAGIC, 0xA0, struct vms_tt_read_args)
#define VMS_IOCTL_TT_WRITE    _IOWR(VMS_TT_IOC_MAGIC, 0xA1, struct vms_tt_write_args)
#define VMS_IOCTL_TT_SETMODE  _IOWR(VMS_TT_IOC_MAGIC, 0xA2, struct vms_tt_mode_args)
#define VMS_TTIOC_BIND        _IOWR(VMS_TT_IOC_MAGIC, 0xA3, struct vms_tt_bind_args)
#define VMS_IOCTL_TT_SENSE    _IOWR(VMS_TT_IOC_MAGIC, 0xA4, struct vms_tt_sense_args)
#define VMS_IOCTL_TT_OOBAST   _IOWR(VMS_TT_IOC_MAGIC, 0xA5, struct vms_tt_oobast_args)

_Static_assert(sizeof(struct vms_tt_read_args) == 104,
               "struct vms_tt_read_args changed size -- terminal reads would decode at the wrong offsets");
_Static_assert(sizeof(struct vms_tt_write_args) == 24,
               "struct vms_tt_write_args changed size");
_Static_assert(sizeof(struct vms_tt_mode_args) == 16,
               "struct vms_tt_mode_args changed size");
_Static_assert(sizeof(struct vms_tt_bind_args) == 24,
               "struct vms_tt_bind_args changed size");
_Static_assert(VMS_IOCTL_TT_READ == 0xC06856A0u,
               "VMS_IOCTL_TT_READ encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_TT_WRITE == 0xC01856A1u,
               "VMS_IOCTL_TT_WRITE encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_TT_SETMODE == 0xC01056A2u,
               "VMS_IOCTL_TT_SETMODE encodes differently here than on the reference build");
_Static_assert(VMS_TTIOC_BIND == 0xC01856A3u,
               "VMS_TTIOC_BIND encodes differently here than on the reference build");
_Static_assert(sizeof(struct vms_tt_sense_args) == 24,
               "struct vms_tt_sense_args changed size");
_Static_assert(VMS_IOCTL_TT_SENSE == 0xC01856A4u,
               "VMS_IOCTL_TT_SENSE encodes differently here than on the reference build");
_Static_assert(sizeof(struct vms_tt_oobast_args) == 40,
               "struct vms_tt_oobast_args changed size");
_Static_assert(VMS_IOCTL_TT_OOBAST == 0xC02856A5u,
               "VMS_IOCTL_TT_OOBAST encodes differently here than on the reference build");

#endif /* OVMX_VMS_TT_NB_H */
