// SPDX-License-Identifier: GPL-2.0
/*
 * vms_bg.c - Executive-resident INET pseudo-device BGn: (vms-527)
 *
 * See vms_bg.h for the full rationale (what this is, where the socket lives,
 * who owns what). In one line: BGn: is a KERNEL-MODE DEVICE DRIVER in the
 * executive, the direct analogue of the mailbox (vms_mbx.c) with a host-kernel
 * socket where the mailbox has a message queue -- so a VMS program reaches the
 * network the ordinary VMS way ($ASSIGN a channel, $QIO on it), and the socket
 * lives in the executive, never in the process.
 *
 * WHY THIS FILE IS IN src/kernel-core/ (vms-9951). It once lived in the Linux
 * rind (src/kernel/) because it drove the Linux in-kernel socket API directly
 * and "there is no exec_socket_* seam". That seam now EXISTS (exec_kbackend.h
 * §12): the host TCP client socket is an opaque, reference-counted exec_socket_t
 * whose per-substrate backend (Linux sock_create_kern/kernel_*; NetBSD socreate/
 * so*) is the ONLY place a host socket type appears. So this facility joins every
 * other executive facility in the substrate-agnostic core, calling exec_socket_*
 * and naming no <linux/...> socket header -- exactly as vmsfs_acp.c drives the
 * disk through exec_blockdev_*. "The IP stack itself is the host kernel's"
 * (docs/design-tcpip-services-ovmx.md §2) is unchanged; it is now reached through
 * the seam. The ONE piece that cannot cross the seam -- the Linux readiness poll
 * fd (VMS_IOCTL_BG_POLLFD: anon_inode + ->poll, no NetBSD analogue; NetBSD is
 * kqueue) -- stays a Linux rind in src/kernel/vms_bg_pollfd.c, reaching this
 * facility only through vms_bg_ref_socket() (vms_bg_core.h) and the seam's
 * Linux-only exec_socket_raw(). A runnable NetBSD BGn: is vms-024.
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
 * its socket handle used after the lock is dropped (a host socket op can sleep
 * and must not run under a spinlock), which is safe precisely because a process
 * does not $QIO and $DASSGN the same channel from two threads at once in this
 * increment. Cross-thread channel sharing is a later phase.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/atomic.h>
#include <linux/errno.h>

#include "vms_internal.h"   /* struct vms_proc / SS__* / VMS_DEVNAM_SIZE and,
                             * via vms_ioctl.h, the struct vms_bg_* args */
#include "exec_kbackend.h"  /* the exec_socket_* host-socket seam (§12) */
#include "vms_bg_core.h"    /* vms_bg_ref_socket -- the core<->pollfd-rind boundary */

/* One process's channel to a BG unit. The host socket hangs here as an
 * exec_socket_t (NULL until IO$_SETMODE creates it) -- the executive-resident
 * object this facility is about, keyed to the channel, exactly as a mailbox
 * channel points at the executive-resident mailbox. exec_socket_t is itself the
 * reference-counted holder (the channel owns one reference; a readiness poll fd
 * takes a second via exec_socket_get, so the socket outlives a poll fd still
 * open when the channel is $DASSGN'd -- exec_socket_release drops a reference,
 * the last one releasing the host socket). */
struct vms_bg_chan {
    struct list_head list;      /* in proc->bg_channels */
    uint32_t chan;              /* this process's channel number */
    uint32_t unit;              /* BGn: unit number */
    exec_socket_t sock;         /* refcounted host socket, NULL until SETMODE */
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

/* Find a channel and lift out its socket handle under the lock, so the (possibly
 * sleeping) host socket op below runs with the lock dropped. Returns the channel
 * (for its identity) and *sock; NULL channel means SS$_IVCHAN, non-NULL channel
 * with *sock==NULL means "no socket yet" (IO$_SETMODE was never issued). */
static struct vms_bg_chan *bgchan_lookup(struct vms_proc *proc, uint32_t chan,
                                         exec_socket_t *sock)
{
    struct vms_bg_chan *ch;

    spin_lock(&proc->chan_lock);
    ch = bgchan_find_locked(proc, chan);
    *sock = (ch && ch->sock) ? ch->sock : NULL;
    spin_unlock(&proc->chan_lock);
    return ch;
}

/* Look up proc's BG channel `chan`, take a reference on its socket holder, and
 * return it (NULL if no such channel or no socket yet). The caller owns the
 * returned reference and must exec_socket_release() it. The core<->rind boundary
 * (vms_bg_core.h): the Linux readiness-poll rind (src/kernel/vms_bg_pollfd.c)
 * needs a held socket without reaching into this file's private channel list. */
exec_socket_t vms_bg_ref_socket(struct vms_proc *proc, uint32_t chan)
{
    struct vms_bg_chan *ch;
    exec_socket_t s = NULL;

    spin_lock(&proc->chan_lock);
    ch = bgchan_find_locked(proc, chan);
    if (ch && ch->sock) {
        s = ch->sock;
        exec_socket_get(s);
    }
    spin_unlock(&proc->chan_lock);
    return s;
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
    exec_socket_t sock, have, bs;
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

    rc = exec_socket_create(&sock);
    if (rc) {
        args.status = SS__ABORT;
        goto out;
    }

    bs = sock;
    spin_lock(&proc->chan_lock);
    if (!ch->sock) {
        ch->sock = bs;
        bs = NULL;
    }
    spin_unlock(&proc->chan_lock);

    if (bs)                         /* someone raced us in; drop the spare */
        exec_socket_release(bs);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_ACCESS -- connect the channel's socket to a peer (exec_socket_connect).
 * The address arrives as the first eight bytes of a sockaddr_in, port and v4
 * addr already in network byte order; the backend builds the host sockaddr.
 */
long vms_ioctl_bg_connect(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_connect_args args;
    struct vms_bg_chan *ch;
    exec_socket_t sock;
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

    rc = exec_socket_connect(sock, args.sin_family, args.sin_port, args.sin_addr);
    args.status = rc ? SS__ABORT : SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_WRITEVBLK -- send one buffer over the connection (exec_socket_send).
 */
long vms_ioctl_bg_send(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_io_args *a;
    struct vms_bg_chan *ch;
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

    if (a->len > VMS_BG_IOCTL_MAXLEN) {
        a->status = SS__BADPARAM;
        goto out_copy;
    }

    ch = bgchan_lookup(proc, a->chan, &sock);
    if (!ch || !sock) {
        a->status = SS__IVCHAN;
        goto out_copy;
    }

    n = exec_socket_send(sock, a->data, a->len);
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
 * IO$_READVBLK -- receive one buffer from the connection (exec_socket_recv).
 * Byte-stream: `len` in is the buffer size, `len` out is the actual count the
 * host kernel returned (0 = orderly peer close, EOF).
 */
long vms_ioctl_bg_recv(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_io_args *a;
    struct vms_bg_chan *ch;
    exec_socket_t sock;
    long ret = 0;
    uint32_t bufsz;
    long n;

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

    n = exec_socket_recv(sock, a->data, bufsz);
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
 * IO$_DEACCESS -- shut the connection down both ways (exec_socket_shutdown).
 * The socket itself is released at $DASSGN; a shutdown-then-reuse is not part
 * of this increment, so DEACCESS is the graceful half and DASSGN the release.
 */
long vms_ioctl_bg_deaccess(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_chanonly_args args;
    struct vms_bg_chan *ch;
    exec_socket_t sock;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch) {
        args.status = SS__IVCHAN;
        goto out;
    }
    if (sock)
        exec_socket_shutdown(sock);
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
 * generic vms_ioctl_dassgn, so it gets its own dassgn ioctl.
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
        exec_socket_release(ch->sock);
    kfree(ch);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_SENSEMODE (getsockname / getpeername) -- report the channel socket's
 * local or peer address, read straight from the host kernel socket via
 * exec_socket_getname. This is the de-veneer crux (vms-4bf): the answer comes
 * from the REAL kernel socket, so an unmodified OpenSSH getpeername() gets the
 * true remote IP:port (known_hosts records the right host), not the AF_UNIX peer
 * a socketpair bridge would return.
 */
long vms_ioctl_bg_getname(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_name_args args;
    struct vms_bg_chan *ch;
    exec_socket_t sock;
    int rc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch || !sock) {
        args.status = SS__IVCHAN;   /* no socket: IO$_SETMODE was never issued */
        goto out;
    }

    /* The backend fills the same 8-byte AF_INET tuple IO$_ACCESS accepts, in
     * network byte order, and reports the AF_INET check itself (a non-AF_INET
     * peer -> negative rc -> SS$_ABORT; IPv6 not carried by this tuple yet). */
    rc = exec_socket_getname(sock, args.which,
                             &args.sin_family, &args.sin_port, &args.sin_addr);
    args.status = rc ? SS__ABORT : SS__NORMAL;   /* NEGCTL bgsock-getname-addr-zeroed */

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_SETMODE / IO$_SENSEMODE socket-option subfunction (setsockopt /
 * getsockopt) on the channel's REAL host kernel socket, through the seam.
 *
 * SET routes the integer value through exec_socket_setopt_int, which applies it
 * to the real kernel socket (the anti-veneer point: a socketpair would swallow
 * TCP_NODELAY as ENOPROTOOPT; here it is applied for real). GET reads the option
 * back out of the live socket state via exec_socket_getopt_int, which honors the
 * small integer-option whitelist OpenSSH probes and returns the true current
 * value; anything outside the whitelist is an HONEST failure -> SS$_BADPARAM
 * (the veneer maps that to ENOPROTOOPT), never a fake success.
 */
long vms_ioctl_bg_sockopt(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_sockopt_args args;
    struct vms_bg_chan *ch;
    exec_socket_t sock;
    int rc, val;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch || !sock) {
        args.status = SS__IVCHAN;
        goto out;
    }

    if (args.op == 0) {
        rc = exec_socket_setopt_int(sock, args.level, args.optname, args.optval);
        args.status = rc ? SS__BADPARAM : SS__NORMAL;
        goto out;
    }

    rc = exec_socket_getopt_int(sock, args.level, args.optname, &val);
    if (rc) {
        args.status = SS__BADPARAM;             /* unsupported option: honest, not faked */
    } else {
        args.optval = val;
        args.status = SS__NORMAL;
    }

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
            exec_socket_release(ch->sock);
        kfree(ch);
    }
}
