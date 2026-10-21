// SPDX-License-Identifier: GPL-2.0
/*
 * vms_l2.c - Executive-resident L2 (raw datalink) socket facility (rd
 * vms-7eb, the auth slice of vms-1e4).
 *
 * See vms_l2.h for the full rationale (what this is, where the socket lives,
 * who owns what, the auth gate). In one line: this is PIECE 1 of the
 * executive L2 datalink -- it lets a NON-ROOT VMS process do raw L2
 * (AF_PACKET) I/O for the SCS cluster wire (ethertype 0x6007), because the
 * KERNEL owns the socket (no userspace CAP_NET_RAW needed) and the executive
 * gates opening it on the real VMS PHY_IO privilege instead.
 *
 * SUBSTRATE-NEUTRAL (rd vms-9394). This file names no host header: every
 * host-coupled operation goes through the seams -- the per-process handle
 * list through exec_list_* (exec_list.h), its guard through exec_lock_*,
 * user<->kernel copies through exec_copyin/exec_copyout, memory through
 * exec_zalloc/exec_free, and the datalink itself through exec_l2_* /
 * exec_socket_release (exec_kbackend.h SS13). So the SAME source builds into
 * the Linux vms.ko (whose exec_l2_* is a kernel AF_PACKET socket,
 * exec_kbackend_linux.h) and the NetBSD vms.kmod (whose exec_l2_* is an
 * in-kernel ifnet binding, src/kernel-netbsd/vms_socket_netbsd.c) -- the
 * PHY_IO gate, the station-address policy and the executive-owned source
 * field have exactly ONE implementation. On Linux every op expands to the
 * primitive this file used before (list_head / spin_lock / copy_from_user /
 * kzalloc), so the x86 build is behaviour-identical.
 *
 * LIFECYCLE. An L2 handle is minted ON DEMAND by VMS_IOCTL_L2_OPEN and lives
 * in the owning process's l2_channels list (struct vms_proc, vms_internal.h),
 * drawing its handle number from that process's own monotonic counter
 * (proc->l2_next_handle, under l2_lock) -- NOT from proc->next_chan: an L2
 * handle is not a VMS $ASSIGN channel (see vms_l2.h's NAMING note), so it does
 * not share that number space, and it is only ever looked up in its owner's
 * list, so per-process uniqueness is the whole requirement. VMS_IOCTL_L2_CLOSE releases the handle and its host socket; a
 * process that dies without an explicit CLOSE has every L2 handle reclaimed
 * by vms_l2_release_all() at teardown (called from vms_module.c, the same
 * place vms_bg_release_all() runs), so no socket leaks for the life of the
 * module.
 *
 * SCOPE (vms-7eb; rd vms-9394). OPEN / SEND / RECV / CLOSE only, reached
 * solely through the owning process's own handle. A handle is looked up under
 * l2_lock and its socket used after the lock is dropped (a host socket op can
 * sleep), so the lookup takes a socket reference for the op's duration: the
 * several threads of one NETACP may SEND and RECV on one handle concurrently,
 * and a CLOSE racing them frees nothing they are using. No fork/exec inheritance of L2 handles in this increment (out of
 * scope for the auth-gate slice) -- unlike BGn:'s vms-3bf/vms-0cd, there is no
 * forking-server case driving L2 yet.
 */

#include "vms_internal.h"   /* struct vms_proc / SS__* / VMS_PRV_M_PHY_IO and
                             * the struct vms_l2_* args (vms_l2.h, via
                             * vms_ioctl.h on Linux / vms_l2_nb.h on NetBSD) */
#include "exec_kbackend.h"  /* exec_lock/copy/alloc + the exec_l2_ seam (SS13) */
#include "exec_list.h"      /* the per-process handle list */

/* One process's L2 handle. The host socket hangs here as an exec_socket_t
 * (the SS12 holder, reused for L2 -- see exec_kbackend_linux.h's SS13 header),
 * keyed to the handle, exactly as a BG channel points at its socket. */
struct vms_l2_handle {
    exec_list_node_t list;      /* in proc->l2_channels */
    uint32_t handle;            /* this process's L2 handle number */
    uint32_t ifindex;           /* the bound interface's index (from OPEN) */
    exec_socket_t sock;         /* refcounted host AF_PACKET socket */
    uint8_t station[6];         /* the validated station (Ethernet SOURCE)
                                 * address every SEND on this handle carries
                                 * (rd vms-1f69; vms_l2_station.h) */
};

/* Caller holds proc->l2_lock. */
static struct vms_l2_handle *l2h_find_locked(struct vms_proc *proc, uint32_t handle)
{
    struct vms_l2_handle *h;

    exec_list_for_each_entry(h, &proc->l2_channels, list) {
        if (h->handle == handle)
            return h;
    }
    return NULL;
}

/* Find a handle and lift out its socket (and, if `station` is non-NULL, its
 * station address) under the lock, so the (possibly sleeping) host socket op
 * below runs with the lock dropped. The socket is returned with a REFERENCE
 * taken under the lock (rd vms-9394), which the caller drops with
 * exec_socket_release() when its op is done -- so a concurrent CLOSE from
 * another thread of the same process (NETACP sends from its FAL/CTERM worker
 * threads while its main loop sits in RECV) only drops the handle's own
 * reference and can never free the socket out from under an op in flight.
 * Returns the handle record (for its identity) and *sock; NULL means "no
 * such handle". */
static struct vms_l2_handle *l2h_lookup(struct vms_proc *proc, uint32_t handle,
                                        exec_socket_t *sock, uint8_t *station)
{
    struct vms_l2_handle *h;

    exec_lock(&proc->l2_lock);
    h = l2h_find_locked(proc, handle);
    *sock = h ? h->sock : NULL;
    if (h)
        exec_socket_get(h->sock);
    if (h && station)
        memcpy(station, h->station, sizeof(h->station));
    exec_unlock(&proc->l2_lock);
    return h;
}

/*
 * l2_priv_check - the PHY_IO gate VMS_IOCTL_L2_OPEN consults (vms-1e4, the
 * auth slice this item lands): OpenVMS System Services Reference / Guide to
 * System Security document PHY_IO as "may do physical I/O", and this is what
 * lets a NON-ROOT VMS process open a kernel-owned raw L2 socket at all -- the
 * Linux-level CAP_NET_RAW gate a userspace raw socket would face is replaced
 * by this real, VMS-authentic privilege check the executive enforces. Checked
 * against cur_privs (the ENABLED mask), exactly as vms_lnm.c's
 * lnm_priv_check() and vms_mbx.c's mbx_priv_check() gate every other
 * privileged mutation.
 */
static bool l2_priv_check(uint64_t cur_privs, uint32_t *status)
{
    bool ok = (cur_privs & VMS_PRV_M_PHY_IO) != 0;

    if (!ok)
        *status = SS__NOPRIV;
    return ok;
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

/*
 * VMS_IOCTL_L2_OPEN -- open a kernel AF_PACKET/SOCK_RAW socket bound to the
 * requested interface/ethertype (exec_l2_open), gated on PHY_IO. On success,
 * mints a fresh handle and hands back the resolved interface index and MAC.
 *
 * STATION ADDRESS (rd vms-1f69). args.station names the Ethernet SOURCE
 * address every frame sent on this handle will carry. All-zero (what every
 * pre-vms-1f69 caller passes, having memset its args) means the NIC's own
 * hwaddr -- the old behaviour. Otherwise it must be the NIC hwaddr or a
 * DECnet Phase IV algorithmic address AA-00-04-00-xx-xx
 * (vms_l2_station_check, vms_l2_station.h); anything else is refused
 * SS$_BADPARAM BEFORE a socket is opened, so a refused request mints nothing.
 * The check runs after the PHY_IO gate: an unprivileged caller learns nothing
 * about which addresses would have been accepted. Unicast addressed to an
 * accepted Phase IV station reaches the handle because exec_l2_open puts the
 * NIC in promiscuous mode on both substrates (exec_kbackend_linux.h,
 * vms-a84d; src/kernel-netbsd/vms_socket_netbsd.c, rd vms-9394) -- on Linux
 * the same reason the cluster wire's aa:00:04:00 SCA address is received.
 */
long vms_ioctl_l2_open(struct vms_proc *proc, unsigned long arg)
{
    struct vms_l2_open_args args;
    struct vms_l2_handle *h;
    exec_socket_t sock;
    uint32_t ifindex = 0;
    uint8_t nic[6];
    int rc;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;
    args.stv = 0;

    if (!l2_priv_check(proc->cur_privs, &args.status))
        goto out;

    /* args.ifname is a fixed 16-byte field (IFNAMSIZ); force-terminate so a
     * full, non-NUL-terminated 16 bytes from userspace cannot run the backend
     * past the buffer. */
    args.ifname[sizeof(args.ifname) - 1] = '\0';

    /* Resolve the NIC's hwaddr first: it is both the default station and the
     * one non-Phase-IV address a caller may name. No such interface -> the
     * same honest SS$_NOSUCHDEV exec_l2_open would have returned. */
    memset(nic, 0, sizeof(nic));
    if (exec_l2_hwaddr(args.ifname, nic) != 0) {
        args.status = SS__NOSUCHDEV;
        goto out;
    }
    switch (vms_l2_station_check(args.station, nic)) {
    case VMS_L2_STATION_USE_NIC:
        memcpy(args.station, nic, sizeof(args.station));
        break;
    case VMS_L2_STATION_OK:
        break;
    default:
        args.status = SS__BADPARAM;   /* not the NIC, not Phase IV: refused */
        memset(args.station, 0, sizeof(args.station));
        goto out;
    }

    rc = exec_l2_open(args.ifname, args.ethertype, &ifindex, &sock);
    if (rc) {
        /* rd vms-b72: the host errno behind it is the STV. */
        args.status = (rc == -ENODEV) ? SS__NOSUCHDEV : SS__ABORT;
        args.stv = (uint16_t)(rc < 0 ? -rc : rc);
        goto out;
    }

    h = exec_zalloc(sizeof(*h));
    if (!h) {
        exec_socket_release(sock);
        return -ENOMEM;
    }
    h->ifindex = ifindex;
    h->sock = sock;
    memcpy(h->station, args.station, sizeof(h->station));

    exec_lock(&proc->l2_lock);
    h->handle = ++proc->l2_next_handle;
    exec_list_add_tail(&h->list, &proc->l2_channels);
    exec_unlock(&proc->l2_lock);

    args.handle = h->handle;
    args.ifindex = ifindex;
    memcpy(args.hwaddr, nic, sizeof(args.hwaddr));
    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * VMS_IOCTL_L2_SEND -- send one frame out `handle` to `dst_mac` on `ifindex`
 * (exec_l2_send). No connect step: every send names its destination.
 *
 * The frame is a COMPLETE Ethernet frame (dst | src | ethertype | payload) --
 * exec_l2_send transmits it verbatim (vms-a84d). The executive OWNS its source
 * field: bytes 6..11 are overwritten with the handle's validated station
 * address (rd vms-1f69), so a PHY_IO holder cannot source a frame from an
 * address OPEN never approved. A frame too short to carry an Ethernet header
 * is refused SS$_BADPARAM.
 */
long vms_ioctl_l2_send(struct vms_proc *proc, unsigned long arg)
{
    struct vms_l2_send_args *a;
    exec_socket_t sock;
    uint8_t station[6];
    long ret = 0;
    long n;

    a = exec_zalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;
    if (exec_copyin(a, (const void *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (a->len > VMS_L2_MAXLEN || a->len < 14u) {
        a->status = SS__BADPARAM;   /* over the cap, or no room for the header */
        goto out_copy;
    }

    if (!l2h_lookup(proc, a->handle, &sock, station)) {
        a->status = SS__IVCHAN;   /* no such L2 handle */
        goto out_copy;
    }
    memcpy(a->data + 6, station, sizeof(station));   /* the executive's source */

    n = exec_l2_send(sock, (int)a->ifindex, a->ethertype, a->dst_mac, a->data, a->len);
    exec_socket_release(sock);   /* the lookup's reference */
    if (n < 0) {
        a->len = 0;
        a->status = SS__ABORT;
    } else {
        a->len = (uint32_t)n;
        a->status = SS__NORMAL;
    }

out_copy:
    if (exec_copyout((void *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    exec_free(a);
    return ret;
}

/*
 * VMS_IOCTL_L2_RECV -- receive one frame from `handle` (exec_l2_recv),
 * honoring `timeout_ms`. `len` out is the actual frame length received.
 */
long vms_ioctl_l2_recv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_l2_recv_args *a;
    exec_socket_t sock;
    long ret = 0;
    size_t n = 0;
    int rc;

    a = exec_zalloc(sizeof(*a));
    if (!a)
        return -ENOMEM;
    if (exec_copyin(a, (const void *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (!l2h_lookup(proc, a->handle, &sock, NULL)) {
        a->status = SS__IVCHAN;   /* no such L2 handle */
        goto out_copy;
    }

    rc = exec_l2_recv(sock, a->data, VMS_L2_MAXLEN, a->timeout_ms, &n);
    exec_socket_release(sock);   /* the lookup's reference */
    if (rc) {
        a->len = 0;
        a->status = SS__ABORT;
    } else {
        a->len = (uint32_t)n;
        a->status = SS__NORMAL;
    }

out_copy:
    if (exec_copyout((void *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    exec_free(a);
    return ret;
}

/*
 * VMS_IOCTL_L2_CLOSE -- release the handle and its host socket.
 */
long vms_ioctl_l2_close(struct vms_proc *proc, unsigned long arg)
{
    struct vms_l2_close_args args;
    struct vms_l2_handle *h;

    memset(&args, 0, sizeof(args));
    if (exec_copyin(&args, (const void *)arg, sizeof(args)))
        return -EFAULT;

    exec_lock(&proc->l2_lock);
    h = l2h_find_locked(proc, args.handle);
    if (h)
        exec_list_del(&h->list);
    exec_unlock(&proc->l2_lock);

    if (!h) {
        args.status = SS__IVCHAN;
        goto out;
    }

    exec_socket_release(h->sock);
    exec_free(h);
    args.status = SS__NORMAL;

out:
    if (exec_copyout((void *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * Give back every L2 handle a dying process holds, releasing each host
 * socket -- the L2 counterpart of vms_bg_release_all(), called from
 * vms_module.c's process-teardown path so a handle whose owner exits without
 * an explicit CLOSE is still reclaimed.
 */
void vms_l2_release_all(struct vms_proc *proc)
{
    struct vms_l2_handle *h, *tmp;
    EXEC_LIST_HEAD(doomed);

    exec_lock(&proc->l2_lock);
    exec_list_for_each_entry_safe(h, tmp, &proc->l2_channels, list)
        exec_list_move(&h->list, &doomed);
    exec_unlock(&proc->l2_lock);

    exec_list_for_each_entry_safe(h, tmp, &doomed, list) {
        exec_list_del(&h->list);
        exec_socket_release(h->sock);
        exec_free(h);
    }
}
