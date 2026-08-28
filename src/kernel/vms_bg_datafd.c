// SPDX-License-Identifier: GPL-2.0
/*
 * vms_bg_datafd.c - the Linux DATA-fd rind for BGn: (vms-0cd, RUNG-3b).
 *
 * VMS_IOCTL_BG_MATERIALIZE_FD hands userspace a REAL, DATA-carrying Linux fd for a
 * BG channel's executive-resident socket. Unlike the readiness-only poll fd
 * (vms_bg_pollfd.c), this fd's .read/.write move real bytes -- routed straight to
 * the channel's executive-resident socket (the SAME host in-kernel socket the
 * IO$_READVBLK/IO$_WRITEVBLK handlers drive via exec_socket_recv/send). So DATA
 * STILL TRANSITS THE EXECUTIVE: the fd is a vms.ko-backed anon_inode ("[bgconn]"),
 * NOT a host socket handed to userspace and NOT an AF_UNIX socketpair (INV-6).
 *
 * WHY IT EXISTS. A ported Unix daemon (sshd) hands an accepted connection to its
 * per-connection child by dup2()'ing the connection onto stdin/stdout and then
 * execv()'ing the child, which does ordinary read()/write() on fd 0/1. A BGn:
 * veneer HANDLE (>= OVMX_BGSOCK_BASE) is not a real fd, so dup2(handle, 0) fails
 * EBADF. This ioctl materializes the channel as a real fd that is (a) dup2-able and
 * (b) has NO O_CLOEXEC, so it survives execve into the child -- whose read()/write()
 * on the inherited fd then reach the executive socket through these fops. The
 * userspace veneer wraps dup2()/dup() to materialize on demand (ovmx_materialize_fd).
 *
 * WHY A LINUX RIND, NOT THE CORE. get_unused_fd_flags / anon_inode_getfile /
 * fd_install / struct file_operations / kernel_recvmsg is pure Linux fd machinery
 * with NO NetBSD analogue (same reasoning as vms_bg_pollfd.c). It reaches the
 * facility ONLY through the core boundary vms_bg_ref_socket() and the seam's
 * Linux-only exec_socket_raw(); NetBSD does not build this file (and does not build
 * the BGn: core at all), so on VAX the ioctl is simply unimplemented and returns an
 * honest error -- never a fabricated fd.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/anon_inodes.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/net.h>          /* struct socket, kernel_recvmsg/sendmsg, ->ops->poll */
#include <net/sock.h>

#include "vms_internal.h"       /* struct vms_proc / SS__* / vms_bg_datafd_args */
#include "exec_kbackend.h"      /* exec_socket_t / exec_socket_raw / exec_socket_release */
#include "vms_bg_core.h"        /* vms_bg_ref_socket -- the core boundary (basename incl,
                                 * -I../kernel-core standalone / -I$(src) in-tree). */

/* ---- data-carrying fd over the executive socket -----------------------------
 * private_data is a HELD exec_socket_t (vms_bg_ref_socket), so the executive socket
 * outlives a data fd still open after the channel is $DASSGN'd; ->release drops it.
 * .read/.write route to the SAME executive-resident struct socket the READVBLK/
 * WRITEVBLK $QIO handlers use (exec_socket_raw), honoring O_NONBLOCK via
 * MSG_DONTWAIT so an event-loop daemon that sets the fd non-blocking + poll()s it
 * behaves correctly. One kernel_recvmsg/sendmsg per call (stream semantics: a
 * short read/write is normal). */
static ssize_t vms_bg_datafd_read(struct file *file, char __user *ubuf,
                                  size_t count, loff_t *ppos)
{
    exec_socket_t bs = file->private_data;
    struct socket *sock = bs ? exec_socket_raw(bs) : NULL;
    struct msghdr msg;
    struct kvec vec;
    void *kbuf;
    size_t cap;
    long n;

    (void)ppos;
    if (!sock)
        return -EBADF;
    if (count == 0)
        return 0;
    cap = count > PAGE_SIZE ? PAGE_SIZE : count;
    kbuf = kmalloc(cap, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    memset(&msg, 0, sizeof(msg));
    vec.iov_base = kbuf;
    vec.iov_len = cap;
    /* Same executive socket, same recv path as IO$_READVBLK (exec_socket_recv). */
    n = kernel_recvmsg(sock, &msg, &vec, 1, cap,
                       (file->f_flags & O_NONBLOCK) ? MSG_DONTWAIT : 0);
    if (n > 0 && copy_to_user(ubuf, kbuf, (unsigned long)n))
        n = -EFAULT;

    kfree(kbuf);
    return n;                   /* bytes / 0 = orderly EOF / -errno (e.g. -EAGAIN) */
}

static ssize_t vms_bg_datafd_write(struct file *file, const char __user *ubuf,
                                   size_t count, loff_t *ppos)
{
    exec_socket_t bs = file->private_data;
    struct socket *sock = bs ? exec_socket_raw(bs) : NULL;
    struct msghdr msg;
    struct kvec vec;
    void *kbuf;
    size_t cap;
    long n;

    (void)ppos;
    if (!sock)
        return -EBADF;
    if (count == 0)
        return 0;
    cap = count > PAGE_SIZE ? PAGE_SIZE : count;
    kbuf = kmalloc(cap, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;
    if (copy_from_user(kbuf, ubuf, cap)) {
        kfree(kbuf);
        return -EFAULT;
    }

    memset(&msg, 0, sizeof(msg));
    if (file->f_flags & O_NONBLOCK)
        msg.msg_flags = MSG_DONTWAIT;
    vec.iov_base = kbuf;
    vec.iov_len = cap;
    /* Same executive socket, same send path as IO$_WRITEVBLK (exec_socket_send). */
    n = kernel_sendmsg(sock, &msg, &vec, 1, cap);

    kfree(kbuf);
    return n;                   /* bytes / -errno (e.g. -EAGAIN, -EPIPE) */
}

static __poll_t vms_bg_datafd_poll(struct file *file, struct poll_table_struct *wait)
{
    exec_socket_t bs = file->private_data;
    struct socket *sock = bs ? exec_socket_raw(bs) : NULL;

    if (!sock || !sock->ops || !sock->ops->poll)
        return EPOLLERR;
    return sock->ops->poll(file, sock, wait);
}

static int vms_bg_datafd_release(struct inode *inode, struct file *file)
{
    exec_socket_t bs = file->private_data;
    (void)inode;
    if (bs)
        exec_socket_release(bs);   /* drop the reference the fd held */
    return 0;
}

/*
 * Socket-name / socket-option surface on the materialized fd itself (vms-0cd). A
 * wrapped daemon's exec'd child holds only this real [bgconn] fd -- no BG channel --
 * yet still getpeername()/getsockname()/setsockopt()/getsockopt()s it. Answer from
 * the fd's HELD exec_socket_t (the SAME executive socket carrying the bytes), so the
 * peer is the TRUE accepted-connection peer, never synthesized. Anything else is
 * -ENOTTY, so an ordinary ioctl on this fd (and the wrap's probe of a non-[bgconn]
 * fd) falls through cleanly. The `chan` field of the reused arg structs is ignored.
 */
static long vms_bg_datafd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    exec_socket_t bs = file->private_data;
    int rc;

    if (!bs)
        return -EBADF;

    switch (cmd) {
    case VMS_IOCTL_BGCONN_GETNAME: {
        struct vms_bg_name_args a;

        if (copy_from_user(&a, (const void __user *)arg, sizeof(a)))
            return -EFAULT;
        rc = exec_socket_getname(bs, a.which, &a.sin_family, &a.sin_port, &a.sin_addr);
        a.status = rc ? SS__ABORT : SS__NORMAL;
        if (copy_to_user((void __user *)arg, &a, sizeof(a)))
            return -EFAULT;
        return 0;
    }
    case VMS_IOCTL_BGCONN_SOCKOPT: {
        struct vms_bg_sockopt_args a;
        int val;

        if (copy_from_user(&a, (const void __user *)arg, sizeof(a)))
            return -EFAULT;
        if (a.op == 0) {
            rc = exec_socket_setopt_int(bs, a.level, a.optname, a.optval);
            a.status = rc ? SS__BADPARAM : SS__NORMAL;
        } else {
            rc = exec_socket_getopt_int(bs, a.level, a.optname, &val);
            if (!rc)
                a.optval = val;
            a.status = rc ? SS__BADPARAM : SS__NORMAL;
        }
        if (copy_to_user((void __user *)arg, &a, sizeof(a)))
            return -EFAULT;
        return 0;
    }
    default:
        return -ENOTTY;                 /* not one of ours -> honest fall-through */
    }
}

static const struct file_operations vms_bg_datafd_fops = {
    .owner          = THIS_MODULE,
    .read           = vms_bg_datafd_read,
    .write          = vms_bg_datafd_write,
    .poll           = vms_bg_datafd_poll,
    .unlocked_ioctl = vms_bg_datafd_ioctl,
    .release        = vms_bg_datafd_release,
    .llseek         = noop_llseek,
};

/*
 * VMS_IOCTL_BG_MATERIALIZE_FD -- hand back a REAL data-carrying fd for the
 * channel's executive socket. The fd holds a reference on the socket (via
 * vms_bg_ref_socket), released by ->release, so the channel may be $DASSGN'd while
 * the fd is still open (and the fd may outlive the channel across an execve).
 *
 * Fail-honest (INV-6): a channel that does not exist, or has no socket yet, returns
 * SS$_IVCHAN and fd = -1 -- NEVER a fabricated fd. get_unused_fd_flags(0): NO
 * O_CLOEXEC, so the fd survives execve (that is the whole point).
 */
long vms_ioctl_bg_materialize_fd(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_datafd_args args;
    exec_socket_t bs;
    struct file *file;
    int fd;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.fd = -1;

    bs = vms_bg_ref_socket(proc, args.chan);
    {   /* OVMX-DIAG (temporary, vms-0cd): why does materialize IVCHAN in sshd's
         * re-exec'd child? Log the requested chan, this task's identity, and its
         * real_parent tgid, so the CI console shows whether inheritance populated
         * the proc. Remove before reap. */
        pid_t rpt = 0;
        rcu_read_lock();
        if (current->real_parent) rpt = task_tgid_nr(current->real_parent);
        rcu_read_unlock();
        pr_err("OVMX-DIAG materialize: chan=%u tgid=%d comm=%s real_parent_tgid=%d found=%d\n",
                args.chan, current->tgid, current->comm, rpt, bs ? 1 : 0);
    }
    if (!bs) {
        args.status = SS__IVCHAN;       /* no channel / no socket -> honest reject */
        goto out;
    }

    fd = get_unused_fd_flags(0);        /* deliberately NO O_CLOEXEC: survives execve */
    if (fd < 0) {
        exec_socket_release(bs);
        args.status = SS__ABORT;
        goto out;
    }
    file = anon_inode_getfile("[bgconn]", &vms_bg_datafd_fops, bs, O_RDWR);
    if (IS_ERR(file)) {
        put_unused_fd(fd);
        exec_socket_release(bs);
        args.status = SS__ABORT;
        goto out;
    }
    fd_install(fd, file);               /* the file now owns the bs reference */
    args.fd = fd;
    args.status = SS__NORMAL;

out:
    if (copy_to_user((void __user *)arg, &args, sizeof(args)))
        return -EFAULT;
    return 0;
}
