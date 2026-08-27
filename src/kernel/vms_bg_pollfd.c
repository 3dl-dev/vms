// SPDX-License-Identifier: GPL-2.0
/*
 * vms_bg_pollfd.c - the Linux readiness-poll rind for BGn: (vms-9951, vms-22a).
 *
 * VMS_IOCTL_BG_POLLFD hands userspace a REAL Linux pollable fd for a BG
 * channel's executive-resident socket, so a userspace event loop (poll()/
 * select(), e.g. OpenSSH's clientloop/serverloop) can wait for the connection to
 * become readable/writable while data still moves ONLY through IO$_READVBLK /
 * IO$_WRITEVBLK. The fd is READINESS-ONLY (no read/write ops); its .poll
 * delegates to the host socket's own ->poll, so the socket stays
 * executive-resident.
 *
 * WHY THIS IS A LINUX RIND, NOT IN THE CORE (vms-9951). anon_inode_getfile /
 * fd_install / __poll_t / ->ops->poll is pure Linux fd machinery with NO NetBSD
 * analogue (NetBSD readiness is kqueue -- a different rind, deferred to a runnable
 * NetBSD BGn:, vms-024). Everything else about BGn: moved to the substrate-
 * agnostic core (../kernel-core/vms_bg.c) behind the exec_socket_* seam; this fd
 * is the one piece that cannot cross it. It reaches the facility ONLY through the
 * core boundary vms_bg_ref_socket() (vms_bg_core.h) for a referenced socket, and
 * the seam's Linux-only exec_socket_raw() for the raw struct socket to poll.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/anon_inodes.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/uaccess.h>
#include <linux/net.h>          /* struct socket, ->ops->poll */
#include <net/sock.h>

#include "vms_internal.h"       /* struct vms_proc / SS__* / vms_bg_pollfd_args */
#include "exec_kbackend.h"      /* exec_socket_t / exec_socket_raw / exec_socket_release */
#include "../kernel-core/vms_bg_core.h"  /* vms_bg_ref_socket -- the core boundary */

/* ---- readiness-only poll fd ------------------------------------------------
 * A real Linux fd whose .poll delegates to the executive socket's own poll, and
 * which has NO read/write ops. Its private_data is a HELD exec_socket_t (a
 * reference taken by vms_bg_ref_socket), so the socket outlives a poll fd that is
 * still open when the channel is $DASSGN'd. */
static __poll_t vms_bg_pollfd_poll(struct file *file, struct poll_table_struct *wait)
{
    exec_socket_t bs = file->private_data;
    struct socket *sock = bs ? exec_socket_raw(bs) : NULL;

    if (!sock || !sock->ops || !sock->ops->poll)
        return EPOLLERR;
    /* Delegate to the host socket's poll: this registers the socket's wait
     * queue with `wait`, so poll()/select() blocks and wakes on real socket
     * readiness, and returns the socket's current mask (EPOLLIN/EPOLLOUT/...). */
    return sock->ops->poll(file, sock, wait);  /* NEGCTL bgsock-poll-always-ready */
}

static int vms_bg_pollfd_release(struct inode *inode, struct file *file)
{
    exec_socket_t bs = file->private_data;
    (void)inode;
    if (bs)
        exec_socket_release(bs);   /* drop the reference the fd held */
    return 0;
}

static const struct file_operations vms_bg_pollfd_fops = {
    .owner   = THIS_MODULE,
    .poll    = vms_bg_pollfd_poll,
    .release = vms_bg_pollfd_release,
    .llseek  = noop_llseek,
};

/*
 * VMS_IOCTL_BG_POLLFD -- hand back a real Linux readiness-only pollable fd for
 * the channel's socket. Item vms-22a (OpenSSH's poll()). The fd holds a
 * reference on the channel's socket (via vms_bg_ref_socket), released by the
 * fd's ->release, so it is safe for the channel to be $DASSGN'd while the poll
 * fd is still open.
 */
long vms_ioctl_bg_pollfd(struct vms_proc *proc, unsigned long arg)
{
    struct vms_bg_pollfd_args args;
    exec_socket_t bs;
    struct file *file;
    int fd;

    memset(&args, 0, sizeof(args));
    if (copy_from_user(&args, (const void __user *)arg, sizeof(args)))
        return -EFAULT;
    args.fd = -1;

    /* Look up + reference the channel's socket holder under the core's lock, so
     * it cannot be released out from under the poll fd. NULL = no channel, or no
     * socket yet (IO$_SETMODE never issued) -- both are SS$_IVCHAN, as before. */
    bs = vms_bg_ref_socket(proc, args.chan);
    if (!bs) {
        args.status = SS__IVCHAN;
        goto out;
    }

    fd = get_unused_fd_flags(O_CLOEXEC);
    if (fd < 0) {
        exec_socket_release(bs);
        args.status = SS__ABORT;
        goto out;
    }
    file = anon_inode_getfile("[bgpoll]", &vms_bg_pollfd_fops, bs,
                              O_RDONLY | O_CLOEXEC);
    if (IS_ERR(file)) {
        put_unused_fd(fd);
        exec_socket_release(bs);
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
