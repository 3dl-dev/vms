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
 * WHY THIS FILE IS IN src/kernel-core/ BUT IS NOT (YET) SUBSTRATE-NEUTRAL.
 * Structurally this facility mirrors every other kernel-core facility -- it
 * calls exec_l2_* and exec_socket_release through the seam and names no
 * <linux/net...> socket header of its own. But, exactly like vms_bg.c (whose
 * own header explains this same posture), it still drives its per-process
 * handle list with raw Linux <linux/list.h> + spinlock primitives rather than
 * the substrate-neutral exec_list_ / exec_lock_ seam, so it is -- like
 * vms_bg.c -- a LINUX BUILD for now: this file is not in the NetBSD kmodule's
 * SRCS (src/kernel-netbsd/Makefile), and struct vms_proc's NetBSD twin
 * carries no l2_channels field. The NetBSD exec_l2_* backend (vms_socket_
 * netbsd.c) is a type-checked, unreferenced contract-only twin until a
 * genuine NetBSD L2 binding (BPF, not a socket at all) lands -- see that
 * file's header. This mirrors vms_bg.c's "stays a Linux build for now"
 * posture exactly, for the exact same reason (a host-socket-coupled
 * per-process list that has not yet been ported to the exec_list_* seam).
 *
 * LIFECYCLE. An L2 handle is minted ON DEMAND by VMS_IOCTL_L2_OPEN and lives
 * in the owning process's l2_channels list (struct vms_proc, vms_internal.h),
 * drawing its handle number from a module-global monotonic counter (like
 * vms_bg_next_unit) -- NOT from proc->next_chan: an L2 handle is not a VMS
 * $ASSIGN channel (see vms_l2.h's NAMING note), so it does not share that
 * number space. VMS_IOCTL_L2_CLOSE releases the handle and its host socket; a
 * process that dies without an explicit CLOSE has every L2 handle reclaimed
 * by vms_l2_release_all() at teardown (called from vms_module.c, the same
 * place vms_bg_release_all() runs), so no socket leaks for the life of the
 * module.
 *
 * FIRST-INCREMENT SCOPE (vms-7eb). OPEN / SEND / RECV / CLOSE only, reached
 * solely through the owning process's own handle, exercised synchronously by
 * one thread -- the same scope limits vms_bg.c's header documents for BGn:'s
 * first increment, for the same reason (a handle is looked up under l2_lock
 * and its socket used after the lock is dropped, since a host socket op can
 * sleep). No fork/exec inheritance of L2 handles in this increment (out of
 * scope for the auth-gate slice) -- unlike BGn:'s vms-3bf/vms-0cd, there is no
 * forking-server case driving L2 yet.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/errno.h>

#include "vms_internal.h"   /* struct vms_proc / SS__* / VMS_PRV_M_PHY_IO and,
                             * via vms_ioctl.h, the struct vms_l2_* args */
#include "exec_kbackend.h"  /* the exec_l2_/exec_socket_ host-socket seam (SS13) */

/* One process's L2 handle. The host socket hangs here as an exec_socket_t
 * (the SS12 holder, reused for L2 -- see exec_kbackend_linux.h's SS13 header),
 * keyed to the handle, exactly as a BG channel points at its socket. */
struct vms_l2_handle {
    struct list_head list;      /* in proc->l2_channels */
    uint32_t handle;            /* this process's L2 handle number */
    uint32_t ifindex;           /* the bound interface's index (from OPEN) */
    exec_socket_t sock;         /* refcounted host AF_PACKET socket */
};

/* L2 handle numbers are node-wide and monotonic, like vms_bg_next_unit for
 * BGn: units -- an OVMX numbering choice (vms_l2.h): a handle is opaque to the
 * program, which passes it back verbatim, not a number it interprets. */
static atomic_t vms_l2_next_handle = ATOMIC_INIT(0);

/* Caller holds proc->l2_lock. */
static struct vms_l2_handle *l2h_find_locked(struct vms_proc *proc, uint32_t handle)
{
    struct vms_l2_handle *h;

    list_for_each_entry(h, &proc->l2_channels, list) {
        if (h->handle == handle)
            return h;
    }
    return NULL;
}

/* Find a handle and lift out its socket under the lock, so the (possibly
 * sleeping) host socket op below runs with the lock dropped. Returns the
 * handle record (for its identity) and *sock; NULL means "no such handle". */
static struct vms_l2_handle *l2h_lookup(struct vms_proc *proc, uint32_t handle,
                                        exec_socket_t *sock)
{
    struct vms_l2_handle *h;

    spin_lock(&proc->l2_lock);
    h = l2h_find_locked(proc, handle);
    *sock = h ? h->sock : NULL;
    spin_unlock(&proc->l2_lock);
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
 */
long vms_ioctl_l2_open(struct vms_proc *proc, unsigned long arg)
{
    struct vms_l2_open_args args;
    struct vms_l2_handle *h;
    exec_socket_t sock;
    uint32_t ifindex = 0;
    int rc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    if (!l2_priv_check(proc->cur_privs, &args.status))
        goto out;

    /* args.ifname is a fixed 16-byte field (IFNAMSIZ); force-terminate so a
     * full, non-NUL-terminated 16 bytes from userspace cannot run the backend
     * past the buffer. */
    args.ifname[sizeof(args.ifname) - 1] = '\0';

    rc = exec_l2_open(args.ifname, args.ethertype, &ifindex, &sock);
    if (rc) {
        args.status = (rc == -ENODEV) ? SS__NOSUCHDEV : SS__ABORT;
        goto out;
    }

    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h) {
        exec_socket_release(sock);
        return -ENOMEM;
    }
    h->handle = (uint32_t)atomic_inc_return(&vms_l2_next_handle);
    h->ifindex = ifindex;
    h->sock = sock;

    spin_lock(&proc->l2_lock);
    list_add_tail(&h->list, &proc->l2_channels);
    spin_unlock(&proc->l2_lock);

    args.handle = h->handle;
    args.ifindex = ifindex;
    memset(args.hwaddr, 0, sizeof(args.hwaddr));
    (void)exec_l2_hwaddr(args.ifname, args.hwaddr);   /* best-effort; zeroed on failure */
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * VMS_IOCTL_L2_SEND -- send one frame out `handle` to `dst_mac` on `ifindex`
 * (exec_l2_send). No connect step: every send names its destination.
 */
long vms_ioctl_l2_send(struct vms_proc *proc, unsigned long arg)
{
    struct vms_l2_send_args *a;
    exec_socket_t sock;
    long ret = 0;
    long n;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    if (copy_from_user(a, (const void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (a->len > VMS_L2_MAXLEN) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    if (!l2h_lookup(proc, a->handle, &sock)) {
        a->status = SS__IVCHAN;   /* no such L2 handle */
        goto out_copy;
    }

    n = exec_l2_send(sock, (int)a->ifindex, a->ethertype, a->dst_mac, a->data, a->len);
    if (n < 0) {
        a->len = 0;
        a->status = SS__ABORT;
    } else {
        a->len = (uint32_t)n;
        a->status = SS__NORMAL;
    }

out_copy:
    if (copy_to_user((void __user *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    kfree(a);
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

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    if (copy_from_user(a, (const void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (!l2h_lookup(proc, a->handle, &sock)) {
        a->status = SS__IVCHAN;   /* no such L2 handle */
        goto out_copy;
    }

    rc = exec_l2_recv(sock, a->data, VMS_L2_MAXLEN, a->timeout_ms, &n);
    if (rc) {
        a->len = 0;
        a->status = SS__ABORT;
    } else {
        a->len = (uint32_t)n;
        a->status = SS__NORMAL;
    }

out_copy:
    if (copy_to_user((void __user *)arg, a, sizeof(*a)))
        ret = -EFAULT;
out_free:
    kfree(a);
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
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->l2_lock);
    h = l2h_find_locked(proc, args.handle);
    if (h)
        list_del(&h->list);
    spin_unlock(&proc->l2_lock);

    if (!h) {
        args.status = SS__IVCHAN;
        goto out;
    }

    exec_socket_release(h->sock);
    kfree(h);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
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
    LIST_HEAD(doomed);

    spin_lock(&proc->l2_lock);
    list_for_each_entry_safe(h, tmp, &proc->l2_channels, list)
        list_move(&h->list, &doomed);
    spin_unlock(&proc->l2_lock);

    list_for_each_entry_safe(h, tmp, &doomed, list) {
        list_del(&h->list);
        exec_socket_release(h->sock);
        kfree(h);
    }
}
