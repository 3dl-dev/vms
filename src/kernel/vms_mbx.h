/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mbx.h - Executive-resident mailboxes (MBAn:, vms-d44)
 *
 * Shared by the kernel module and userspace, exactly like vms_lnm.h: both
 * sides compile these structures from this one file and pass them across
 * /dev/vms by raw address.
 *
 * WHAT THIS REPLACES. src/libvms/syssvc/sys_mailbox.c used to implement
 * $CREMBX/$DELMBX with a Unix AF_UNIX socketpair private to the creating
 * process, and registered the mailbox's logical name in LNM$PROCESS_TABLE
 * (also per-process). Two processes that both called $CREMBX with the same
 * logical name each got their OWN socketpair and each called it MBA1: --
 * a socketpair end is not a thing an unrelated process can open, so no
 * second process could ever reach the first's mailbox. That is the exact
 * INV-6 shape (CLAUDE.md Rule 9): a facility that LOOKS like shared system
 * state while sharing nothing. A mailbox is, on real VMS, a device: created
 * by one process, written by one process, read by another (System Services
 * Reference, $CREMBX). It has to live in the executive for that to be true
 * here, following the SAME residency pattern LNM$SYSTEM (vms_lnm.c/.h) and
 * the device table (vms_devtab.c) already use.
 *
 * WHO OWNS WHAT. vms.ko owns the mailbox table (src/kernel/vms_mbx.c): the
 * unit number, the device name MBAn:, the message queue and the buffer
 * quota accounting. A process's CHANNEL to a mailbox (from $CREMBX or from
 * $ASSIGN to an existing MBAn:) is tracked per-process, in
 * struct vms_proc's mbx_channels list -- the same "device is the
 * executive's, the channel is the process's" split vms_devtab.c uses.
 * Deassigning the last channel to a TEMPORARY mailbox (or any mailbox
 * $DELMBX has marked) frees it; a PERMANENT mailbox with no channels left
 * survives until $DELMBX marks it, exactly as OpenVMS documents.
 *
 * RENDEZVOUS. Two UNRELATED processes reach the same mailbue by device name
 * (MBAn:, via $ASSIGN) or, for a NAMED mailbox, by logical name. This
 * implementation defines a named mailbox's logical name in LNM$SYSTEM
 * (src/kernel/vms_lnm.c's already-executive-resident table), not
 * LNM$PROCESS_TABLE as the userspace-only implementation did: a name in
 * LNM$PROCESS is invisible to every other process, so it could never do the
 * one thing this item exists to make possible. STATED AS AN OVMX DESIGN
 * CHOICE (real $CREMBX's own logical-name argument on genuine VMS defaults
 * to the caller's LNM$PROCESS_TABLE, and only lands in LNM$SYSTEM under an
 * explicit /TABLE=LNM$SYSTEM-equivalent qualifier and SYSNAM/SYSPRV
 * privilege) -- see src/libvms/syssvc/sys_mailbox.c's header for the same
 * note where it is actually invoked.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), stated rather than implied: the
 * per-ioctl message transfer cap (VMS_MBX_IOCTL_MAXLEN) and the default
 * MAXMSG/BUFQUO below are OVMX's own numbers. Public VMS documentation
 * ($CREMBX) says a zero MAXMSG/BUFQUO takes the SYSGEN parameters
 * DEFMBXMXMSG/DEFMBXBUFQUO, which are site-tunable and have no single
 * universal value to cite -- so OVMX picks its own default rather than
 * presenting a made-up number as a VMS constant, the same posture
 * vms_lnm.h takes for its arena sizing.
 */

#ifndef _VMS_MBX_H
#define _VMS_MBX_H

/*
 * Relies on the integer typedefs and on _IOWR / VMS_IOC_MAGIC already
 * being in scope. vms_ioctl.h includes this file at its foot, after it has
 * set both up for the kernel and userspace builds alike.
 */

/* A single ioctl transfer, in either direction (OVMX design cap, not a VMS
 * value -- see the file header). Large enough for typical control/IPC
 * traffic; a caller wanting more is out of scope for vms-d44. */
#define VMS_MBX_IOCTL_MAXLEN   4096u

/* OVMX defaults used when the caller passes 0 for MAXMSG/BUFQUO (design
 * choice standing in for VMS's site-tunable DEFMBXMXMSG/DEFMBXBUFQUO --
 * see the file header). Both are clamped to VMS_MBX_IOCTL_MAXLEN. */
#define VMS_MBX_DEFAULT_MAXMSG 1024u
#define VMS_MBX_DEFAULT_BUFQUO 4096u

/*
 * $CREMBX: create a mailbox and hand back a channel to it.
 *
 * `permanent` gates on VMS_PRV_M_PRMMBX; a temporary mailbox gates on
 * VMS_PRV_M_TMPMBX (both checked against the caller's cur_privs, exactly
 * as vms_lnm.c's lnm_priv_check() gates LNM$SYSTEM/GROUP mutation).
 */
struct vms_mbx_create_args {
    uint32_t permanent;     /* in: 1 = permanent (PRMMBX), 0 = temporary (TMPMBX) */
    uint32_t maxmsg;        /* in: 0 => VMS_MBX_DEFAULT_MAXMSG */
    uint32_t bufquo;        /* in: 0 => VMS_MBX_DEFAULT_BUFQUO */
    uint32_t chan;          /* out: this process's channel number */
    uint32_t unit;          /* out: MBAn: unit number */
    uint32_t status;        /* out: SS$_ status */
    char     devnam[VMS_DEVNAM_SIZE]; /* out: "MBAn:" */
};

/* $ASSIGN to an EXISTING mailbox by device name ("MBAn:"), the rendezvous
 * path an unrelated process (or a second channel from the same process)
 * uses to reach a mailbox it did not create. */
struct vms_mbx_assign_args {
    char     devnam[VMS_DEVNAM_SIZE]; /* in */
    uint32_t chan;                    /* out */
    uint32_t status;                  /* out */
};

/* $DELMBX: mark the mailbox behind `chan` for deletion. Does NOT deassign
 * the caller's own channel -- deletion happens when the LAST channel (this
 * one or any other process's) is given back, exactly as OpenVMS documents. */
struct vms_mbx_delmbx_args {
    uint32_t chan;      /* in: any channel assigned to the mailbox */
    uint32_t status;    /* out */
};

/* $QIO IO$_WRITEVBLK-equivalent: one message, moved whole (record-oriented
 * -- a mailbox never coalesces or splits writes). */
struct vms_mbx_write_args {
    uint32_t chan;      /* in */
    uint32_t len;       /* in: bytes in data (<= VMS_MBX_IOCTL_MAXLEN) */
    uint32_t status;    /* out */
    uint32_t pad;
    char     data[VMS_MBX_IOCTL_MAXLEN];
};

/* $QIO IO$_READVBLK-equivalent. Blocks (kernel wait queue) until a message
 * is queued -- see vms_ioctl_mbx_read()'s "INTERRUPTED WAITS" note, which
 * follows vms_eflag.c's WAITFR precedent exactly: no VMS status exists for
 * "the wait was interrupted", so none is invented (CLAUDE.md Rule 10). */
struct vms_mbx_read_args {
    uint32_t chan;      /* in */
    uint32_t bufsz;     /* in: caller's buffer size (<= VMS_MBX_IOCTL_MAXLEN) */
    uint32_t len;       /* out: actual message length */
    uint32_t status;    /* out */
    char     data[VMS_MBX_IOCTL_MAXLEN];
};

/*
 * $QIO IO$_SETMODE|IO$M_WRTATTN-equivalent: register a WRITE-ATTENTION AST on
 * this process's channel to the mailbox (vms-9003). When ANOTHER process writes
 * a message to the mailbox, the executive queues `astadr`(`astprm`) into THIS
 * process's AST queue at `acmode` -- the same executive AST queue $DCLAST and
 * the lock manager's completion/blocking ASTs use (src/kernel-core/vms_ast.c),
 * so the notification is real cross-process delivery through /dev/vms, not a
 * per-process fake (CLAUDE.md Rule 9 / INV-6).
 *
 * ONE-SHOT, RE-ARM ON SETMODE (public VSI OpenVMS I/O User's Reference, mailbox
 * driver): the AST fires once per SETMODE and must be re-established to fire
 * again. A fresh SETMODE|WRTATTN on the same channel REPLACES any still-armed
 * registration for that channel rather than stacking a second one.
 */
struct vms_mbx_wrtattn_args {
    uint32_t chan;      /* in: this process's channel to the mailbox */
    uint32_t acmode;    /* in: access mode to deliver the AST at (0-3) */
    uint64_t astadr;    /* in: write-attention AST routine (opaque address) */
    uint64_t astprm;    /* in: parameter passed to the AST routine */
    uint32_t status;    /* out: SS$_ status */
    uint32_t pad;
};

#define VMS_IOCTL_MBX_CREATE  _IOWR(VMS_IOC_MAGIC, 0x70, struct vms_mbx_create_args)
#define VMS_IOCTL_MBX_ASSIGN  _IOWR(VMS_IOC_MAGIC, 0x71, struct vms_mbx_assign_args)
#define VMS_IOCTL_MBX_WRITE   _IOWR(VMS_IOC_MAGIC, 0x72, struct vms_mbx_write_args)
#define VMS_IOCTL_MBX_READ    _IOWR(VMS_IOC_MAGIC, 0x73, struct vms_mbx_read_args)
#define VMS_IOCTL_MBX_DELMBX  _IOWR(VMS_IOC_MAGIC, 0x74, struct vms_mbx_delmbx_args)
#define VMS_IOCTL_MBX_SET_WRTATTN _IOWR(VMS_IOC_MAGIC, 0x75, struct vms_mbx_wrtattn_args)

/*
 * $DASSGN for a mailbox channel reuses VMS_IOCTL_DASSGN (vms_ioctl.h,
 * struct vms_dassgn_args) -- one $DASSGN ioctl for every channel kind, the
 * same channel-number space vms_devtab.c already allocates from
 * (proc->next_chan). vms_ioctl_dassgn() falls back to the mailbox channel
 * list when the channel is not one of the device table's.
 */

/*
 * Freeze the shared layouts -- see vms_lnm.h's identical note for why this
 * matters: both sides of /dev/vms compile these structs separately and
 * pass them across the boundary by raw address.
 */
_Static_assert(sizeof(struct vms_mbx_create_args) == 40,
               "vms_mbx_create_args changed size -- VMS_IOCTL_MBX_CREATE ABI break");
_Static_assert(sizeof(struct vms_mbx_assign_args) == 24,
               "vms_mbx_assign_args changed size -- VMS_IOCTL_MBX_ASSIGN ABI break");
_Static_assert(sizeof(struct vms_mbx_delmbx_args) == 8,
               "vms_mbx_delmbx_args changed size -- VMS_IOCTL_MBX_DELMBX ABI break");
_Static_assert(sizeof(struct vms_mbx_write_args) == 16 + VMS_MBX_IOCTL_MAXLEN,
               "vms_mbx_write_args changed size -- VMS_IOCTL_MBX_WRITE ABI break");
_Static_assert(sizeof(struct vms_mbx_read_args) == 16 + VMS_MBX_IOCTL_MAXLEN,
               "vms_mbx_read_args changed size -- VMS_IOCTL_MBX_READ ABI break");
_Static_assert(sizeof(struct vms_mbx_wrtattn_args) == 32,
               "vms_mbx_wrtattn_args changed size -- VMS_IOCTL_MBX_SET_WRTATTN ABI break");

#endif /* _VMS_MBX_H */
