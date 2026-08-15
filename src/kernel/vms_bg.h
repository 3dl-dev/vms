/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bg.h - Executive-resident INET pseudo-device BGn: (vms-527)
 *
 * Shared by the kernel module and userspace, exactly like vms_mbx.h: both
 * sides compile these structures from this one file and pass them across
 * /dev/vms by raw address.
 *
 * WHAT THIS IS. On OpenVMS, TCP/IP Services present the network to a program
 * as a device -- BGn:, the INET pseudo-device -- reached the ordinary VMS way:
 * $ASSIGN a channel to TCPIP$DEVICE: (or BGn:), then $QIO on that channel
 * (IO$_SETMODE creates the socket, IO$_ACCESS connects, IO$_WRITEVBLK /
 * IO$_READVBLK move data, IO$_DEACCESS shuts down). BGn: is a KERNEL-MODE
 * DEVICE DRIVER in the executive, not a userspace library -- the socket it
 * fronts lives in the executive and the QIO functions are driver entry points.
 *
 * WHERE THE SOCKET LIVES (the design crux, operator 2026-08-14). The socket is
 * EXECUTIVE-RESIDENT: BGn: is the direct analogue of the mailbox (vms_mbx.h),
 * with a host-kernel `struct socket *` where the mailbox has a message queue.
 * $QIO to BGn: routes through vms.ko into the HOST kernel's in-kernel socket
 * API (Linux: sock_create_kern / kernel_connect / kernel_sendmsg /
 * kernel_recvmsg / kernel_sock_shutdown), so the IP/TCP stack is the host
 * kernel's -- OVMX never reimplements a transport (docs/design-tcpip-services-
 * ovmx.md §2, "the IP stack itself is Linux's"). There is NO userspace socket
 * layer: userspace is only the thin $QIO -> vms_kif_bg_* -> VMS_IOCTL_BG_*
 * dispatch, mirroring qio_mailbox_op -> vms_kif_mbx_* -> VMS_IOCTL_MBX_*
 * socket-for-queue.
 *
 * WHO OWNS WHAT (mirrors vms_mbx.h). vms.ko owns the socket object and the BGn:
 * unit number (src/kernel/vms_bg.c). A process's CHANNEL to a BG unit (from
 * $ASSIGN to TCPIP$DEVICE:) is tracked per-process, in struct vms_proc's
 * bg_channels list -- the same "the device is the executive's, the channel is
 * the process's" split the mailbox uses. This first increment (vms-527) is the
 * CLIENT path only and the socket is reached solely through its owning
 * process's channel; cross-process socket visibility (TCPIP SHOW
 * DEVICE_SOCKET) is a later phase (design §5, Phase 3), deliberately not built
 * here.
 *
 * HONEST FAILURE (CLAUDE.md Rule 9 / INV-6). Every BG operation routes through
 * /dev/vms. If the executive is absent, the userspace wrapper returns
 * SS$_NOSUCHDEV -- never a per-process fake that reports success while opening
 * a private userspace socket the rest of the system cannot see.
 *
 * NAMING (CLAUDE.md INV-0 / device-native-naming). BGn: / TCPIP$DEVICE: is the
 * VMS-visible INET TRANSPORT device (the SRI-QIO / INETDRIVER interface), which
 * is distinct from the NIC hardware device ETH0: (vms_devtab.c): BGn: layers
 * the IP stack over the NIC. Keep the two names apart.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), stated rather than implied: the
 * per-ioctl transfer cap (VMS_BG_IOCTL_MAXLEN) is OVMX's own number, the same
 * posture vms_mbx.h takes for VMS_MBX_IOCTL_MAXLEN. The BGn: unit / device name
 * form and the QIO-function-to-socket-call mapping are derived from the public
 * VSI OpenVMS TCP/IP Services documentation (docs.vmssoftware.com), never from
 * VSI/HPE source or a disassembly.
 */

#ifndef _VMS_BG_H
#define _VMS_BG_H

/*
 * Relies on the integer typedefs, on _IOWR / VMS_IOC_MAGIC and on
 * VMS_DEVNAM_SIZE already being in scope. vms_ioctl.h includes this file at its
 * foot, after it has set all of them up for the kernel and userspace builds
 * alike (the same contract vms_mbx.h relies on).
 */

/* A single BG send/recv transfer, in either direction (OVMX design cap, not a
 * VMS value -- see the file header). Large enough for typical connection
 * traffic; a caller wanting more is out of scope for vms-527's first
 * increment. */
#define VMS_BG_IOCTL_MAXLEN   4096u

/*
 * $ASSIGN to TCPIP$DEVICE: -- allocate a fresh BGn: unit and hand back a
 * channel to it (the direct analogue of $CREMBX's VMS_IOCTL_MBX_CREATE). No
 * socket is created yet; IO$_SETMODE (VMS_IOCTL_BG_SETMODE) creates it, exactly
 * as on VMS the socket is a set-mode step, not an assign step.
 */
struct vms_bg_create_args {
    uint32_t chan;          /* out: this process's channel number */
    uint32_t unit;          /* out: BGn: unit number */
    uint32_t status;        /* out: SS$_ status */
    char     devnam[VMS_DEVNAM_SIZE]; /* out: "BGn:" */
};

/*
 * The chan-only shape: IO$_SETMODE (create the socket), IO$_DEACCESS (shut the
 * connection down) and $DASSGN (release the channel + socket) all take just a
 * channel and return a status.
 */
struct vms_bg_chanonly_args {
    uint32_t chan;          /* in */
    uint32_t status;        /* out */
};

/*
 * IO$_ACCESS-equivalent: connect the channel's socket to a peer. The address is
 * carried as the first eight bytes of a sockaddr_in (family / port / v4 addr),
 * port and addr already in NETWORK byte order -- the executive hands them
 * straight to the host kernel's connect. IPv6 and richer socket-address item
 * lists are a later phase (design §4 L2).
 */
struct vms_bg_connect_args {
    uint32_t chan;          /* in */
    uint32_t status;        /* out */
    uint16_t sin_family;    /* in: AF_INET (2) */
    uint16_t sin_port;      /* in: network byte order */
    uint32_t sin_addr;      /* in: network byte order (e.g. 127.0.0.1) */
};

/*
 * IO$_WRITEVBLK / IO$_READVBLK-equivalent: move one buffer over the connection
 * (send / recv). Byte-stream, not record-oriented -- unlike a mailbox, TCP may
 * coalesce or split, so `len` on a read is the actual count the host kernel
 * returned, which a caller reads back through the IOSB byte count.
 */
struct vms_bg_io_args {
    uint32_t chan;          /* in */
    uint32_t len;           /* in: bytes to send / buffer size for recv;
                             * out: bytes actually moved */
    uint32_t status;        /* out */
    uint32_t pad;
    char     data[VMS_BG_IOCTL_MAXLEN];
};

/*
 * Readiness poll fd (vms-22a): hand back a REAL Linux pollable fd for the
 * channel's socket, so a userspace event loop (poll()/select(), e.g. OpenSSH's
 * clientloop/serverloop) can wait for the connection to become readable or
 * writable WITHOUT reading its data. The fd is READINESS-ONLY: it has no read/
 * write file ops, its .poll delegates to the executive socket's own readiness,
 * so the socket stays executive-resident and data still moves ONLY through
 * IO$_READVBLK / IO$_WRITEVBLK. This is what makes a VMS TCP/IP Services socket
 * select()-able, and it lets OpenSSH's poll() over the connection fd work
 * unchanged. OVMX design choice (Rule 8): the fd is our own readiness handle,
 * not a VMS-authentic object.
 */
struct vms_bg_pollfd_args {
    uint32_t chan;          /* in */
    int32_t  fd;            /* out: readiness-only pollable fd (>= 0), else -1 */
    uint32_t status;        /* out */
};

#define VMS_IOCTL_BG_CREATE   _IOWR(VMS_IOC_MAGIC, 0x80, struct vms_bg_create_args)
#define VMS_IOCTL_BG_SETMODE  _IOWR(VMS_IOC_MAGIC, 0x81, struct vms_bg_chanonly_args)
#define VMS_IOCTL_BG_CONNECT  _IOWR(VMS_IOC_MAGIC, 0x82, struct vms_bg_connect_args)
#define VMS_IOCTL_BG_SEND     _IOWR(VMS_IOC_MAGIC, 0x83, struct vms_bg_io_args)
#define VMS_IOCTL_BG_RECV     _IOWR(VMS_IOC_MAGIC, 0x84, struct vms_bg_io_args)
#define VMS_IOCTL_BG_DEACCESS _IOWR(VMS_IOC_MAGIC, 0x85, struct vms_bg_chanonly_args)
#define VMS_IOCTL_BG_DASSGN   _IOWR(VMS_IOC_MAGIC, 0x86, struct vms_bg_chanonly_args)
#define VMS_IOCTL_BG_POLLFD   _IOWR(VMS_IOC_MAGIC, 0x87, struct vms_bg_pollfd_args)

/*
 * Freeze the shared layouts -- see vms_mbx.h's identical note for why this
 * matters: both sides of /dev/vms compile these structs separately and pass
 * them across the boundary by raw address, and _IOWR folds sizeof(struct) into
 * the request number, so a size change also renumbers the request.
 */
_Static_assert(sizeof(struct vms_bg_create_args) == 12 + VMS_DEVNAM_SIZE,
               "vms_bg_create_args changed size -- VMS_IOCTL_BG_CREATE ABI break");
_Static_assert(sizeof(struct vms_bg_chanonly_args) == 8,
               "vms_bg_chanonly_args changed size -- VMS_IOCTL_BG_SETMODE/DEACCESS/DASSGN ABI break");
_Static_assert(sizeof(struct vms_bg_connect_args) == 16,
               "vms_bg_connect_args changed size -- VMS_IOCTL_BG_CONNECT ABI break");
_Static_assert(sizeof(struct vms_bg_io_args) == 16 + VMS_BG_IOCTL_MAXLEN,
               "vms_bg_io_args changed size -- VMS_IOCTL_BG_SEND/RECV ABI break");
_Static_assert(sizeof(struct vms_bg_pollfd_args) == 12,
               "vms_bg_pollfd_args changed size -- VMS_IOCTL_BG_POLLFD ABI break");

#endif /* _VMS_BG_H */
