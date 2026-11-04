/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ovmx_decc_sockets.c - the C RTL sockets of DECC$SHR: every socket call is a
 * VMS system service on a BGn: channel (rd vms-99f). See
 * docs/design-bgsockets-veneer-ovmx.md section 8.
 *
 * WHAT THIS REPLACES. DECC$SHR whole-archives musl libc.a, and its socket(),
 * connect(), send(), recv(), select(), poll() ... were musl's: raw Linux socket
 * system calls. mk_decc_shr.sh binds every one of those universals -- and the
 * descriptor calls read/write/readv/writev/close/dup/dup2/fcntl/ioctl that a
 * program also uses on a socket -- to the ovmx_decc_* entry points below.
 *
 * THE DESIGN: A SOCKET IS A CHANNEL. As in TCP/IP Services' DECC$SHR, a socket
 * descriptor names a BGn: channel and every operation on it is a $QIO function
 * on that channel (bg_qio_func.h, the SAME function map sys$qio uses):
 *   socket()            $ASSIGN TCPIP$DEVICE: + IO$_SETMODE (create)
 *   connect()           IO$_ACCESS
 *   bind() / listen()   IO$_SETMODE (P1 address / P3 backlog)
 *   accept()            $ASSIGN + IO$_ACCESS|IO$M_ACCEPT
 *   send/write/...      IO$_WRITEVBLK
 *   recv/read/...       IO$_READVBLK
 *   shutdown()          IO$_DEACCESS          close()  $DASSGN
 *   [gs]etsockopt()     IO$_SETMODE / IO$_SENSEMODE with the P4 option list
 *                       (TCPIP$C_SOCKOPT / _TCPOPT / _IPOPT item_list_2)
 *   getsockname/peer    IO$_SENSEMODE (IO$K_BG_SENSE_LOCAL / _PEER)
 *   select() / poll()   IO$_SENSEMODE readiness wait (IO$K_BG_SENSE_READY):
 *                       the executive sleeps on the sockets' own wait queues.
 *   O_NONBLOCK          a readiness sense first; not ready -> EAGAIN (the
 *                       SS$_SUSPENDED of a non-blocking socket).
 * No Linux socket and no Linux descriptor is on the I/O path. The descriptor
 * NUMBER the program holds is reserved in the process's descriptor table by a
 * close-on-exec duplicate of the executive device descriptor, so no file can
 * be opened onto the same number; nothing is ever read, written or polled
 * through it. Ordinary files keep their existing C RTL path.
 *
 * HONEST FAILURE (INV-6). No /dev/vms -> $ASSIGN TCPIP$DEVICE: is
 * SS$_NOSUCHDEV -> socket() returns -1 with errno ENODEV. There is no fallback
 * to the host. What BGn: does not provide (UDP, raw, AF_INET6, AF_UNIX via
 * socket(), MSG_OOB/MSG_PEEK, FIONREAD, struct options) fails with its errno.
 * socketpair() stays musl's: an AF_UNIX pair inside the process (local IPC).
 *
 * LIMITS, each honest rather than hidden: a socket channel belongs to the
 * process that created it (as a VMS channel does), so a forked child's
 * operations on an inherited socket fail EBADF; stdio on a socket (fdopen)
 * reaches the reserving descriptor and fails; a poll/select that mixes sockets
 * with other descriptors alternates a short executive wait with a zero-timeout
 * poll of the others (follow-on: terminal readiness from the executive too).
 *
 * TRANSPORT. DECC$SHR cannot link libvmssys's vms_kif.c (it keeps its executive
 * descriptor in __thread storage; DECC$SHR is a non-TLS producer, vms-212) and
 * has no --use of LIBVMS$SHR, so this file keeps a PROCESS-WIDE descriptor of
 * its own, opened and driven ONLY through libvmssys's transport seam
 * (kif_transport_linux.c, rd vms-bbde), includes the one shared BGn:
 * marshalling source (vms_kif_bg_ops.h) and the one shared $QIO function map
 * (bg_qio_func.h).
 *
 * CLEAN-ROOM (Rule 8). The BSD sockets API is POSIX; the VMS side is the public
 * TCP/IP Services $QIO interface, with OVMX's own parameter layouts (iodef.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "vms_kif.h"
#include "kif_transport.h"
#include "ssdef.h"
#include "iodef.h"

/* ---- the process-wide executive descriptor --------------------------------
 * Opened on first use and re-opened in a forked child (the executive keys BG
 * channels on the calling process, so a child binds its own, as vms_kif.c's
 * kif_bind does). O_CLOEXEC: an exec'd image binds afresh. */
static int g_vms_fd = -1;
static pid_t g_vms_pid;
static pthread_mutex_t g_vms_lock = PTHREAD_MUTEX_INITIALIZER;

static int bg_bind_ok(void)
{
    pid_t me = getpid();
    int ok;

    pthread_mutex_lock(&g_vms_lock);
    if (g_vms_pid != me || g_vms_fd < 0) {
        if (g_vms_fd >= 0 && g_vms_pid != me)
            kif_xport_dev_close(g_vms_fd);
        g_vms_fd = kif_xport_dev_open();       /* the transport seam (O_CLOEXEC) */
        g_vms_pid = (g_vms_fd >= 0) ? me : 0;
    }
    ok = g_vms_fd >= 0;
    pthread_mutex_unlock(&g_vms_lock);
    return ok;
}

/* A failed ioctl's errno as the VMS status the caller is entitled to see -- the
 * same closed mapping as vms_kif.c's vms_kif_kerr_to_ss (both sides are ours). */
static uint32_t decc_kerr_to_ss(int err)
{
    switch (err) {
    case EFAULT: return SS$_ACCVIO;
    case ENOMEM: return SS$_INSFMEM;
    case ENOTTY: return SS$_ILLIOFUNC;
    case ESRCH:  return SS$_NONEXPR;
    default:     return SS$_BUGCHECK;
    }
}

#define KIF_CALL(req, argsp) do {                                      \
        int _rc = kif_xport_ioctl(g_vms_fd, (req), (argsp));           \
        if (_rc < 0)                                                   \
            return decc_kerr_to_ss(-_rc);                              \
    } while (0)
#define vms_memset memset
#define vms_memcpy memcpy
#define vms_strncpy strncpy

/* The BGn: requests: ONE source, shared with LIBVMSSYS$SHR's vms_kif.c. */
#include "vms_kif_bg_ops.h"
/* The $QIO function map: ONE source, shared with LIBVMS$SHR's sys$qio. */
#include "bg_qio_func.h"

/* One $QIOW on an executive BG channel; the completion status (the IOSB's). */
static uint32_t sock_qiow(uint32_t chan, uint32_t func, void *p1, uint32_t p2,
                          uint32_t p3, uintptr_t p4, uint32_t acc_chan,
                          uint32_t *actlen)
{
    uint32_t n = 0, st;

    st = bg_qio_func(chan, func, p1, p2, p3, p4, acc_chan, &n);
    if (actlen) *actlen = n;
    return st;
}

/* $ASSIGN TCPIP$DEVICE: -- a fresh BGn: channel. */
static uint32_t sock_assign(uint32_t *chan)
{
    return vms_kif_bg_create(chan, NULL, NULL, 0);
}

static void sock_dassgn(uint32_t chan)
{
    (void)vms_kif_bg_dassgn(chan);
}

/* VMS status -> errno, for the statuses the BGn: services return. `fail` is the
 * errno the operation reports for SS$_ABORT (the executive's "the host socket
 * refused it": ECONNREFUSED for connect, EPIPE for a send ...). */
static int ss_errno(uint32_t st, int fail)
{
    switch (st) {
    case SS$_NOSUCHDEV:  return ENODEV;
    case SS$_IVCHAN:     return EBADF;
    case SS$_BADPARAM:   return EINVAL;
    case SS$_INSFMEM:    return ENOMEM;
    case SS$_EXQUOTA:    return EMFILE;
    case SS$_ACCVIO:     return EFAULT;
    case SS$_UNSUPPORTED:
    case SS$_ILLIOFUNC:  return EOPNOTSUPP;
    case SS$_SUSPENDED:  return EAGAIN;
    default:             return fail;
    }
}

/* ---- descriptor -> channel -------------------------------------------------
 * A socket descriptor names a `struct dsock`: the channel, and the open-file
 * status (O_NONBLOCK) that dup() shares, as POSIX shares it across one open
 * file description. The channel is $DASSGNed when the last descriptor goes. */
#define DECC_SOCK_MAXFD 4096

struct dsock {
    uint32_t chan;          /* the executive BG channel */
    int      refs;          /* descriptors naming it */
    int      nonblock;      /* O_NONBLOCK */
};

static struct dsock *g_fd_sock[DECC_SOCK_MAXFD];
static struct dsock g_inherited;    /* a parent's socket, seen in a forked child */
static pid_t g_fd_pid;
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;

/* Under g_fd_lock: make the table this process's. In a forked child every entry
 * the parent left becomes g_inherited -- the channel is the parent's, so the
 * child's operations on it fail EBADF and never reach the reserving
 * descriptor; its close() just gives the number back. */
static void table_own(void)
{
    pid_t me = getpid();
    int i;

    if (g_fd_pid == me)
        return;
    for (i = 0; i < DECC_SOCK_MAXFD; i++)
        if (g_fd_sock[i])
            g_fd_sock[i] = &g_inherited;
    g_fd_pid = me;
}

/* The socket behind fd (NULL for any other descriptor, and *inherited set for a
 * parent's socket in a forked child). */
static struct dsock *fd_lookup(int fd, int *inherited)
{
    struct dsock *d = NULL;

    if (inherited) *inherited = 0;
    if (fd < 0 || fd >= DECC_SOCK_MAXFD)
        return NULL;
    pthread_mutex_lock(&g_fd_lock);
    table_own();
    d = g_fd_sock[fd];
    pthread_mutex_unlock(&g_fd_lock);
    if (d == &g_inherited) {
        if (inherited) *inherited = 1;
        d = NULL;
    }
    return d;
}

static struct dsock *fd_sock(int fd)
{
    return fd_lookup(fd, NULL);
}

/* A descriptor call on fd that must not reach the C RTL: a parent's socket. */
static int fd_inherited(int fd)
{
    int inh = 0;
    (void)fd_lookup(fd, &inh);
    if (inh)
        errno = EBADF;
    return inh;
}

/* Reserve a descriptor number for a socket: a close-on-exec duplicate of the
 * executive device descriptor, never read, written or polled. */
static int fd_reserve(void)
{
    int fd = fcntl(g_vms_fd, F_DUPFD_CLOEXEC, 0);

    if (fd >= DECC_SOCK_MAXFD) {
        close(fd);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

static void fd_install(int fd, struct dsock *d)
{
    pthread_mutex_lock(&g_fd_lock);
    table_own();
    g_fd_sock[fd] = d;
    d->refs++;
    pthread_mutex_unlock(&g_fd_lock);
}

/* Forget fd; when it was the channel's last descriptor, $DASSGN the channel. */
static void fd_release(int fd)
{
    struct dsock *d = NULL;

    pthread_mutex_lock(&g_fd_lock);
    if (fd >= 0 && fd < DECC_SOCK_MAXFD) {
        table_own();
        d = g_fd_sock[fd];
        g_fd_sock[fd] = NULL;
        if (d == &g_inherited || (d && --d->refs > 0))
            d = NULL;
    }
    pthread_mutex_unlock(&g_fd_lock);
    if (d) {
        sock_dassgn(d->chan);
        free(d);
    }
}

/* Give a freshly made channel its descriptor (SOCK_NONBLOCK applied). */
static int sock_adopt(uint32_t chan, int flags)
{
    struct dsock *d;
    int fd;

    d = calloc(1, sizeof(*d));
    fd = d ? fd_reserve() : -1;
    if (fd < 0) {
        int e = d ? errno : ENOMEM;
        free(d);
        sock_dassgn(chan);
        errno = e;
        return -1;
    }
    d->chan = chan;
    d->nonblock = (flags & SOCK_NONBLOCK) != 0;
    fd_install(fd, d);
    return fd;
}

/* The socket behind fd, or NULL with errno ENOTSOCK. */
static struct dsock *sock_of(int fd)
{
    struct dsock *d = fd_sock(fd);
    if (!d)
        errno = (fd < 0) ? EBADF : ENOTSOCK;
    return d;
}

/* Is the channel ready for `ev` right now? 1 yes, 0 no, -1 (errno) failed. */
static int sock_ready_now(struct dsock *d, uint16_t ev)
{
    struct vms_bg_selent e = { d->chan, ev, 0 };
    uint32_t n = 0, st;

    st = sock_qiow(d->chan, IO$_SENSEMODE, &e, 1, IO$K_BG_SENSE_READY, 0, 0, &n);
    if (!(st & 1)) {
        errno = ss_errno(st, EIO);
        return -1;
    }
    return n != 0 && (e.revents & (ev | VMS_BG_RDY_ERR | VMS_BG_RDY_HUP)) != 0;
}

/* ======================================================================
 * The socket entry points DECC$SHR exports (socket, decc$socket, ...).
 * ====================================================================== */

int ovmx_decc_socket(int domain, int type, int protocol)
{
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    uint32_t chan = 0, st;

    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM) { errno = EPROTOTYPE; return -1; }
    if (protocol != 0 && protocol != IPPROTO_TCP) { errno = EPROTONOSUPPORT; return -1; }

    st = sock_assign(&chan);                       /* $ASSIGN TCPIP$DEVICE: */
    if (!(st & 1)) {
        errno = ss_errno(st, ENFILE);              /* ENODEV with no executive */
        return -1;
    }
    st = sock_qiow(chan, IO$_SETMODE, NULL, IO$K_SOCK_STREAM, 0, 0, 0, NULL); /* NEGCTL decc-socket-bound-to-musl */
    if (!(st & 1)) {
        sock_dassgn(chan);
        errno = ss_errno(st, ENFILE);
        return -1;
    }
    return sock_adopt(chan, flags);
}

static int sockaddr_in_of(const struct sockaddr *addr, socklen_t len,
                          struct bg_qio_sockaddr_in *out)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;

    if (addr == NULL) { errno = EFAULT; return -1; }
    if (len < (socklen_t)sizeof(*sin) || sin->sin_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    out->family = AF_INET;
    out->port = sin->sin_port;
    out->addr = sin->sin_addr.s_addr;
    return 0;
}

static void sockaddr_in_to(const struct bg_qio_sockaddr_in *in,
                           struct sockaddr *addr, socklen_t *len)
{
    struct sockaddr_in sin;
    socklen_t n;

    if (addr == NULL || len == NULL)
        return;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = in->port;
    sin.sin_addr.s_addr = in->addr;
    n = *len < (socklen_t)sizeof(sin) ? *len : (socklen_t)sizeof(sin);
    memcpy(addr, &sin, n);
    *len = sizeof(sin);
}

int ovmx_decc_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct dsock *d = sock_of(fd);
    struct bg_qio_sockaddr_in sa;
    uint32_t st;

    if (!d || sockaddr_in_of(addr, len, &sa) != 0)
        return -1;
    st = sock_qiow(d->chan, IO$_ACCESS, &sa, sizeof(sa), 0, 0, 0, NULL);
    if (!(st & 1)) { errno = ss_errno(st, ECONNREFUSED); return -1; }
    return 0;
}

int ovmx_decc_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    struct dsock *d = sock_of(fd);
    struct bg_qio_sockaddr_in sa;
    uint32_t st;

    if (!d || sockaddr_in_of(addr, len, &sa) != 0)
        return -1;
    st = sock_qiow(d->chan, IO$_SETMODE, &sa, sizeof(sa), 0, 0, 0, NULL);
    if (!(st & 1)) { errno = ss_errno(st, EADDRINUSE); return -1; }
    return 0;
}

int ovmx_decc_listen(int fd, int backlog)
{
    struct dsock *d = sock_of(fd);
    uint32_t st;

    if (!d)
        return -1;
    if (backlog <= 0)
        backlog = 1;            /* P3 = 0 means "not a listen" in the map */
    st = sock_qiow(d->chan, IO$_SETMODE, NULL, 0, (uint32_t)backlog, 0, 0, NULL);
    if (!(st & 1)) { errno = ss_errno(st, EOPNOTSUPP); return -1; }
    return 0;
}

int ovmx_decc_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    struct dsock *d = sock_of(fd);
    struct bg_qio_sockaddr_in peer;
    uint32_t nchan = 0, st;

    if (!d)
        return -1;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) { errno = EINVAL; return -1; }
    /* A non-blocking listener must not sit in a blocking accept. */
    if (d->nonblock) {
        int r = sock_ready_now(d, VMS_BG_RDY_IN);
        if (r <= 0) { if (r == 0) errno = EAGAIN; return -1; }
    }
    st = sock_assign(&nchan);                      /* the channel that receives it */
    if (!(st & 1)) { errno = ss_errno(st, ENFILE); return -1; }
    memset(&peer, 0, sizeof(peer));
    st = sock_qiow(d->chan, IO$_ACCESS | IO$M_ACCEPT, &peer, sizeof(peer), 0, 0,
                   nchan, NULL);
    if (!(st & 1)) {
        sock_dassgn(nchan);
        errno = ss_errno(st, ECONNABORTED);
        return -1;
    }
    sockaddr_in_to(&peer, addr, len);
    return sock_adopt(nchan, flags);
}

int ovmx_decc_accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return ovmx_decc_accept4(fd, addr, len, 0);
}

int ovmx_decc_shutdown(int fd, int how)
{
    struct dsock *d = sock_of(fd);
    uint32_t st;

    if (!d)
        return -1;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) { errno = EINVAL; return -1; }
    /* IO$_DEACCESS shuts the connection in both directions (the BGn: driver has
     * no half-close yet): SHUT_RD/SHUT_WR therefore over-shut -- recorded as a
     * gap on sockets-api-gaps, not hidden. */
    st = sock_qiow(d->chan, IO$_DEACCESS, NULL, 0, 0, 0, 0, NULL);
    if (!(st & 1)) { errno = ss_errno(st, ENOTCONN); return -1; }
    return 0;
}

/* ---- data: IO$_WRITEVBLK / IO$_READVBLK ------------------------------------ */

#define DECC_SEND_FLAGS_OK (MSG_DONTWAIT | MSG_NOSIGNAL | MSG_EOR | MSG_MORE)
#define DECC_RECV_FLAGS_OK (MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL | MSG_CMSG_CLOEXEC)


static ssize_t sock_send(struct dsock *d, const void *buf, size_t len, int flags)
{
    uint32_t st, n = 0;

    if (flags & ~DECC_SEND_FLAGS_OK) { errno = EOPNOTSUPP; return -1; }
    if (len == 0)
        return 0;
    if (buf == NULL) { errno = EFAULT; return -1; }
    if (len > 0x7fffffffu)
        len = 0x7fffffffu;
    if (d->nonblock || (flags & MSG_DONTWAIT)) {
        int r = sock_ready_now(d, VMS_BG_RDY_OUT);
        if (r < 0) return -1;
        if (r == 0) { errno = EAGAIN; return -1; }
        if (len > VMS_BG_IOCTL_MAXLEN)
            len = VMS_BG_IOCTL_MAXLEN;     /* one piece: what fits now */
    }
    st = sock_qiow(d->chan, IO$_WRITEVBLK, (void *)buf, (uint32_t)len, 0, 0, 0, &n);
    if (!(st & 1)) {
        errno = ss_errno(st, EPIPE);   /* SS$_ABORT: the connection is gone */
        return -1;
    }
    return (ssize_t)n;
}

static ssize_t sock_recv(struct dsock *d, void *buf, size_t len, int flags)
{
    char *p = buf;
    size_t off = 0;

    if (flags & ~DECC_RECV_FLAGS_OK) { errno = EOPNOTSUPP; return -1; }   /* MSG_PEEK, MSG_OOB */
    if (len == 0)
        return 0;
    if (buf == NULL) { errno = EFAULT; return -1; }
    if (d->nonblock || (flags & MSG_DONTWAIT)) {
        int r = sock_ready_now(d, VMS_BG_RDY_IN);
        if (r < 0) return -1;
        if (r == 0) { errno = EAGAIN; return -1; }
    }
    for (;;) {
        uint32_t want = (len - off > 0x7fffffffu) ? 0x7fffffffu : (uint32_t)(len - off);
        uint32_t n = 0, st;

        st = sock_qiow(d->chan, IO$_READVBLK, p + off, want, 0, 0, 0, &n);
        if (!(st & 1)) {
            if (off)
                return (ssize_t)off;
            errno = ss_errno(st, ECONNRESET);
            return -1;
        }
        off += n;
        if (n == 0 || !(flags & MSG_WAITALL) || off >= len)
            return (ssize_t)off;
    }
}

ssize_t ovmx_decc_send(int fd, const void *buf, size_t len, int flags)
{
    struct dsock *d = sock_of(fd);
    return d ? sock_send(d, buf, len, flags) : -1;
}

ssize_t ovmx_decc_recv(int fd, void *buf, size_t len, int flags)
{
    struct dsock *d = sock_of(fd);
    return d ? sock_recv(d, buf, len, flags) : -1;
}

/* A BGn: socket is a connected TCP stream: a destination on sendto is the one it
 * is connected to (as for any connected TCP socket), and recvfrom's source is the
 * peer, read back from the executive. */
ssize_t ovmx_decc_sendto(int fd, const void *buf, size_t len, int flags,
                         const struct sockaddr *to, socklen_t tolen)
{
    (void)to; (void)tolen;
    return ovmx_decc_send(fd, buf, len, flags);
}

static int sock_name(struct dsock *d, int peer, struct sockaddr *addr, socklen_t *len)
{
    struct bg_qio_sockaddr_in sa;
    uint32_t st;

    if (addr == NULL || len == NULL) { errno = EFAULT; return -1; }
    memset(&sa, 0, sizeof(sa));
    st = sock_qiow(d->chan, IO$_SENSEMODE, &sa, sizeof(sa),
                   peer ? IO$K_BG_SENSE_PEER : IO$K_BG_SENSE_LOCAL, 0, 0, NULL);
    if (!(st & 1)) { errno = ss_errno(st, ENOTCONN); return -1; }
    sockaddr_in_to(&sa, addr, len);
    return 0;
}

static void peer_into(struct dsock *d, struct sockaddr *from, socklen_t *fromlen)
{
    if (from != NULL && fromlen != NULL && sock_name(d, 1, from, fromlen) != 0)
        *fromlen = 0;
}

ssize_t ovmx_decc_recvfrom(int fd, void *buf, size_t len, int flags,
                           struct sockaddr *from, socklen_t *fromlen)
{
    struct dsock *d = sock_of(fd);
    ssize_t n;

    if (!d)
        return -1;
    n = sock_recv(d, buf, len, flags);
    if (n >= 0)
        peer_into(d, from, fromlen);
    return n;
}

/* msg_iovlen / msg_controllen are read through their low 32 bits: correct for
 * both the musl (int + pad) and glibc (size_t) struct msghdr on LP64. */
ssize_t ovmx_decc_sendmsg(int fd, const struct msghdr *m, int flags)
{
    struct dsock *d = sock_of(fd);
    int i, iovlen;
    ssize_t total = 0;

    if (!d)
        return -1;
    if (m == NULL) { errno = EFAULT; return -1; }
    if ((socklen_t)m->msg_controllen != 0) { errno = EOPNOTSUPP; return -1; }  /* no SCM_RIGHTS over BGn: */
    iovlen = (int)m->msg_iovlen;
    for (i = 0; i < iovlen; i++) {
        ssize_t n;
        if (m->msg_iov[i].iov_len == 0)
            continue;
        n = sock_send(d, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len,
                      total ? (flags | MSG_NOSIGNAL) : flags);
        if (n < 0)
            return total ? total : -1;
        total += n;
        if ((size_t)n < m->msg_iov[i].iov_len)
            break;
    }
    return total;
}

static ssize_t sock_readv(struct dsock *d, const struct iovec *iov, int iovcnt, int flags)
{
    ssize_t total = 0;
    int i;

    for (i = 0; i < iovcnt; i++) {
        ssize_t n;
        if (iov[i].iov_len == 0)
            continue;
        /* only the first piece may wait: after data has arrived, take what is
         * already there and stop */
        if (total) {
            int r = sock_ready_now(d, VMS_BG_RDY_IN);
            if (r <= 0)
                break;
        }
        n = sock_recv(d, iov[i].iov_base, iov[i].iov_len, flags);
        if (n < 0)
            return total ? total : -1;
        total += n;
        if (n == 0 || (size_t)n < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t ovmx_decc_recvmsg(int fd, struct msghdr *m, int flags)
{
    struct dsock *d = sock_of(fd);
    ssize_t n;

    if (!d)
        return -1;
    if (m == NULL) { errno = EFAULT; return -1; }
    if (flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) { errno = EOPNOTSUPP; return -1; }
    n = sock_readv(d, m->msg_iov, (int)m->msg_iovlen, flags);
    if (n >= 0) {
        m->msg_flags = 0;
        if (m->msg_name != NULL) {
            socklen_t nl = m->msg_namelen;
            peer_into(d, (struct sockaddr *)m->msg_name, &nl);
            m->msg_namelen = nl;
        }
        if (m->msg_control != NULL)
            m->msg_controllen = 0;
    }
    return n;
}

/* ---- names and options: IO$_SENSEMODE / IO$_SETMODE ------------------------ */

int ovmx_decc_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    struct dsock *d = sock_of(fd);
    return d ? sock_name(d, 0, addr, len) : -1;
}

int ovmx_decc_getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    struct dsock *d = sock_of(fd);
    return d ? sock_name(d, 1, addr, len) : -1;
}

/* The P4 option list's type for a C level, or 0 (a level BGn: does not take). */
static uint16_t optlist_type(int level)
{
    switch (level) {
    case SOL_SOCKET:  return TCPIP$C_SOCKOPT;
    case IPPROTO_TCP: return TCPIP$C_TCPOPT;
    case IPPROTO_IP:  return TCPIP$C_IPOPT;
    default:          return 0;
    }
}

/* One option through the IO$_SETMODE / IO$_SENSEMODE P4 option list: a
 * descriptor of the level's list type naming one item_list_2 entry. */
static int sock_opt(struct dsock *d, int level, int name, void *val,
                    unsigned short len, int get)
{
    struct tcpip_item_list_2 item, desc;
    uint16_t type = optlist_type(level);
    uint32_t st;

    if (type == 0 || name < 0 || name > 0xffff) { errno = ENOPROTOOPT; return -1; }
    item.length = len;
    item.type = (unsigned short)name;
    item.address = val;
    desc.length = sizeof(item);
    desc.type = type;
    desc.address = &item;
    if (get)
        st = sock_qiow(d->chan, IO$_SENSEMODE, NULL, 0, 0, (uintptr_t)&desc, 0, NULL);
    else
        st = sock_qiow(d->chan, IO$_SETMODE, NULL, 0, 0, (uintptr_t)&desc, 0, NULL);
    if (!(st & 1)) { errno = (st == SS$_BADPARAM) ? ENOPROTOOPT : ss_errno(st, EINVAL); return -1; }
    return 0;
}

int ovmx_decc_setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    struct dsock *d = sock_of(fd);
    int v;

    if (!d)
        return -1;
    if (val == NULL || len != (socklen_t)sizeof(int)) {
        /* Only integer options cross the BGn: option list here (SO_LINGER,
         * SO_RCVTIMEO ... are structs): refused, never silently swallowed. */
        errno = ENOPROTOOPT;
        return -1;
    }
    memcpy(&v, val, sizeof(int));
    return sock_opt(d, level, name, &v, sizeof(int), 0);
}

int ovmx_decc_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    struct dsock *d = sock_of(fd);
    int v = 0;

    if (!d)
        return -1;
    if (val == NULL || len == NULL || *len < (socklen_t)sizeof(int)) { errno = EINVAL; return -1; }
    if (sock_opt(d, level, name, &v, sizeof(int), 1) != 0)
        return -1;
    memcpy(val, &v, sizeof(int));
    *len = sizeof(int);
    return 0;
}

/* ======================================================================
 * The descriptor calls. On a socket descriptor each is a $QIO on its channel;
 * on any other descriptor it is the C RTL's own call, unchanged.
 * ====================================================================== */

ssize_t ovmx_decc_read(int fd, void *buf, size_t len)
{
    struct dsock *d = fd_sock(fd);
    if (d) return sock_recv(d, buf, len, 0);   /* NEGCTL decc-socket-read-via-musl */
    return fd_inherited(fd) ? -1 : read(fd, buf, len);
}

ssize_t ovmx_decc_write(int fd, const void *buf, size_t len)
{
    struct dsock *d = fd_sock(fd);
    if (d) return sock_send(d, buf, len, 0);
    return fd_inherited(fd) ? -1 : write(fd, buf, len);
}

ssize_t ovmx_decc_readv(int fd, const struct iovec *iov, int iovcnt)
{
    struct dsock *d = fd_sock(fd);
    if (!d)
        return fd_inherited(fd) ? -1 : readv(fd, iov, iovcnt);
    if (iovcnt < 0 || (iovcnt > 0 && iov == NULL)) { errno = EINVAL; return -1; }
    return sock_readv(d, iov, iovcnt, 0);
}

ssize_t ovmx_decc_writev(int fd, const struct iovec *iov, int iovcnt)
{
    struct dsock *d = fd_sock(fd);
    ssize_t total = 0;
    int i;

    if (!d)
        return fd_inherited(fd) ? -1 : writev(fd, iov, iovcnt);
    if (iovcnt < 0 || (iovcnt > 0 && iov == NULL)) { errno = EINVAL; return -1; }
    for (i = 0; i < iovcnt; i++) {
        ssize_t n;
        if (iov[i].iov_len == 0)
            continue;
        n = sock_send(d, iov[i].iov_base, iov[i].iov_len, total ? MSG_NOSIGNAL : 0);
        if (n < 0)
            return total ? total : -1;
        total += n;
        if ((size_t)n < iov[i].iov_len)
            break;
    }
    return total;
}

int ovmx_decc_close(int fd)
{
    int inh = 0;
    if (fd_lookup(fd, &inh) || inh)
        fd_release(fd);             /* $DASSGN with the channel's last descriptor */
    return close(fd);               /* gives back the reserved number */
}

int ovmx_decc_dup(int fd)
{
    struct dsock *d = fd_sock(fd);
    int nfd;

    if (!d)
        return dup(fd);
    nfd = fd_reserve();
    if (nfd < 0)
        return -1;
    fd_install(nfd, d);
    return nfd;
}

int ovmx_decc_dup2(int fd, int nfd)
{
    struct dsock *d = fd_sock(fd);
    int r;

    if (fd == nfd)
        return (d || fcntl(fd, F_GETFD) >= 0) ? nfd : -1;
    if (nfd < 0 || (d && nfd >= DECC_SOCK_MAXFD)) { errno = EBADF; return -1; }
    if (!d && fd_inherited(fd))
        return -1;
    r = dup2(fd, nfd);              /* the reserving descriptor moves with it */
    if (r < 0)
        return -1;
    {
        int inh = 0;
        if (fd_lookup(nfd, &inh) || inh)
            fd_release(nfd);        /* nfd's old socket loses a descriptor */
    }
    if (d) {
        (void)fcntl(nfd, F_SETFD, FD_CLOEXEC);
        fd_install(nfd, d);
    }
    return r;
}

int ovmx_decc_fcntl(int fd, int cmd, ...)
{
    struct dsock *d = fd_sock(fd);
    va_list ap;
    long arg;

    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    if (d) {
        switch (cmd) {
        case F_GETFL:
            return O_RDWR | (d->nonblock ? O_NONBLOCK : 0);
        case F_SETFL:
            d->nonblock = (arg & O_NONBLOCK) != 0;
            return 0;
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int nfd = fcntl(fd, F_DUPFD_CLOEXEC, (int)arg);
            if (nfd < 0)
                return -1;
            if (nfd >= DECC_SOCK_MAXFD) { close(nfd); errno = EMFILE; return -1; }
            fd_install(nfd, d);
            return nfd;
        }
        case F_GETFD:
        case F_SETFD:
            break;                  /* the descriptor's own flag: the C RTL's */
        default:
            errno = EINVAL;         /* locks, owners ... are not socket operations here */
            return -1;
        }
    }
    return fcntl(fd, cmd, arg);
}

int ovmx_decc_ioctl(int fd, unsigned long req, ...)
{
    struct dsock *d = fd_sock(fd);
    va_list ap;
    void *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (!d)
        return ioctl(fd, req, arg);
    if (req == FIONBIO) {
        if (arg == NULL) { errno = EFAULT; return -1; }
        d->nonblock = *(int *)arg != 0;
        return 0;
    }
    errno = (req == FIONREAD) ? EOPNOTSUPP : ENOTTY;   /* no byte count from BGn: yet */
    return -1;
}

/* ---- select() / poll(): the IO$_SENSEMODE readiness wait ------------------- */

#define MIXED_SLICE_MS 10

static int ms_left(const struct timespec *end)
{
    struct timespec now;
    long ms;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (end->tv_sec - now.tv_sec) * 1000L + (end->tv_nsec - now.tv_nsec) / 1000000L;
    return ms < 0 ? 0 : (ms > 0x7fffffffL ? 0x7fffffff : (int)ms);
}

static short poll_rev(uint16_t r, short asked)
{
    short v = 0;
    if (r & VMS_BG_RDY_IN)   v |= asked & (POLLIN | POLLRDNORM);
    if (r & VMS_BG_RDY_OUT)  v |= asked & (POLLOUT | POLLWRNORM);
    if (r & VMS_BG_RDY_ERR)  v |= POLLERR;
    if (r & VMS_BG_RDY_HUP)  v |= POLLHUP;
    if (r & VMS_BG_RDY_NVAL) v |= POLLNVAL;
    return v;
}

int ovmx_decc_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    struct vms_bg_selent ent[VMS_BG_SELECT_MAX];
    int idx[VMS_BG_SELECT_MAX];
    struct pollfd *other = NULL;
    int *oidx = NULL;
    nfds_t i;
    int ns = 0, no = 0, ready = 0, waitms;
    struct timespec end;

    for (i = 0; i < nfds; i++) {
        struct dsock *d = fds[i].fd >= 0 ? fd_sock(fds[i].fd) : NULL;
        fds[i].revents = 0;
        if (!d)
            continue;
        if (ns >= (int)VMS_BG_SELECT_MAX) { errno = EINVAL; return -1; }
        ent[ns].chan = d->chan;
        ent[ns].events = (uint16_t)(((fds[i].events & (POLLIN | POLLRDNORM)) ? VMS_BG_RDY_IN : 0) |
                                    ((fds[i].events & (POLLOUT | POLLWRNORM)) ? VMS_BG_RDY_OUT : 0));
        ent[ns].revents = 0;
        idx[ns++] = (int)i;
    }
    if (ns == 0)
        return poll(fds, nfds, timeout);           /* no socket: the C RTL's poll */

    if (nfds > (nfds_t)ns) {
        other = calloc(nfds - (nfds_t)ns, sizeof(*other));
        oidx = calloc(nfds - (nfds_t)ns, sizeof(*oidx));
        if (!other || !oidx) { free(other); free(oidx); errno = ENOMEM; return -1; }
        for (i = 0; i < nfds; i++) {
            if (fds[i].fd >= 0 && fd_sock(fds[i].fd))
                continue;
            other[no] = fds[i];
            oidx[no++] = (int)i;
        }
    }
    if (timeout > 0) {
        clock_gettime(CLOCK_MONOTONIC, &end);
        end.tv_sec += timeout / 1000;
        end.tv_nsec += (long)(timeout % 1000) * 1000000L;
        if (end.tv_nsec >= 1000000000L) { end.tv_sec++; end.tv_nsec -= 1000000000L; }
    }
    for (;;) {
        uint32_t n = 0, st;
        int k, orc = 0;

        /* Sockets alone: the whole wait is the executive's. Mixed: a slice. */
        if (timeout == 0)
            waitms = 0;
        else if (no == 0)
            waitms = (timeout < 0) ? -1 : ms_left(&end);
        else
            waitms = (timeout < 0 || ms_left(&end) > MIXED_SLICE_MS)
                         ? MIXED_SLICE_MS : ms_left(&end);
        st = sock_qiow(ent[0].chan, IO$_SENSEMODE, ent, (uint32_t)ns,
                       IO$K_BG_SENSE_READY, (uintptr_t)(intptr_t)waitms, 0, &n);
        if (!(st & 1) && st != SS$_IVCHAN) {
            free(other); free(oidx);
            errno = (st == SS$_ABORT) ? EINTR : ss_errno(st, EIO);
            return -1;
        }
        ready = 0;
        for (k = 0; k < ns; k++) {
            fds[idx[k]].revents = poll_rev(ent[k].revents, fds[idx[k]].events);
            if (fds[idx[k]].revents)
                ready++;
        }
        if (no) {
            orc = poll(other, (nfds_t)no, 0);
            if (orc < 0) { free(other); free(oidx); return -1; }
            for (k = 0; k < no; k++) {
                fds[oidx[k]].revents = other[k].revents;
                if (other[k].revents)
                    ready++;
            }
        }
        if (ready || timeout == 0 || (timeout > 0 && ms_left(&end) == 0))
            break;
    }
    free(other);
    free(oidx);
    return ready;
}

int ovmx_decc_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    struct pollfd *p;
    int i, n = 0, any_sock = 0, rc, timeout, count = 0;

    if (nfds < 0 || nfds > FD_SETSIZE) { errno = EINVAL; return -1; }
    for (i = 0; i < nfds && !any_sock; i++)
        if (((r && FD_ISSET(i, r)) || (w && FD_ISSET(i, w)) || (e && FD_ISSET(i, e))) &&
            fd_sock(i))
            any_sock = 1;
    if (!any_sock)
        return select(nfds, r, w, e, tv);          /* no socket: the C RTL's select */

    if (tv && (tv->tv_sec < 0 || tv->tv_usec < 0)) { errno = EINVAL; return -1; }
    timeout = tv ? (int)(tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000) : -1;
    p = calloc((size_t)nfds, sizeof(*p));
    if (!p) { errno = ENOMEM; return -1; }
    for (i = 0; i < nfds; i++) {
        short ev = 0;
        if (r && FD_ISSET(i, r)) ev |= POLLIN;
        if (w && FD_ISSET(i, w)) ev |= POLLOUT;
        if (e && FD_ISSET(i, e)) ev |= POLLPRI;
        if (!ev) continue;
        p[n].fd = i;
        p[n++].events = ev;
    }
    rc = ovmx_decc_poll(p, (nfds_t)n, timeout);
    if (rc < 0) { free(p); return -1; }
    for (i = 0; i < n; i++)
        if (p[i].revents & POLLNVAL) { free(p); errno = EBADF; return -1; }
    if (r) FD_ZERO(r);
    if (w) FD_ZERO(w);
    if (e) FD_ZERO(e);
    for (i = 0; i < n; i++) {
        short v = p[i].revents;
        if (r && (p[i].events & POLLIN) && (v & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[i].fd, r); count++; }
        if (w && (p[i].events & POLLOUT) && (v & (POLLOUT | POLLERR))) { FD_SET(p[i].fd, w); count++; }
        if (e && (p[i].events & POLLPRI) && (v & POLLPRI)) { FD_SET(p[i].fd, e); count++; }
    }
    free(p);
    return count;
}


/* ======================================================================
 * Name resolution: numeric only until the BGn: UDP resolver (rd vms-a9e7).
 * Never musl's resolver (/etc/hosts + DNS over Linux UDP sockets).
 * ====================================================================== */

static int numeric_port(const char *serv, uint16_t *port)
{
    char *end;
    unsigned long v;

    if (serv == NULL || *serv == '\0') { *port = 0; return serv == NULL ? 0 : -1; }
    errno = 0;
    v = strtoul(serv, &end, 10);
    if (*end != '\0' || errno != 0 || v > 65535)
        return -1;      /* a service NAME: the services database is not wired */
    *port = (uint16_t)v;
    return 0;
}

int ovmx_decc_getaddrinfo(const char *node, const char *serv,
                          const struct addrinfo *hints, struct addrinfo **res)
{
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int socktype = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    int flags = hints ? hints->ai_flags : 0;
    struct in_addr a;
    uint16_t port = 0;
    struct addrinfo *ai;
    struct sockaddr_in *sin;

    if (res == NULL)
        return EAI_SYSTEM;
    *res = NULL;
    if (node == NULL && serv == NULL)
        return EAI_NONAME;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;                     /* BGn: is AF_INET */
    if (socktype != 0 && socktype != SOCK_STREAM)
        return EAI_SOCKTYPE;                   /* BGn: sockets are TCP streams */
    if (protocol != 0 && protocol != IPPROTO_TCP)
        return EAI_SERVICE;
    if (numeric_port(serv, &port) != 0)
        return EAI_SERVICE;

    if (node == NULL)
        a.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    else if ((flags & AI_NUMERICHOST) ? inet_pton(AF_INET, node, &a) != 1
                                      : inet_aton(node, &a) == 0)
        /* A host NAME. The resolver over BGn: UDP is rd vms-a9e7; until then a
         * name is a non-recoverable resolution failure, never musl's DNS. */
        return (flags & AI_NUMERICHOST) ? EAI_NONAME : EAI_FAIL;

    ai = calloc(1, sizeof(*ai) + sizeof(*sin));
    if (ai == NULL)
        return EAI_MEMORY;
    sin = (struct sockaddr_in *)(ai + 1);
    sin->sin_family = AF_INET;
    sin->sin_port = htons(port);
    sin->sin_addr = a;
    ai->ai_family = AF_INET;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addrlen = sizeof(*sin);
    ai->ai_addr = (struct sockaddr *)sin;
    if ((flags & AI_CANONNAME) && node != NULL) {
        ai->ai_canonname = strdup(node);
        if (ai->ai_canonname == NULL) { free(ai); return EAI_MEMORY; }
    }
    *res = ai;
    return 0;
}

void ovmx_decc_freeaddrinfo(struct addrinfo *ai)
{
    while (ai != NULL) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_canonname);
        free(ai);
        ai = next;
    }
}

int ovmx_decc_getnameinfo(const struct sockaddr *sa, socklen_t salen,
                          char *host, socklen_t hostlen,
                          char *serv, socklen_t servlen, int flags)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    char buf[16];

    if (sa == NULL || salen < (socklen_t)sizeof(*sin))
        return EAI_FAMILY;
    if (sa->sa_family != AF_INET)
        return EAI_FAMILY;
    if (host != NULL && hostlen > 0) {
        if (flags & NI_NAMEREQD)
            return EAI_FAIL;                   /* no reverse resolver (vms-a9e7) */
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) == NULL)
            return EAI_SYSTEM;
        if (strlen(buf) + 1 > (size_t)hostlen)
            return EAI_OVERFLOW;
        strcpy(host, buf);
    }
    if (serv != NULL && servlen > 0) {
        unsigned p = ntohs(sin->sin_port), i = 0, d = 10000;
        int started = 0;
        for (; d > 0; d /= 10) {
            unsigned digit = (p / d) % 10;
            if (digit || started || d == 1) { buf[i++] = (char)('0' + digit); started = 1; }
        }
        buf[i] = '\0';
        if (i + 1 > (unsigned)servlen)
            return EAI_OVERFLOW;
        strcpy(serv, buf);
    }
    return 0;
}

/* gethostbyname: numeric literals only (static result, as the interface is). */
static struct {
    struct hostent he;
    struct in_addr addr;
    char *addrs[2];
    char *aliases[1];
    char name[16];
} g_host;
static pthread_mutex_t g_host_lock = PTHREAD_MUTEX_INITIALIZER;

struct hostent *ovmx_decc_gethostbyname(const char *name)
{
    struct in_addr a;

    if (name == NULL || inet_aton(name, &a) == 0) {
        h_errno = NO_RECOVERY;                 /* no resolver yet: rd vms-a9e7 */
        return NULL;
    }
    pthread_mutex_lock(&g_host_lock);
    memset(&g_host, 0, sizeof(g_host));
    g_host.addr = a;
    (void)inet_ntop(AF_INET, &a, g_host.name, sizeof(g_host.name));
    g_host.addrs[0] = (char *)&g_host.addr;
    g_host.he.h_name = g_host.name;
    g_host.he.h_aliases = g_host.aliases;
    g_host.he.h_addrtype = AF_INET;
    g_host.he.h_length = (int)sizeof(struct in_addr);
    g_host.he.h_addr_list = g_host.addrs;
    pthread_mutex_unlock(&g_host_lock);
    return &g_host.he;
}

struct hostent *ovmx_decc_gethostbyaddr(const void *addr, socklen_t len, int type)
{
    (void)addr; (void)len; (void)type;
    h_errno = NO_RECOVERY;                     /* reverse lookup is DNS: rd vms-a9e7 */
    return NULL;
}

/* The services database: musl's reads the HOST'S /etc/services, which is not
 * the VMS node's services database (not wired yet). Not found, honestly. */
struct servent *ovmx_decc_getservbyname(const char *name, const char *proto)
{
    (void)name; (void)proto;
    return NULL;
}

struct servent *ovmx_decc_getservbyport(int port, const char *proto)
{
    (void)port; (void)proto;
    return NULL;
}

/* Interface names/indexes: musl's open a Linux socket and ioctl the HOST's
 * interfaces. The VMS interface database (TCPIP SHOW INTERFACE's) is not
 * exposed to the C RTL yet; fail rather than describe the host. */
unsigned ovmx_decc_if_nametoindex(const char *name)
{
    (void)name;
    errno = ENODEV;
    return 0;
}

char *ovmx_decc_if_indextoname(unsigned index, char *name)
{
    (void)index; (void)name;
    errno = ENXIO;
    return NULL;
}

void *ovmx_decc_if_nameindex(void)
{
    errno = ENOSYS;
    return NULL;
}
