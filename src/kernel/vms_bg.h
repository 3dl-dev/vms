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

/*
 * IO$_SENSEMODE (getsockname / getpeername) -- report the channel socket's
 * LOCAL (which==0) or PEER (which==1) address, straight from the host kernel
 * socket via kernel_getsockname / kernel_getpeername. The answer is the REAL
 * kernel-socket endpoint, never a userspace guess: this is what lets an
 * unmodified OpenSSH's getpeername() record the true remote IP for known_hosts
 * (OpenSSH de-veneer Tier A, vms-4bf). The address is returned as the same
 * 8-byte AF_INET tuple IO$_ACCESS accepts (family + network-order port + v4
 * addr); IPv6 is a later phase.
 */
struct vms_bg_name_args {
    uint32_t chan;          /* in */
    uint32_t which;         /* in: 0 = getsockname (local), 1 = getpeername (peer) */
    uint32_t status;        /* out */
    uint16_t sin_family;    /* out: AF_INET (2) */
    uint16_t sin_port;      /* out: network byte order */
    uint32_t sin_addr;      /* out: network byte order */
};

/*
 * IO$_SETMODE / IO$_SENSEMODE socket-option subfunction (setsockopt /
 * getsockopt) -- carry a single INTEGER-valued option to/from the REAL host
 * kernel socket. On SET the value is applied through the socket's own
 * ->setsockopt (so SO_KEEPALIVE / TCP_NODELAY / IP_TOS / SO_REUSEADDR take
 * genuine effect on the kernel socket, NOT swallowed); on GET the executive
 * reads the option back from the live socket state. Only the small
 * integer-option whitelist OpenSSH sets is honored; anything else returns
 * SS$_BADPARAM (the veneer maps that to ENOPROTOOPT), an honest "unsupported",
 * never a fake success. OVMX design choice (Rule 8): the int-only wire form is
 * ours; the option semantics are the host kernel's.
 */
struct vms_bg_sockopt_args {
    uint32_t chan;          /* in */
    uint32_t op;            /* in: 0 = setsockopt, 1 = getsockopt */
    int32_t  level;         /* in: SOL_SOCKET / IPPROTO_TCP / IPPROTO_IP */
    int32_t  optname;       /* in */
    int32_t  optval;        /* in for set / out for get (integer options only) */
    uint32_t status;        /* out */
};

/*
 * Server path (vms-698, OpenSSH server port). IO$_SETMODE(bind) binds the
 * channel's socket to a local AF_INET address and reads the EFFECTIVE address
 * back (a zero port yields an ephemeral one, returned so a server learns its
 * port). Same 8-byte AF_INET tuple as IO$_ACCESS, port/addr in network order.
 */
struct vms_bg_bind_args {
    uint32_t chan;          /* in */
    uint32_t status;        /* out */
    uint16_t sin_family;    /* in / out: AF_INET (2) */
    uint16_t sin_port;      /* in (0 = ephemeral) / out: effective, network order */
    uint32_t sin_addr;      /* in / out: network order (0 = INADDR_ANY) */
};

/* IO$_SETMODE(listen) -- mark the socket passive with a backlog. */
struct vms_bg_listen_args {
    uint32_t chan;          /* in */
    int32_t  backlog;       /* in */
    uint32_t status;        /* out */
    uint32_t pad;
};

/*
 * IO$_ACCESS|IO$M_ACCEPT -- block for one inbound connection on listen_chan and
 * install the accepted socket onto accept_chan (a SECOND BG channel the caller
 * $ASSIGNed empty); the peer address is returned. The accepted connection is
 * then used via ordinary IO$_READVBLK/WRITEVBLK on accept_chan.
 */
struct vms_bg_accept_args {
    uint32_t listen_chan;   /* in: the listening channel */
    uint32_t accept_chan;   /* in: the pre-$ASSIGNed empty channel to receive it */
    uint32_t status;        /* out */
    uint16_t sin_family;    /* out: peer AF_INET (2) */
    uint16_t sin_port;      /* out: peer port, network order */
    uint32_t sin_addr;      /* out: peer v4 addr, network order */
    uint32_t pad;
};

/*
 * Materialize a BG channel as a REAL, DATA-carrying Linux fd (vms-0cd, RUNG-3b).
 * Unlike the readiness-only poll fd above, this fd has real .read/.write ops that
 * route straight to the channel's executive-resident socket (the SAME
 * exec_socket_send/recv the IO$_READVBLK/WRITEVBLK handlers use) -- so DATA STILL
 * TRANSITS THE EXECUTIVE; it is NOT a host socket handed to userspace, and NOT an
 * AF_UNIX socketpair (INV-6). It exists because a ported Unix daemon (sshd) hands a
 * connection to its child by dup2()'ing it onto stdin/stdout and then execv()'ing;
 * a veneer HANDLE is not a real fd, so dup2 fails. This gives back a real fd that
 * is dup2-able and -- crucially -- has NO O_CLOEXEC, so it survives execve into the
 * child, whose ordinary read()/write() on fd 0/1 then reach the executive socket
 * through this fd's fops. OVMX design choice (Rule 8): the materialized fd is our
 * own vms.ko-backed handle, labelled "[bgconn]", not a VMS-authentic object. Layout
 * mirrors vms_bg_pollfd_args (chan in, fd out, status out).
 */
struct vms_bg_datafd_args {
    uint32_t chan;          /* in */
    int32_t  fd;            /* out: real data-carrying fd (>= 0), else -1 */
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
#define VMS_IOCTL_BG_GETNAME  _IOWR(VMS_IOC_MAGIC, 0x88, struct vms_bg_name_args)
#define VMS_IOCTL_BG_SOCKOPT  _IOWR(VMS_IOC_MAGIC, 0x89, struct vms_bg_sockopt_args)
#define VMS_IOCTL_BG_BIND     _IOWR(VMS_IOC_MAGIC, 0x8a, struct vms_bg_bind_args)
#define VMS_IOCTL_BG_LISTEN   _IOWR(VMS_IOC_MAGIC, 0x8b, struct vms_bg_listen_args)
#define VMS_IOCTL_BG_ACCEPT   _IOWR(VMS_IOC_MAGIC, 0x8c, struct vms_bg_accept_args)
#define VMS_IOCTL_BG_MATERIALIZE_FD _IOWR(VMS_IOC_MAGIC, 0x8d, struct vms_bg_datafd_args)

/*
 * Socket-name / socket-option surface for a MATERIALIZED [bgconn] fd (vms-0cd).
 * These are issued on the materialized data fd ITSELF (its .unlocked_ioctl), NOT on
 * /dev/vms: after a wrapped daemon dup2()s the connection onto stdin/stdout and
 * execs its child, that child holds only the real [bgconn] fd -- with no BG channel
 * of its own -- yet still calls getpeername()/getsockname()/setsockopt()/
 * getsockopt() on it. The handler answers them from the fd's HELD exec_socket_t
 * (the SAME executive socket carrying the bytes), so the peer is the TRUE accepted-
 * connection peer, never a synthesized value. Reuses the BG name/sockopt arg
 * layouts (the `chan` field is unused here -- the fd is the socket).
 */
#define VMS_IOCTL_BGCONN_GETNAME  _IOWR(VMS_IOC_MAGIC, 0x90, struct vms_bg_name_args)
#define VMS_IOCTL_BGCONN_SOCKOPT  _IOWR(VMS_IOC_MAGIC, 0x91, struct vms_bg_sockopt_args)

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
_Static_assert(sizeof(struct vms_bg_name_args) == 20,
               "vms_bg_name_args changed size -- VMS_IOCTL_BG_GETNAME ABI break");
_Static_assert(sizeof(struct vms_bg_sockopt_args) == 24,
               "vms_bg_sockopt_args changed size -- VMS_IOCTL_BG_SOCKOPT ABI break");
_Static_assert(sizeof(struct vms_bg_bind_args) == 16,
               "vms_bg_bind_args changed size -- VMS_IOCTL_BG_BIND ABI break");
_Static_assert(sizeof(struct vms_bg_listen_args) == 16,
               "vms_bg_listen_args changed size -- VMS_IOCTL_BG_LISTEN ABI break");
_Static_assert(sizeof(struct vms_bg_accept_args) == 24,
               "vms_bg_accept_args changed size -- VMS_IOCTL_BG_ACCEPT ABI break");
_Static_assert(sizeof(struct vms_bg_datafd_args) == 12,
               "vms_bg_datafd_args changed size -- VMS_IOCTL_BG_MATERIALIZE_FD ABI break");

#endif /* _VMS_BG_H */
