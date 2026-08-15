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
#include <linux/kref.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/anon_inodes.h>
#include <linux/tcp.h>          /* tcp_sk() -- read TCP_NODELAY back off the socket */
#include <net/sock.h>
#include <net/tcp.h>            /* TCP_NAGLE_OFF */
#include <net/inet_sock.h>      /* inet_sk() -- read IP_TOS back off the socket */

#include "vms_internal.h"   /* struct vms_proc / SS__* / VMS_DEVNAM_SIZE and,
                             * via vms_ioctl.h, the struct vms_bg_* args */

/* One process's channel to a BG unit. The host socket hangs here (NULL until
 * IO$_SETMODE creates it) -- the executive-resident object this facility is
 * about, keyed to the channel, exactly as a mailbox channel points at the
 * executive-resident mailbox. */
/* A refcounted holder for the host socket. The channel holds one reference; a
 * readiness poll fd (VMS_IOCTL_BG_POLLFD) holds another, so the socket outlives
 * a poll fd that is still open when the channel is $DASSGN'd. The last reference
 * to drop releases the host socket. */
struct vms_bg_socket {
    struct socket *sock;        /* host-kernel socket */
    struct kref    kref;
};

static void vms_bg_socket_free(struct kref *kref)
{
    struct vms_bg_socket *bs = container_of(kref, struct vms_bg_socket, kref);
    if (bs->sock)
        sock_release(bs->sock);
    kfree(bs);
}

struct vms_bg_chan {
    struct list_head list;      /* in proc->bg_channels */
    uint32_t chan;              /* this process's channel number */
    uint32_t unit;              /* BGn: unit number */
    struct vms_bg_socket *bs;   /* refcounted host socket, NULL until SETMODE */
};

/* ---- readiness-only poll fd (VMS_IOCTL_BG_POLLFD) --------------------------
 * A real Linux fd whose .poll delegates to the executive socket's own poll, and
 * which has NO read/write ops -- so a userspace event loop can wait on the BGn:
 * connection's readability/writability while data still moves only through
 * IO$_READVBLK / IO$_WRITEVBLK. Its private_data is a held vms_bg_socket. */
static __poll_t vms_bg_pollfd_poll(struct file *file, struct poll_table_struct *wait)
{
    struct vms_bg_socket *bs = file->private_data;
    struct socket *sock = bs ? bs->sock : NULL;

    if (!sock || !sock->ops || !sock->ops->poll)
        return EPOLLERR;
    /* Delegate to the host socket's poll: this registers the socket's wait
     * queue with `wait`, so poll()/select() blocks and wakes on real socket
     * readiness, and returns the socket's current mask (EPOLLIN/EPOLLOUT/...). */
    return sock->ops->poll(file, sock, wait);  /* NEGCTL bgsock-poll-always-ready */
}

static int vms_bg_pollfd_release(struct inode *inode, struct file *file)
{
    struct vms_bg_socket *bs = file->private_data;
    (void)inode;
    if (bs)
        kref_put(&bs->kref, vms_bg_socket_free);
    return 0;
}

static const struct file_operations vms_bg_pollfd_fops = {
    .owner   = THIS_MODULE,
    .poll    = vms_bg_pollfd_poll,
    .release = vms_bg_pollfd_release,
    .llseek  = noop_llseek,
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
    *sock = (ch && ch->bs) ? ch->bs->sock : NULL;
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
    ch->bs = NULL;

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

    {
        struct vms_bg_socket *bs = kmalloc(sizeof(*bs), GFP_KERNEL);
        if (!bs) {
            sock_release(sock);
            args.status = SS__ABORT;
            goto out;
        }
        bs->sock = sock;
        kref_init(&bs->kref);       /* the channel's reference */

        spin_lock(&proc->chan_lock);
        if (!ch->bs) {
            ch->bs = bs;
            bs = NULL;
        }
        spin_unlock(&proc->chan_lock);

        if (bs) {                   /* someone raced us in; drop the spare */
            sock_release(bs->sock);
            kfree(bs);
        }
    }
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

    if (ch->bs)
        kref_put(&ch->bs->kref, vms_bg_socket_free);
    kfree(ch);
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * VMS_IOCTL_BG_POLLFD -- hand back a real Linux readiness-only pollable fd for
 * the channel's socket (see the fops above and vms_bg.h). The fd's .poll
 * delegates to the executive socket's own poll, so a userspace event loop can
 * wait for the connection to become readable/writable while data still moves
 * only through IO$_READVBLK / IO$_WRITEVBLK. Item vms-22a (OpenSSH's poll()).
 */
long vms_ioctl_bg_pollfd(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_pollfd_args args;
    struct vms_bg_chan *ch;
    struct vms_bg_socket *bs = NULL;
    struct file *file;
    int fd;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.fd = -1;

    /* Take a reference to the channel's socket holder under the lock so it
     * cannot be released out from under the poll fd. */
    spin_lock(&proc->chan_lock);
    ch = bgchan_find_locked(proc, args.chan);
    if (ch && ch->bs) {
        bs = ch->bs;
        kref_get(&bs->kref);
    }
    spin_unlock(&proc->chan_lock);

    if (!ch) {
        args.status = SS__IVCHAN;
        goto out;
    }
    if (!bs) {
        args.status = SS__IVCHAN;   /* no socket: IO$_SETMODE was never issued */
        goto out;
    }

    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) {
        kref_put(&bs->kref, vms_bg_socket_free);
        args.status = SS__ABORT;
        goto out;
    }
    file = anon_inode_getfile("[bgpoll]", &vms_bg_pollfd_fops, bs,
                              O_RDONLY | O_CLOEXEC);
    if (IS_ERR(file)) {
        put_unused_fd(fd);
        kref_put(&bs->kref, vms_bg_socket_free);
        args.status = SS__ABORT;
        goto out;
    }
    fd_install(fd, file);           /* the file now owns the bs reference */
    args.fd = fd;
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_SENSEMODE (getsockname / getpeername) -- report the channel socket's
 * local or peer address, read straight from the host kernel socket. This is the
 * de-veneer crux (vms-4bf): the answer comes from the REAL kernel socket via
 * kernel_getsockname / kernel_getpeername, so an unmodified OpenSSH getpeername()
 * gets the true remote IP:port (known_hosts records the right host), not the
 * AF_UNIX peer a socketpair bridge would return.
 */
long vms_ioctl_bg_getname(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_name_args args;
    struct vms_bg_chan *ch;
    struct socket *sock;
    struct sockaddr_storage ss;
    struct sockaddr_in *sin;
    int rc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch || !sock) {
        args.status = SS__IVCHAN;   /* no socket: IO$_SETMODE was never issued */
        goto out;
    }

    memset(&ss, 0, sizeof(ss));
    if (args.which)
        rc = kernel_getpeername(sock, (struct sockaddr *)&ss);
    else
        rc = kernel_getsockname(sock, (struct sockaddr *)&ss);
    if (rc < 0) {
        args.status = SS__ABORT;
        goto out;
    }
    if (ss.ss_family != AF_INET) {
        args.status = SS__ABORT;    /* IPv6 not carried by this 8-byte tuple yet */
        goto out;
    }

    sin = (struct sockaddr_in *)&ss;
    args.sin_family = sin->sin_family;
    args.sin_port   = sin->sin_port;            /* network byte order, straight through */
    args.sin_addr   = sin->sin_addr.s_addr;     /* NEGCTL bgsock-getname-addr-zeroed */
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}

/*
 * IO$_SETMODE / IO$_SENSEMODE socket-option subfunction (setsockopt /
 * getsockopt) on the channel's REAL host kernel socket.
 *
 * SET routes the integer value through the socket's own ->setsockopt with a
 * KERNEL_SOCKPTR, so the option genuinely takes effect on the kernel socket
 * (this is the anti-veneer point: a socketpair would swallow TCP_NODELAY as
 * ENOPROTOOPT; here it is applied for real). GET reads the option back out of
 * the live socket state -- ->getsockopt takes user pointers so it is unusable
 * from kernel context; instead we read the small integer-option whitelist that
 * OpenSSH sets directly off the socket, which is the true current value.
 * Anything outside the whitelist returns SS$_BADPARAM (honest "unsupported",
 * mapped to ENOPROTOOPT by the veneer), never a fake success.
 */
long vms_ioctl_bg_sockopt(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_sockopt_args args;
    struct vms_bg_chan *ch;
    struct socket *sock;
    struct sock *sk;
    int rc;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;

    ch = bgchan_lookup(proc, args.chan, &sock);
    if (!ch || !sock) {
        args.status = SS__IVCHAN;
        goto out;
    }
    sk = sock->sk;
    if (!sk) {
        args.status = SS__ABORT;
        goto out;
    }

    if (args.op == 0) {
        /* setsockopt: apply the integer value to the real kernel socket. The
         * SOL_SOCKET special-casing that __sys_setsockopt does in the syscall
         * path is NOT in ->setsockopt (which reaches only the protocol/IP
         * handlers), so SOL_SOCKET options must go through sock_setsockopt
         * directly; every other level rides ->setsockopt. */
        int val = args.optval;
        if (args.level == SOL_SOCKET) {
            rc = sock_setsockopt(sock, args.level, args.optname,
                                 KERNEL_SOCKPTR(&val), sizeof(val));
        } else if (sock->ops && sock->ops->setsockopt) {
            rc = sock->ops->setsockopt(sock, args.level, args.optname,
                                       KERNEL_SOCKPTR(&val), sizeof(val));
        } else {
            args.status = SS__BADPARAM;
            goto out;
        }
        args.status = rc ? SS__BADPARAM : SS__NORMAL;
        goto out;
    }

    /* getsockopt: read the option back off the live socket. The whitelist
     * covers exactly the integer options OpenSSH probes (misc.c/sshconnect.c);
     * each value is the socket's genuine current state. */
    if (args.level == SOL_SOCKET && args.optname == SO_KEEPALIVE) {
        args.optval = sock_flag(sk, SOCK_KEEPOPEN) ? 1 : 0;
        args.status = SS__NORMAL;
    } else if (args.level == SOL_SOCKET && args.optname == SO_REUSEADDR) {
        args.optval = sk->sk_reuse ? 1 : 0;
        args.status = SS__NORMAL;
    } else if (args.level == SOL_SOCKET && args.optname == SO_ERROR) {
        args.optval = -sock_error(sk);          /* pending error, cleared (as getsockopt does) */
        args.status = SS__NORMAL;
    } else if (args.level == IPPROTO_TCP && args.optname == TCP_NODELAY) {
        args.optval = (tcp_sk(sk)->nonagle & TCP_NAGLE_OFF) ? 1 : 0;
        args.status = SS__NORMAL;
    } else if (args.level == IPPROTO_IP && args.optname == IP_TOS) {
        args.optval = inet_sk(sk)->tos;
        args.status = SS__NORMAL;
    } else {
        args.status = SS__BADPARAM;             /* unsupported option: honest, not faked */
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
        if (ch->bs)
            kref_put(&ch->bs->kref, vms_bg_socket_free);
        kfree(ch);
    }
}
