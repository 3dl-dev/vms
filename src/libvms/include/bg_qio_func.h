/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bg_qio_func.h - the $QIO function codes of an INET pseudo-device (BGn:)
 * channel, ONE source for the two callers that issue them (rd vms-99f):
 *
 *   - src/libvms/syssvc/sys_qio.c        sys$qio / sys$qiow on a BG channel
 *     (LIBVMS$SHR), after mapping the process channel to the executive's;
 *   - src/vmstcpip/sockets/ovmx_decc_sockets.c   the C RTL's sockets
 *     (DECC$SHR), which hold executive channels directly because DECC$SHR has
 *     no --use of LIBVMS$SHR (every OVMX image is linked against DECC$SHR
 *     first) -- the same reason vms_kif_bg_ops.h is a shared fragment.
 *
 * NOT a normal header: a fragment defining one static function. The includer
 * supplies the vms_kif_bg_* entry points (vms_kif.h declarations, or the
 * vms_kif_bg_ops.h definitions), iodef.h and ssdef.h.
 *
 * THE FUNCTION MAP (the SRI-QIO / INETDRIVER interface, docs/design-tcpip-
 * services-ovmx.md section 4 L2, with the OVMX parameter layouts iodef.h labels):
 *
 *   IO$_SETMODE / IO$_SETCHAR
 *       P4 alone                    -> apply the socket-option list (vms-a9e7)
 *       P1 = sockaddr               -> bind (effective address written back)
 *       P3 = backlog                -> listen
 *       P2 = IO$K_SOCK_ICMP / _UDP  -> create a raw ICMP / UDP socket
 *       otherwise                   -> create the TCP socket
 *       (P4 with any of these: create/bind/listen, then apply the options)
 *   IO$_SENSEMODE   P3 = IO$K_BG_SENSE_*: interface list, readiness wait,
 *                   local / peer name; P3 = 0 with P4 = an option list:
 *                   getsockopt into each entry
 *   IO$_ACCESS      P1 = sockaddr   -> connect
 *   IO$_ACCESS|IO$M_ACCEPT  acc_exec_chan = the empty channel that receives
 *                   the connection, P1 (optional) = peer sockaddr out
 *   IO$_WRITEVBLK   P1/P2 -> send ALL P2 bytes (in executive-sized pieces)
 *   IO$_READVBLK    P1/P2 -> receive what is there (0 = orderly close)
 *   IO$_DEACCESS    -> shut the connection down
 *
 * A caller that must not block (a non-blocking socket) first issues the
 * IO$K_BG_SENSE_READY sense; no function modifier is overloaded for it (bit 6
 * of a BGn: write is IO$M_INTERRUPT, out-of-band data, not "now").
 */
#ifndef BG_QIO_FUNC_H
#define BG_QIO_FUNC_H

struct bg_qio_sockaddr_in {
    uint16_t family;    /* AF_INET */
    uint16_t port;      /* network byte order */
    uint32_t addr;      /* network byte order (IPv4) */
};

/* Linux/BSD option levels the executive's socket option request takes. */
#define BG_QIO_SOL_SOCKET   1
#define BG_QIO_IPPROTO_IP   0
#define BG_QIO_IPPROTO_TCP  6

/* The level an option list's TYPE names, or -1. */
static int bg_qio_optlevel(uint16_t type)
{
    switch (type) {
    case TCPIP$C_SOCKOPT: return BG_QIO_SOL_SOCKET;
    case TCPIP$C_TCPOPT:  return BG_QIO_IPPROTO_TCP;
    case TCPIP$C_IPOPT:   return BG_QIO_IPPROTO_IP;
    default:              return -1;
    }
}

/*
 * IO$_SETMODE / IO$_SENSEMODE P4: a socket-option list (iodef.h, vms-a9e7 +
 * vms-99f). P4 is the address of an item_list_2 descriptor whose type names the
 * level and whose address/length name an array of item_list_2 entries, one per
 * option: type = option name, address = its value. TCPIP$C_RCVTIMEO (a timeval)
 * maps to the executive's receive timeout; every other entry is an int (length
 * 4). `get` fills each entry from the live socket instead of applying it. A
 * malformed list or an option the socket refuses is SS$_BADPARAM -- never
 * silently swallowed.
 */
static uint32_t bg_qio_sockopts(uint32_t exec_chan, uintptr_t p4, int get)
{
    const struct tcpip_item_list_2 *desc = (const struct tcpip_item_list_2 *)p4;
    const struct tcpip_item_list_2 *opt;
    uint32_t n, i, st;
    int level;

    level = bg_qio_optlevel(desc->type);
    if (level < 0 || desc->address == NULL || desc->length == 0 ||
        desc->length % sizeof(struct tcpip_item_list_2) != 0)
        return SS$_BADPARAM;
    opt = (const struct tcpip_item_list_2 *)desc->address;
    n = desc->length / (uint32_t)sizeof(struct tcpip_item_list_2);
    for (i = 0; i < n; i++) {
        if (opt[i].address == NULL)
            return SS$_BADPARAM;
        if (level == BG_QIO_SOL_SOCKET && opt[i].type == TCPIP$C_RCVTIMEO) {
            struct tcpip_timeval32 *tv = (struct tcpip_timeval32 *)opt[i].address;
            if (opt[i].length < sizeof(*tv))
                return SS$_BADPARAM;
            if (get) {
                int ms = 0;
                st = vms_kif_bg_getsockopt(exec_chan, VMS_BG_SOL_EXEC,
                                           VMS_BG_SO_RCVTIMEO_MS, &ms);
                if (!(st & 1)) return st;
                tv->tv_sec = ms / 1000;
                tv->tv_usec = (ms % 1000) * 1000;
            } else {
                uint64_t ms;
                if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
                    return SS$_BADPARAM;
                ms = (uint64_t)tv->tv_sec * 1000u + (uint64_t)tv->tv_usec / 1000u;
                if (ms == 0 && tv->tv_usec > 0)
                    ms = 1;                 /* sub-ms timeout: still a timeout */
                if (ms > 0x7fffffffu)
                    ms = 0x7fffffffu;
                st = vms_kif_bg_setsockopt(exec_chan, VMS_BG_SOL_EXEC,
                                           VMS_BG_SO_RCVTIMEO_MS, (int)ms);
                if (!(st & 1)) return st;
            }
            continue;
        }
        if (opt[i].length != sizeof(int32_t))
            return SS$_BADPARAM;            /* integer options only */
        if (get)
            st = vms_kif_bg_getsockopt(exec_chan, level, opt[i].type,
                                       (int *)opt[i].address);
        else
            st = vms_kif_bg_setsockopt(exec_chan, level, opt[i].type,
                                       *(const int *)opt[i].address);
        if (!(st & 1))
            return st;
    }
    return SS$_NORMAL;
}

static uint32_t bg_qio_func(uint32_t exec_chan, uint32_t func, void *p1,
                            uint32_t p2, uint32_t p3, uintptr_t p4,
                            uint32_t acc_exec_chan, uint32_t *actlen)
{
    uint32_t base = func & IO$M_FCODE;
    uint32_t st;

    *actlen = 0;
    switch (base) {
    case IO$_SETMODE:
    case IO$_SETCHAR:
        if (p4 && !p1 && p3 == 0 && p2 == 0)
            return bg_qio_sockopts(exec_chan, p4, 0);   /* options only */
        if (p1) {
            struct bg_qio_sockaddr_in *sa = (struct bg_qio_sockaddr_in *)p1;
            uint16_t eport = 0;
            uint32_t eaddr = 0;
            st = vms_kif_bg_bind(exec_chan, sa->family, sa->port, sa->addr,
                                 &eport, &eaddr);
            if (st & 1) { sa->port = eport; sa->addr = eaddr; }
        } else if (p3 != 0) {
            st = vms_kif_bg_listen(exec_chan, (int)p3);
        } else if (p2 == IO$K_SOCK_ICMP) {
            st = vms_kif_bg_setmode_icmp(exec_chan);
        } else if (p2 == IO$K_SOCK_UDP) {
            st = vms_kif_bg_setmode_udp(exec_chan);
        } else {
            st = vms_kif_bg_setmode(exec_chan);
        }
        if ((st & 1) && p4)
            st = bg_qio_sockopts(exec_chan, p4, 0);    /* create/bind + options */
        return st;

    case IO$_SENSEMODE:
    case IO$_SENSECHAR:
        switch (p3) {
        case IO$K_BG_SENSE_IFLIST:
            if (!p1) return SS$_BADPARAM;
            return vms_kif_bg_iflist(exec_chan, (struct vms_bg_ifent *)p1,
                                     p2 / (uint32_t)sizeof(struct vms_bg_ifent),
                                     actlen);
        case IO$K_BG_SENSE_ROUTES:
            if (!p1) return SS$_BADPARAM;
            return vms_kif_bg_routes(exec_chan, (struct vms_bg_rtent *)p1,
                                     p2 / (uint32_t)sizeof(struct vms_bg_rtent),
                                     actlen);
        case IO$K_BG_SENSE_READY:
            if (!p1 && p2) return SS$_BADPARAM;
            return vms_kif_bg_select((struct vms_bg_selent *)p1, p2,
                                     (int32_t)(intptr_t)p4, actlen);
        case IO$K_BG_SENSE_LOCAL:
        case IO$K_BG_SENSE_PEER: {
            struct bg_qio_sockaddr_in *sa = (struct bg_qio_sockaddr_in *)p1;
            uint16_t fam = 0, port = 0;
            uint32_t a4 = 0;
            if (!sa) return SS$_BADPARAM;
            st = vms_kif_bg_getname(exec_chan, p3 == IO$K_BG_SENSE_PEER ? 1u : 0u,
                                    &fam, &port, &a4);
            if (st & 1) { sa->family = fam; sa->port = port; sa->addr = a4; }
            return st;
        }
        case 0:
            if (!p4) return SS$_BADPARAM;
            return bg_qio_sockopts(exec_chan, p4, 1);  /* getsockopt */
        default:
            return SS$_BADPARAM;
        }

    case IO$_ACCESS:
        if (func & IO$M_ACCEPT) {
            uint16_t fam = 0, port = 0;
            uint32_t a4 = 0;
            if (acc_exec_chan == 0)
                return SS$_BADPARAM;
            st = vms_kif_bg_accept(exec_chan, acc_exec_chan, &fam, &port, &a4);
            if ((st & 1) && p1 && p2 >= sizeof(struct bg_qio_sockaddr_in)) {
                struct bg_qio_sockaddr_in *sa = (struct bg_qio_sockaddr_in *)p1;
                sa->family = fam; sa->port = port; sa->addr = a4;
            }
            return st;
        }
        if (!p1 || p2 < sizeof(struct bg_qio_sockaddr_in))
            return SS$_BADPARAM;
        {
            const struct bg_qio_sockaddr_in *sa =
                (const struct bg_qio_sockaddr_in *)p1;
            return vms_kif_bg_connect(exec_chan, sa->family, sa->port, sa->addr);
        }

    case IO$_WRITEVBLK:
    case IO$_WRITELBLK:
    case IO$_WRITEPBLK: {
        const char *b = (const char *)p1;
        uint32_t done = 0;
        if (!p1) return SS$_BADPARAM;
        st = SS$_NORMAL;
        while (done < p2) {
            uint32_t piece = p2 - done, n = 0;
            if (piece > VMS_BG_IOCTL_MAXLEN) piece = VMS_BG_IOCTL_MAXLEN;
            st = vms_kif_bg_send(exec_chan, b + done, piece, &n);
            if (!(st & 1)) break;
            done += n;
            if (n == 0) break;
        }
        *actlen = done;
        return (done > 0) ? SS$_NORMAL : st;
    }

    case IO$_READVBLK:
    case IO$_READLBLK:
    case IO$_READPBLK:
        if (!p1) return SS$_BADPARAM;
        /* IO$M_NOW (rd vms-a6b): complete at once; SS$_ENDOFFILE and a zero
         * count when nothing has arrived. A caller multiplexing the
         * connection with a terminal waits on an event flag/timer and
         * reads with NOW -- never poll() on a Linux fd. */
        return vms_kif_bg_recv_ex(exec_chan, p1, p2, actlen,
                                  (func & IO$M_NOW) != 0);

    case IO$_DEACCESS:
        return vms_kif_bg_deaccess(exec_chan);

    case IO$_NOP:
        return SS$_NORMAL;

    default:
        return SS$_ILLIOFUNC;
    }
}

#endif /* BG_QIO_FUNC_H */
