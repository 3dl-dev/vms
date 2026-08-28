/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_ssh_wrap.c - OVMX OpenSSH de-veneer dispatch layer (vms-4bf, parent
 * vms-843). The linker-`--wrap` analogue of DECC$SHR owning the socket fd
 * namespace: it lets UNMODIFIED OpenSSH dispatch EVERY fd syscall on the
 * connection to the executive BGn: veneer ($QIO), with NO AF_UNIX socketpair and
 * NO byte-pump threads (retiring ovmx_ssh_glue.c's bridge).
 *
 * HOW IT WORKS. ovmx_socket() returns a veneer HANDLE offset by OVMX_BGSOCK_BASE
 * (0x40000000), far above any real fd. The build links `ssh` with
 * `-Wl,--wrap=<call>` for every fd syscall OpenSSH makes on the connection, so
 * each `<call>` resolves to `__wrap_<call>` here. Each wrapper inspects the fd:
 *   - fd >= OVMX_BGSOCK_BASE  -> route to the veneer's ovmx_* ($QIO on BGn:).
 *   - otherwise               -> `__real_<call>` (the genuine libc syscall),
 *                                so stdin/stdout/pipes/keyfiles are untouched.
 * The connection fd OpenSSH holds is thus genuinely the VMS INET channel: the
 * kex.c banner atomicio (read/write), getpeername (known_hosts), setsockopt
 * (TCP_NODELAY) and the clientloop poll set all dispatch to a real $QIO.
 *
 * poll()/ppoll()/select(): a veneer handle is not a pollable fd, so these
 * wrappers substitute each veneer fd for its CACHED executive readiness fd
 * (ovmx_readyfd -> VMS_IOCTL_BG_POLLFD, which reflects the socket's true
 * readiness), call __real_*, then map the readiness revents back onto the
 * caller's array -- composing cleanly with OpenSSH's mixed (connection + stdio)
 * fd set. Data still moves ONLY through IO$_READVBLK/IO$_WRITEVBLK.
 *
 * INV-6 / Rule 9: ovmx_socket() fails ENODEV when /dev/vms is absent; no wrapper
 * ever falls back to a raw host socket.
 *
 * STATUS (vms-4bf): the socket SURFACE these wrappers call (getpeername/
 * getsockname/get+setsockopt/nonblock) is proven against a real /dev/vms by
 * tests/qemu/test_syssvc_bgsock_peername.c. Flipping the `ssh`/KEX build onto
 * this wrap path (OVMX_WRAP) and RE-PROVING the end-to-end KEX over it is the
 * follow-on that finally deletes the socketpair; until that proof lands, the
 * OVMX_VENEER socketpair path remains the proven default.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>         /* IPPROTO_TCP (AF_INET is in sys/socket.h) */

#include "ovmx/vms_bgsock.h"

/* __real_* — the genuine libc implementations the linker binds under --wrap. */
extern ssize_t __real_read(int fd, void *buf, size_t n);
extern ssize_t __real_write(int fd, const void *buf, size_t n);
extern int __real_close(int fd);
extern int __real_getpeername(int fd, struct sockaddr *addr, socklen_t *alen);
extern int __real_getsockname(int fd, struct sockaddr *addr, socklen_t *alen);
extern int __real_setsockopt(int fd, int lvl, int opt, const void *v, socklen_t l);
extern int __real_getsockopt(int fd, int lvl, int opt, void *v, socklen_t *l);
extern int __real_shutdown(int fd, int how);
extern int __real_fcntl(int fd, int cmd, ...);
extern int __real_poll(struct pollfd *fds, nfds_t nfds, int timeout);
extern int __real_ppoll(struct pollfd *fds, nfds_t nfds,
                        const struct timespec *tmo, const sigset_t *mask);
extern int __real_socket(int domain, int type, int protocol);
extern int __real_connect(int fd, const struct sockaddr *addr, socklen_t alen);
#ifdef OVMX_WRAP_SERVER
/* Server-path __real_* — only referenced when the sshd (server) build wraps
 * bind/listen/accept/accept4. The client (ssh) link does NOT pass --wrap for
 * these, so the linker would leave __real_bind &c. undefined; gate them (and
 * their __wrap_* below) on OVMX_WRAP_SERVER, which the 3c sshd build defines. */
extern int __real_bind(int fd, const struct sockaddr *addr, socklen_t alen);
extern int __real_listen(int fd, int backlog);
extern int __real_accept(int fd, struct sockaddr *addr, socklen_t *alen);
extern int __real_accept4(int fd, struct sockaddr *addr, socklen_t *alen, int flags);
#endif /* OVMX_WRAP_SERVER */

static int is_veneer(int fd)
{
    return fd >= OVMX_BGSOCK_BASE;
}

/* vms-9ac (full de-veneer): wrap socket()/connect() so UNMODIFIED OpenSSH's
 * ssh_create_socket() reaches the executive with NO source patch. An
 * AF_INET/SOCK_STREAM socket becomes a veneer handle (the executive IS the IP
 * stack, so every TCP socket belongs to it, not just the client connection);
 * everything else stays a real libc socket. connect() on a veneer handle is the
 * blocking ovmx_connect returning 0 on success -- OpenSSH's timeout_connect
 * treats connect()==0 as immediate success and skips the non-blocking/EINPROGRESS
 * dance, so no sshconnect source patch is needed. */
int __wrap_socket(int domain, int type, int protocol)
{
    if (domain == AF_INET && (type & 0x0f) == SOCK_STREAM &&
        (protocol == 0 || protocol == IPPROTO_TCP))
        return ovmx_socket(AF_INET, SOCK_STREAM, protocol);
    return __real_socket(domain, type, protocol);
}

int __wrap_connect(int fd, const struct sockaddr *addr, socklen_t alen)
{
    if (is_veneer(fd))
        return ovmx_connect(fd, addr, alen);
    return __real_connect(fd, addr, alen);
}

#ifdef OVMX_WRAP_SERVER
/* vms-0cd (server path): wrap bind/listen/accept so UNMODIFIED OpenSSH sshd's
 * listener rides BGn: -- the inbound analogue of the client's socket/connect.
 * accept mints a NEW veneer handle for the connection (ovmx_accept, vms-698), so
 * the accepted connection is $QIO-backed like any other veneer socket; there is
 * NO real fd and NO socketpair (the fabrication excised in vms-9ac stays gone).
 * Gated on OVMX_WRAP_SERVER: the client (ssh) link does not --wrap these, so
 * compiling them there would leave __real_bind &c. undefined. The 3c sshd build
 * defines OVMX_WRAP_SERVER and passes the matching --wrap=bind/listen/accept. */
int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t alen)
{
    if (is_veneer(fd))
        return ovmx_bind(fd, addr, alen);
    return __real_bind(fd, addr, alen);
}

int __wrap_listen(int fd, int backlog)
{
    if (is_veneer(fd))
        return ovmx_listen(fd, backlog);
    return __real_listen(fd, backlog);
}

int __wrap_accept(int fd, struct sockaddr *addr, socklen_t *alen)
{
    if (is_veneer(fd))
        return ovmx_accept(fd, addr, alen);
    return __real_accept(fd, addr, alen);
}

/* accept4() -- OpenSSH uses it (SOCK_CLOEXEC/NONBLOCK). The veneer handle carries
 * no O_CLOEXEC/NONBLOCK real-fd flags; O_NONBLOCK is tracked via ovmx_fcntl, and
 * CLOEXEC is moot (a handle is not a kernel fd), so the flags are dropped. */
int __wrap_accept4(int fd, struct sockaddr *addr, socklen_t *alen, int flags)
{
    if (is_veneer(fd)) {
        int h = ovmx_accept(fd, addr, alen);
        if (h >= 0 && (flags & O_NONBLOCK))
            (void)ovmx_fcntl(h, F_SETFL, O_NONBLOCK);
        return h;
    }
    return __real_accept4(fd, addr, alen, flags);
}
#endif /* OVMX_WRAP_SERVER */

ssize_t __wrap_read(int fd, void *buf, size_t n)
{
    if (is_veneer(fd))
        return ovmx_recv(fd, buf, n, 0);
    return __real_read(fd, buf, n);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    if (is_veneer(fd))
        return ovmx_send(fd, buf, n, 0);
    return __real_write(fd, buf, n);
}

int __wrap_close(int fd)
{
    if (is_veneer(fd))
        return ovmx_socket_close(fd);
    return __real_close(fd);
}

int __wrap_getpeername(int fd, struct sockaddr *addr, socklen_t *alen)
{
    if (is_veneer(fd))
        return ovmx_getpeername(fd, addr, alen);
    return __real_getpeername(fd, addr, alen);
}

int __wrap_getsockname(int fd, struct sockaddr *addr, socklen_t *alen)
{
    if (is_veneer(fd))
        return ovmx_getsockname(fd, addr, alen);
    return __real_getsockname(fd, addr, alen);
}

int __wrap_setsockopt(int fd, int lvl, int opt, const void *v, socklen_t l)
{
    if (is_veneer(fd))
        return ovmx_setsockopt(fd, lvl, opt, v, l);
    return __real_setsockopt(fd, lvl, opt, v, l);
}

int __wrap_getsockopt(int fd, int lvl, int opt, void *v, socklen_t *l)
{
    if (is_veneer(fd))
        return ovmx_getsockopt(fd, lvl, opt, v, l);
    return __real_getsockopt(fd, lvl, opt, v, l);
}

int __wrap_shutdown(int fd, int how)
{
    if (is_veneer(fd))
        return ovmx_shutdown(fd, how);
    return __real_shutdown(fd, how);
}

int __wrap_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;
    /* All fcntl commands OpenSSH issues on the connection fd (F_GETFL/F_SETFL,
     * F_SETFD) take a single int/void arg; extract one int uniformly. */
    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);
    if (is_veneer(fd))
        return ovmx_fcntl(fd, cmd, arg);
    return __real_fcntl(fd, cmd, arg);
}

/* ---- poll/ppoll/select: fold veneer handles into a real readiness fd set ---
 * For each caller pollfd whose fd is a veneer handle, poll the socket's cached
 * executive readiness fd instead, then copy the resulting revents back. Real
 * fds pass through untouched. */
static int wrap_poll_common(struct pollfd *fds, nfds_t nfds,
                            const struct timespec *tmo, const sigset_t *mask,
                            int timeout_ms, int is_ppoll)
{
    struct pollfd stackfds[16];
    struct pollfd *tmp = stackfds;
    nfds_t i;
    int rc, any_veneer = 0;

    for (i = 0; i < nfds; i++) {
        if (is_veneer(fds[i].fd)) { any_veneer = 1; break; }
    }
    if (!any_veneer) {
        return is_ppoll ? __real_ppoll(fds, nfds, tmo, mask)
                        : __real_poll(fds, nfds, timeout_ms);
    }

    if (nfds > (nfds_t)(sizeof(stackfds) / sizeof(stackfds[0]))) {
        tmp = calloc(nfds, sizeof(*tmp));
        if (tmp == NULL) { errno = ENOMEM; return -1; }
    }

    for (i = 0; i < nfds; i++) {
        tmp[i].events = fds[i].events;
        tmp[i].revents = 0;
        if (is_veneer(fds[i].fd)) {
            int rf = ovmx_readyfd(fds[i].fd);
            tmp[i].fd = rf;             /* readiness fd; -1 => poll ignores it */
        } else {
            tmp[i].fd = fds[i].fd;
        }
    }

    rc = is_ppoll ? __real_ppoll(tmp, nfds, tmo, mask)
                  : __real_poll(tmp, nfds, timeout_ms);

    for (i = 0; i < nfds; i++)
        fds[i].revents = tmp[i].revents;

    if (tmp != stackfds)
        free(tmp);
    return rc;
}

int __wrap_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    return wrap_poll_common(fds, nfds, NULL, NULL, timeout, 0);
}

int __wrap_ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo,
                 const sigset_t *mask)
{
    return wrap_poll_common(fds, nfds, tmo, mask, 0, 1);
}
