// SPDX-License-Identifier: GPL-2.0
/*
 * vms_bg.c - Executive-resident INET pseudo-device BGn: (vms-527)
 *
 * See vms_bg.h for the full rationale (what this is, where the socket lives,
 * who owns what). In one line: BGn: is a KERNEL-MODE DEVICE DRIVER in the
 * executive, the direct analogue of the mailbox (kernel-core/vms_mbx.c) with a
 * host-kernel socket where the mailbox has a message queue -- so a VMS program
 * reaches the network the ordinary VMS way ($ASSIGN a channel, $QIO on it),
 * and the socket lives in vms.ko, never in the process.
 *
 * WHY THIS FILE IS IN src/kernel/ AND NOT src/kernel-core/. Every other
 * executive facility now lives in the substrate-agnostic core (see the
 * Makefile note) and touches the host only through the exec_* kernel-backend
 * shim. BGn: is different in kind: it drives the HOST KERNEL'S in-kernel socket
 * API directly (sock_create_kern / kernel_connect / kernel_sendmsg /
 * kernel_recvmsg / kernel_sock_shutdown / sock_release), which is exactly the
 * point of the design -- "the IP stack itself is Linux's" (docs/design-tcpip-
 * services-ovmx.md §2), OVMX never reimplements a transport. There is no
 * exec_socket_* seam, and inventing one for a single facility would be
 * gold-plating this increment; a future OVMX/NetBSD SYSKRNL will supply its own
 * BGn: driver against its own kernel sockets. So this stays in the Linux rind
 * beside vms_module.c / vms_p0.c / vms_p1.c and names <linux/...> directly.
 *
 * LIFECYCLE (mirrors the mailbox). A BG channel is created ON DEMAND by
 * $ASSIGN (VMS_IOCTL_BG_CREATE) and lives in the owning process's bg_channels
 * list (struct vms_proc, vms_internal.h), drawing its channel NUMBER from the
 * same proc->next_chan counter the device and mailbox channels use. $DASSGN
 * (VMS_IOCTL_BG_DASSGN) releases the channel and, if it still holds one, the
 * host socket; a process that dies without an explicit $DASSGN has every BG
 * channel reclaimed by vms_bg_release_all() at teardown (called from
 * vms_module.c, the same place vms_proc_release_channels / vms_mbx_release_all
 * run), so no socket leaks for the life of the module.
 *
 * FIRST-INCREMENT SCOPE (vms-527). This is the CLIENT path -- socket / connect
 * / send / recv / shutdown -- reached only through the owning process's own
 * channel, exercised synchronously by one thread (the shape $QIOW gives). It
 * deliberately does NOT add cross-process socket visibility (TCPIP SHOW
 * DEVICE_SOCKET, design §5 Phase 3), listen/accept handoff, or per-channel
 * concurrency hardening: a BG channel is looked up under proc->chan_lock and
 * its socket pointer used after the lock is dropped (a host socket op can
 * sleep and must not run under a spinlock), which is safe precisely because a
 * process does not $QIO and $DASSGN the same channel from two threads at once
 * in this increment. Cross-thread channel sharing is a later phase.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <net/sock.h>

#include "vms_internal.h"   /* struct vms_proc / SS__* / VMS_DEVNAM_SIZE and,
                             * via vms_ioctl.h, the struct vms_bg_* args */

/* One process's channel to a BG unit. The host socket hangs here (NULL until
 * IO$_SETMODE creates it) -- the executive-resident object this facility is
 * about, keyed to the channel, exactly as a mailbox channel points at the
 * executive-resident mailbox. */
struct vms_bg_chan {
    struct list_head list;      /* in proc->bg_channels */
    uint32_t chan;              /* this process's channel number */
    uint32_t unit;              /* BGn: unit number */
    struct socket *sock;        /* host-kernel socket, NULL until SETMODE */
};

/* BGn: unit numbers are node-wide and monotonic, like the mailbox's
 * vms_mbx_next_unit. Starting at 1 (BG1: the first unit) is an OVMX numbering
 * choice (vms_bg.h): a VMS BG unit number is opaque to the program, which binds
 * the channel, not the number. */
static atomic_t vms_bg_next_unit = ATOMIC_INIT(0);

/* Caller holds proc->chan_lock. */
static struct vms_bg_chan *bgchan_find_locked(struct vms_proc *proc, uint32_t chan)
{
    struct vms_bg_chan *ch;

    list_for_each_entry(ch, &proc->bg_channels, list) {
        if (ch->chan == chan)
            return ch;
    }
    return NULL;
}

/* Find a channel and lift out its socket pointer under the lock, so the (possibly
 * sleeping) host socket op below runs with the lock dropped. Returns the channel
 * (for its identity) and *sock; NULL channel means SS$_IVCHAN, non-NULL channel
 * with *sock==NULL means "no socket yet" (IO$_SETMODE was never issued). */
static struct vms_bg_chan *bgchan_lookup(struct vms_proc *proc, uint32_t chan,
                                         struct socket **sock)
{
    struct vms_bg_chan *ch;

    spin_lock(&proc->chan_lock);
    ch = bgchan_find_locked(proc, chan);
    *sock = ch ? ch->sock : NULL;
    spin_unlock(&proc->chan_lock);
    return ch;
}

/* ================================================================
 * ioctl handlers
 * ================================================================ */

/*
 * $ASSIGN to TCPIP$DEVICE: -- allocate a fresh BGn: unit and a channel to it
 * (the direct analogue of $CREMBX / VMS_IOCTL_MBX_CREATE). No socket yet.
 */
long vms_ioctl_bg_create(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_create_args args;
    struct vms_bg_chan *ch;

    memset(&args, 0, sizeof(args));

    ch = kzalloc(sizeof(*ch), GFP_KERNEL);
    if (!ch)
        return -ENOMEM;

    ch->unit = (uint32_t)atomic_inc_return(&vms_bg_next_unit);
    ch->sock = NULL;

    spin_lock(&proc->chan_lock);
    ch->chan = ++proc->next_chan;
    list_add_tail(&ch->list, &proc->bg_channels);
    spin_unlock(&proc->chan_lock);

    args.chan = ch->chan;
    args.unit = ch->unit;
    snprintf(args.devnam, sizeof(args.devnam), "BG%u:", ch->unit);
    args.status = SS__NORMAL;

    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_SETMODE -- create the host socket on the channel (AF_INET/SOCK_STREAM).
 * Idempotent: a second SETMODE on a channel that already has a socket is a
 * success, not a leak (only the first create wins; a raced second socket is
 * released).
 */
long vms_ioctl_bg_setmode(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_chanonly_args args;
    struct vms_bg_chan *ch;
    struct socket *sock, *have;
    int rc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &have);
    if (!ch) {
        args.status = SS__IVCHAN;
        goto out;
    }
    if (have) {
        args.status = SS__NORMAL;   /* socket already created */
        goto out;
    }

    rc = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &sock);
    if (rc) {
        args.status = SS__ABORT;
        goto out;
    }

    spin_lock(&proc->chan_lock);
    if (!ch->sock) {
        ch->sock = sock;
        sock = NULL;
    }
    spin_unlock(&proc->chan_lock);

    if (sock)               /* someone raced us in; release the spare */
        sock_release(sock);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_ACCESS -- connect the channel's socket to a peer (kernel_connect). The
 * address arrives as the first eight bytes of a sockaddr_in, port and v4 addr
 * already in network byte order.
 */
long vms_ioctl_bg_connect(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_connect_args args;
    struct vms_bg_chan *ch;
    struct socket *sock;
    struct sockaddr_in sa;
    int rc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch) {
        args.status = SS__IVCHAN;
        goto out;
    }
    if (!sock) {
        args.status = SS__IVCHAN;   /* no socket: IO$_SETMODE was never issued */
        goto out;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = args.sin_family;
    sa.sin_port = args.sin_port;    /* network byte order, straight through */
    sa.sin_addr.s_addr = args.sin_addr;

    rc = kernel_connect(sock, (struct sockaddr *)&sa, sizeof(sa), 0);
    args.status = rc ? SS__ABORT : SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_WRITEVBLK -- send one buffer over the connection (kernel_sendmsg).
 */
long vms_ioctl_bg_send(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_io_args *a;
    struct vms_bg_chan *ch;
    struct socket *sock;
    struct msghdr msg;
    struct kvec vec;
    long ret = 0;
    int n;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    if (copy_from_user(a, (const void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    if (a->len > VMS_BG_IOCTL_MAXLEN) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    ch = bgchan_lookup(proc, a->chan, &sock);
    if (!ch || !sock) {
        a->status = SS__IVCHAN;
        goto out_copy;
    }

    memset(&msg, 0, sizeof(msg));
    vec.iov_base = a->data;
    vec.iov_len = a->len;
    n = kernel_sendmsg(sock, &msg, &vec, 1, a->len);
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
 * IO$_READVBLK -- receive one buffer from the connection (kernel_recvmsg).
 * Byte-stream: `len` in is the buffer size, `len` out is the actual count the
 * host kernel returned (0 = orderly peer close, EOF).
 */
long vms_ioctl_bg_recv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_io_args *a;
    struct vms_bg_chan *ch;
    struct socket *sock;
    struct msghdr msg;
    struct kvec vec;
    long ret = 0;
    uint32_t bufsz;
    int n;

    a = kzalloc(sizeof(*a), GFP_KERNEL);
    if (!a)
        return -ENOMEM;
    if (copy_from_user(a, (const void __user *)arg, sizeof(*a))) {
        ret = -EFAULT;
        goto out_free;
    }

    bufsz = (a->len > VMS_BG_IOCTL_MAXLEN) ? VMS_BG_IOCTL_MAXLEN : a->len;

    ch = bgchan_lookup(proc, a->chan, &sock);
    if (!ch || !sock) {
        a->status = SS__IVCHAN;
        goto out_copy;
    }

    memset(&msg, 0, sizeof(msg));
    vec.iov_base = a->data;
    vec.iov_len = bufsz;
    n = kernel_recvmsg(sock, &msg, &vec, 1, bufsz, 0);
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
 * IO$_DEACCESS -- shut the connection down both ways (kernel_sock_shutdown).
 * The socket itself is released at $DASSGN; a shutdown-then-reuse is not part
 * of this increment, so DEACCESS is the graceful half and DASSGN the release.
 */
long vms_ioctl_bg_deaccess(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_chanonly_args args;
    struct vms_bg_chan *ch;
    struct socket *sock;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch) {
        args.status = SS__IVCHAN;
        goto out;
    }
    if (sock)
        kernel_sock_shutdown(sock, SHUT_RDWR);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * $DASSGN -- release the channel and its host socket. Reached from
 * sys$dassgn's BG branch (src/libvms/syssvc/sys_assign.c) via
 * VMS_IOCTL_BG_DASSGN; unlike the mailbox, BG does not fall through the
 * generic vms_ioctl_dassgn (which lives in the substrate-agnostic kernel-core
 * and must not name the host socket API), so it gets its own dassgn ioctl.
 */
long vms_ioctl_bg_dassgn(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_chanonly_args args;
    struct vms_bg_chan *ch;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    spin_lock(&proc->chan_lock);
    ch = bgchan_find_locked(proc, args.chan);
    if (ch)
        list_del(&ch->list);
    spin_unlock(&proc->chan_lock);

    if (!ch) {
        args.status = SS__IVCHAN;
        goto out;
    }

    if (ch->sock)
        sock_release(ch->sock);
    kfree(ch);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * Give back every BG channel a dying process holds, releasing each host socket
 * -- the BG counterpart of vms_mbx_release_all(), called from vms_module.c's
 * process-teardown path (vms_proc_free_claimed) so a temporary BG connection
 * whose owner exits without $DASSGN is still reclaimed.
 */
void vms_bg_release_all(struct vms_proc *proc)
{
    struct vms_bg_chan *ch, *tmp;
    LIST_HEAD(doomed);

    spin_lock(&proc->chan_lock);
    list_for_each_entry_safe(ch, tmp, &proc->bg_channels, list)
        list_move(&ch->list, &doomed);
    spin_unlock(&proc->chan_lock);

    list_for_each_entry_safe(ch, tmp, &doomed, list) {
        list_del(&ch->list);
        if (ch->sock)
            sock_release(ch->sock);
        kfree(ch);
    }
}
