/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_l2.h - Executive-resident L2 (raw datalink) socket surface (rd vms-7eb,
 * the auth slice of vms-1e4). Shared by the kernel module and userspace,
 * exactly like vms_bg.h: both sides compile these structures from this one
 * file and pass them across /dev/vms by raw address.
 *
 * WHAT THIS IS. The SCS cluster wire (ethertype 0x6007) is not carried over
 * TCP/IP -- OpenVMS clusters exchange SCS/NISCA traffic as RAW ETHERNET
 * FRAMES, one layer below BGn:/TCPIP$DEVICE:. On Linux the equivalent host
 * facility is an AF_PACKET/SOCK_RAW socket. A userspace process opening one
 * itself needs CAP_NET_RAW; OVMX processes are ordinary (non-root) Linux
 * tasks, so that path is closed by design. This ioctl surface is PIECE 1 of
 * the executive L2 datalink: the KERNEL (vms.ko) opens and owns the AF_PACKET
 * socket -- exactly the "a kernel socket bypasses the CAP_NET_RAW check a
 * userspace raw socket would face" posture exec_socket_create_icmp already
 * established for raw ICMP (vms-80b) -- and a VMS process reaches it through
 * $QIO-shaped ioctls, gated on the VMS PHY_IO privilege (the real VMS
 * privilege OpenVMS System Services Reference documents for physical I/O),
 * never on a Linux capability. This is the keystone that makes booted-OVMX
 * cluster membership possible without running the executive's SCS peers as
 * root.
 *
 * WHERE THE SOCKET LIVES. Executive-resident, the same posture as BGn:
 * (vms_bg.h) and mailboxes (vms_mbx.h): the AF_PACKET socket lives in vms.ko,
 * reached through the exec_l2_* seam (exec_kbackend.h SS13) whose Linux
 * backend is sock_create_kern(AF_PACKET, SOCK_RAW, ...) + a struct sockaddr_ll
 * bind (src/kernel/exec_kbackend_linux.h). There is NO userspace socket here.
 *
 * WHO OWNS WHAT. vms.ko owns the socket object. A process's HANDLE to an open
 * L2 socket is tracked per-process, in struct vms_proc's l2_channels list
 * (src/kernel-core/vms_l2.c) -- the same "the device is the executive's, the
 * handle is the process's" split BGn: and mailboxes use. A handle is an
 * OVMX-local number, not a $ASSIGN channel: L2 is not a VMS device class, so
 * it deliberately does not overload the shared channel/next_chan number space
 * BGn:/mailboxes/files draw from (vms_bg.h's rationale for sharing that space
 * does not apply here -- $DASSGN never touches an L2 handle).
 *
 * HONEST FAILURE (CLAUDE.md Rule 9 / INV-6). Every L2 operation routes
 * through /dev/vms. If the executive is absent, the userspace wrapper returns
 * SS$_NOSUCHDEV -- never a per-process fake that reports success while
 * opening a private userspace raw socket the rest of the system cannot see
 * (and could not open anyway without CAP_NET_RAW).
 *
 * AUTH GATE (vms-1e4). VMS_IOCTL_L2_OPEN requires the caller's process to
 * hold VMS_PRV_M_PHY_IO (the real, oracle-pinned VMS PHY_IO privilege bit --
 * see the comment beside VMS_PRV_V_PHY_IO below); a caller without it gets
 * SS$_NOPRIV, exactly as vms_lnm.c gates LNM$SYSTEM on SYSNAM and vms_mbx.c
 * gates a permanent mailbox on PRMMBX. This is what lets a NON-ROOT VMS
 * process do raw L2 I/O at all: the Linux-level CAP_NET_RAW gate is replaced
 * by a real, VMS-authentic privilege check the executive enforces.
 *
 * NAMING. This surface has no VMS device name of its own (no ASSIGN, no
 * $QIO): it is reached directly by ioctl, the same footing SETMODE/SETPRV/
 * SETEF etc. use in vms_ioctl.h's low ioctl bands. A later increment may wrap
 * it behind a VMS-visible device (design TBD, vms-1e4) -- PIECE 1 is the raw
 * kernel primitive + the privilege gate only.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8), stated rather than implied: the
 * per-frame transfer cap (VMS_L2_MAXLEN) and the handle numbering are OVMX's
 * own, the same posture VMS_BG_IOCTL_MAXLEN takes for BGn:. The ethertype
 * (0x6007, SCS) and the privilege this surface gates on (PHY_IO) are real,
 * documented VMS/public values, never invented.
 */

#ifndef _VMS_L2_H
#define _VMS_L2_H

/*
 * Relies on the integer typedefs and on _IOWR / VMS_IOC_MAGIC already being
 * in scope. vms_ioctl.h includes this file at its foot, after it has set
 * both up for the kernel and userspace builds alike (the same contract
 * vms_bg.h relies on).
 */

/* One L2 frame, either direction (OVMX design cap, not a VMS value -- see the
 * file header). 2048 covers a standard 1518-byte Ethernet frame (with room)
 * without paying for jumbo-frame headroom no cluster interconnect here needs
 * yet; a caller wanting more is out of scope for this first increment. */
#define VMS_L2_MAXLEN   2048u

/*
 * VMS_IOCTL_L2_OPEN -- open a kernel AF_PACKET/SOCK_RAW socket bound to
 * `ifname`/`ethertype` (exec_l2_open) and hand back an opaque per-process
 * handle plus the resolved interface index and hardware (MAC) address. Gated
 * on VMS_PRV_M_PHY_IO (see vms_l2.c); no privilege, no socket, SS$_NOPRIV.
 */
struct vms_l2_open_args {
    char     ifname[16];    /* in: interface name (e.g. "eth0"), NUL-padded */
    uint16_t ethertype;     /* in: L2 ethertype to bind, host order (e.g. 0x6007) */
    uint16_t pad0;          /* zero */
    uint32_t handle;        /* out: this process's L2 handle */
    uint32_t ifindex;       /* out: resolved interface index */
    uint8_t  hwaddr[6];     /* out: the bound interface's MAC */
    uint16_t pad1;          /* zero */
    uint32_t status;        /* out: SS$_ status */
};

/*
 * VMS_IOCTL_L2_SEND -- send one frame's payload out `handle` to `dst_mac` on
 * `ifindex` with the given `ethertype` (exec_l2_send). No peer/connect step:
 * every send names its destination, exactly like a raw datalink socket.
 */
struct vms_l2_send_args {
    uint32_t handle;             /* in */
    uint32_t ifindex;            /* in: destination interface index (from OPEN) */
    uint16_t ethertype;          /* in: host order */
    uint8_t  dst_mac[6];         /* in: destination hardware address */
    uint32_t len;                /* in: bytes to send (<= VMS_L2_MAXLEN);
                                  * out: bytes actually sent */
    uint8_t  data[VMS_L2_MAXLEN];
    uint32_t status;             /* out: SS$_ status */
};

/*
 * VMS_IOCTL_L2_RECV -- receive one frame from `handle` (exec_l2_recv),
 * honoring `timeout_ms` (0 = block indefinitely). `len` in is unused; `len`
 * out is the actual frame length the host kernel returned.
 */
struct vms_l2_recv_args {
    uint32_t handle;             /* in */
    uint32_t timeout_ms;         /* in: 0 = wait indefinitely */
    uint32_t len;                /* out: bytes actually received */
    uint8_t  data[VMS_L2_MAXLEN];
    uint32_t status;             /* out: SS$_ status */
};

/* VMS_IOCTL_L2_CLOSE -- release the handle and its host socket. */
struct vms_l2_close_args {
    uint32_t handle;        /* in */
    uint32_t status;        /* out */
};

#define VMS_IOCTL_L2_OPEN  _IOWR(VMS_IOC_MAGIC, 0x92, struct vms_l2_open_args)
#define VMS_IOCTL_L2_SEND  _IOWR(VMS_IOC_MAGIC, 0x93, struct vms_l2_send_args)
#define VMS_IOCTL_L2_RECV  _IOWR(VMS_IOC_MAGIC, 0x94, struct vms_l2_recv_args)
#define VMS_IOCTL_L2_CLOSE _IOWR(VMS_IOC_MAGIC, 0x95, struct vms_l2_close_args)

/*
 * Freeze the shared layouts -- see vms_bg.h's identical note for why this
 * matters: both sides of /dev/vms compile these structs separately and pass
 * them across the boundary by raw address, and _IOWR folds sizeof(struct)
 * into the request number, so a size change also renumbers the request.
 */
_Static_assert(sizeof(struct vms_l2_open_args) == 40,
               "vms_l2_open_args changed size -- VMS_IOCTL_L2_OPEN ABI break");
_Static_assert(sizeof(struct vms_l2_send_args) == 2072,
               "vms_l2_send_args changed size -- VMS_IOCTL_L2_SEND ABI break");
_Static_assert(sizeof(struct vms_l2_recv_args) == 2064,
               "vms_l2_recv_args changed size -- VMS_IOCTL_L2_RECV ABI break");
_Static_assert(sizeof(struct vms_l2_close_args) == 8,
               "vms_l2_close_args changed size -- VMS_IOCTL_L2_CLOSE ABI break");

_Static_assert(VMS_IOCTL_L2_OPEN == 0xC0285692u,
               "VMS_IOCTL_L2_OPEN encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_L2_SEND == 0xC8185693u,
               "VMS_IOCTL_L2_SEND encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_L2_RECV == 0xC8105694u,
               "VMS_IOCTL_L2_RECV encodes differently here than on the reference build");
_Static_assert(VMS_IOCTL_L2_CLOSE == 0xC0085695u,
               "VMS_IOCTL_L2_CLOSE encodes differently here than on the reference build");

#endif /* _VMS_L2_H */
