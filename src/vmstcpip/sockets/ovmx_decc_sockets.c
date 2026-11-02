/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ovmx_decc_sockets.c - the C RTL sockets of DECC$SHR, over the executive BGn:
 * socket (rd vms-99f). See docs/design-bgsockets-veneer-ovmx.md section 8.
 *
 * WHAT THIS REPLACES. DECC$SHR whole-archives musl libc.a, and until this file
 * its socket(), connect(), send(), recv(), getaddrinfo() ... were musl's: raw
 * Linux socket system calls. A VMS-linked program that called socket() did
 * host-side Linux networking dressed up as VMS. mk_decc_shr.sh now binds every
 * one of those universals to the ovmx_decc_* entry points below, so a VMS
 * program's socket is an executive BG channel and no code path in DECC$SHR's
 * exported surface reaches a Linux socket system call.
 *
 * THE DESIGN: A SOCKET IS A REAL FD WHOSE FILE IS THE EXECUTIVE SOCKET.
 *   socket()  = $ASSIGN TCPIP$DEVICE: + IO$_SETMODE (ovmx_socket), then the
 *               executive MATERIALIZES the channel's socket as a real fd
 *               (VMS_IOCTL_BG_MATERIALIZE_FD -> a vms.ko "[bgconn]" anon inode
 *               whose read/write/poll go to the SAME executive-resident socket
 *               IO$_READVBLK/IO$_WRITEVBLK drive). The fd is what the program
 *               gets back; this file remembers fd -> veneer handle.
 *   read/write/readv/writev/poll/select/fcntl(O_NONBLOCK)/dup on that fd are
 *               UNCHANGED C RTL calls: the kernel routes them to vms.ko's file
 *               operations, so every byte still transits the executive socket.
 *   connect/bind/listen/accept/shutdown need the channel: IO$_ACCESS etc. on
 *               the remembered handle (ovmx_*). accept materializes the new
 *               channel the same way.
 *   send/recv/sendto/recvfrom/sendmsg/recvmsg = read/write on the fd (the
 *               flags this layer cannot honour fail EOPNOTSUPP, never ignored).
 *   getsockname/getpeername/setsockopt/getsockopt = ioctls on the fd itself,
 *               answered by the executive from the held socket (the TRUE peer).
 *   close()   = $DASSGN the channel, then close the fd; the transport FIN
 *               happens when the last reference to the executive socket drops.
 * A program can therefore mix sockets with files and terminals in one poll()
 * set, hand a socket to a child by dup2(), and never meets a handle that is not
 * an fd. No Linux socket is ever created in the process.
 *
 * HONEST FAILURE (INV-6). No /dev/vms -> $ASSIGN TCPIP$DEVICE: is SS$_NOSUCHDEV
 * -> socket() returns -1 with errno ENODEV. There is no fallback to the host's
 * socket(). A descriptor that is not an executive socket fails ENOTSOCK. What
 * BGn: does not provide (UDP, raw, AF_INET6, AF_UNIX via socket(), MSG_OOB/
 * MSG_PEEK) fails EAFNOSUPPORT/EPROTOTYPE/EOPNOTSUPP. socketpair() stays musl's:
 * it makes an AF_UNIX pair inside the process (local IPC, no network).
 *
 * NAME RESOLUTION. The DNS resolver over BGn: UDP is rd vms-a9e7. Until it is
 * wired here, getaddrinfo/getnameinfo/gethostbyname/gethostbyaddr resolve
 * NUMERIC addresses only and fail names honestly (EAI_FAIL / NO_RECOVERY); they
 * never reach musl's resolver, which reads /etc/hosts and sends DNS over Linux
 * UDP sockets. getservbyname/getservbyport return NULL: musl's read the HOST'S
 * /etc/services, and the VMS services database is not wired yet. The interface
 * calls (if_nametoindex & co) fail: musl's open a Linux socket to ask the host.
 *
 * TRANSPORT. DECC$SHR cannot link libvmssys's vms_kif.c (it keeps its executive
 * descriptor in __thread storage; DECC$SHR is a non-TLS producer, vms-212), so
 * this file keeps a PROCESS-WIDE descriptor of its own, opened and driven ONLY
 * through libvmssys's transport seam (kif_transport_linux.c, linked into
 * DECC$SHR alongside this file -- rd vms-bbde), and includes the one shared
 * BGn: marshalling source, src/libvmssys/vms_kif_bg_ops.h.
 *
 * CLEAN-ROOM (Rule 8). The BSD sockets API is POSIX; the VMS side is the public
 * TCP/IP Services $QIO interface already implemented by vms.ko's BGn: driver.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
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
#include "kif_calls.h"
#include "kif_transport.h"
#include "ssdef.h"
#include "vms_bgsock.h"

/* ---- the process-wide executive descriptor --------------------------------
 * Opened on first use and re-opened in a forked child (the descriptor number a
 * child inherits is a copy of its parent's; the executive keys BG channels on
 * the calling process, so a child binds its own, as vms_kif.c's kif_bind does).
 * O_CLOEXEC: an exec'd image binds afresh. */
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

/* ---- fd -> veneer handle --------------------------------------------------
 * Only sockets THIS image created (socket/accept) are here: they are the ones
 * whose executive channel this process holds. A [bgconn] fd that arrived some
 * other way (dup, inheritance across exec) still carries data, names and
 * options -- those go through the fd -- but has no channel to connect/listen. */
#define DECC_SOCK_MAXFD 4096
static int g_fd_handle[DECC_SOCK_MAXFD];      /* handle, or 0 */
static pthread_mutex_t g_fd_lock = PTHREAD_MUTEX_INITIALIZER;

static int fd_handle(int fd)
{
    int h;
    if (fd < 0 || fd >= DECC_SOCK_MAXFD)
        return 0;
    pthread_mutex_lock(&g_fd_lock);
    h = g_fd_handle[fd];
    pthread_mutex_unlock(&g_fd_lock);
    return h;
}

static void fd_set_handle(int fd, int h)
{
    pthread_mutex_lock(&g_fd_lock);
    g_fd_handle[fd] = h;
    pthread_mutex_unlock(&g_fd_lock);
}

/* Is fd an executive socket ([bgconn])? Asked of the fd itself: the executive
 * answers the name query on its own file and refuses (ENOTTY) on any other. */
static int fd_is_bgconn(int fd)
{
    struct vms_bg_name_args a;
    memset(&a, 0, sizeof(a));
    return kif_call(fd, KIF_SVC_BGCONN_GETNAME, &a) == 0;
}

static int is_sock(int fd)
{
    return fd_handle(fd) != 0 || fd_is_bgconn(fd);
}

/* The channel behind fd, or 0 with errno: ENOTSOCK for a non-socket, and
 * EOPNOTSUPP for an executive socket whose channel this image does not hold. */
static int chan_handle(int fd)
{
    int h = fd_handle(fd);
    if (h != 0 && fd_is_bgconn(fd))
        return h;
    errno = fd_is_bgconn(fd) ? EOPNOTSUPP : ENOTSOCK;
    return 0;
}

static int fd_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && (fl & O_NONBLOCK);
}

/* Wait-free readiness test, for MSG_DONTWAIT and a non-blocking accept. */
static int fd_ready(int fd, short ev)
{
    struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
    return poll(&p, 1, 0) > 0;
}

/* Give a freshly made channel its fd: materialize, apply SOCK_* flags, record. */
static int adopt_handle(int h, int flags)
{
    int fd = ovmx_materialize_fd(h);
    int e;

    if (fd < 0) {
        e = errno;
        ovmx_socket_close(h);
        errno = e;
        return -1;
    }
    if (fd >= DECC_SOCK_MAXFD) {
        close(fd);
        ovmx_socket_close(h);
        errno = EMFILE;
        return -1;
    }
    if (flags & SOCK_CLOEXEC)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flags & SOCK_NONBLOCK)
        (void)fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fd_set_handle(fd, h);
    return fd;
}

/* ======================================================================
 * The socket entry points DECC$SHR exports (socket, decc$socket, ...).
 * ====================================================================== */

int ovmx_decc_socket(int domain, int type, int protocol)
{
    int flags = type & (SOCK_NONBLOCK | SOCK_CLOEXEC);
    int h;

    h = ovmx_socket(domain, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), protocol); /* NEGCTL decc-socket-bound-to-musl */
    if (h < 0)
        return -1;          /* ENODEV with no executive; never a host socket */
    return adopt_handle(h, flags);
}

int ovmx_decc_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    int h = chan_handle(fd);
    return h ? ovmx_connect(h, addr, len) : -1;
}

int ovmx_decc_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    int h = chan_handle(fd);
    return h ? ovmx_bind(h, addr, len) : -1;
}

int ovmx_decc_listen(int fd, int backlog)
{
    int h = chan_handle(fd);
    return h ? ovmx_listen(h, backlog) : -1;
}

int ovmx_decc_accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags)
{
    int h = chan_handle(fd);
    int nh;

    if (!h)
        return -1;
    if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) { errno = EINVAL; return -1; }
    /* A non-blocking listener must not sit in a blocking IO$_ACCESS accept. */
    if (fd_nonblocking(fd) && !fd_ready(fd, POLLIN)) { errno = EAGAIN; return -1; }
    nh = ovmx_accept(h, addr, len);
    if (nh < 0)
        return -1;
    return adopt_handle(nh, flags);
}

int ovmx_decc_accept(int fd, struct sockaddr *addr, socklen_t *len)
{
    return ovmx_decc_accept4(fd, addr, len, 0);
}

int ovmx_decc_shutdown(int fd, int how)
{
    int h = chan_handle(fd);
    if (!h)
        return -1;
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) { errno = EINVAL; return -1; }
    /* IO$_DEACCESS shuts the connection in both directions (the BGn: driver has
     * no half-close yet): SHUT_RD/SHUT_WR therefore over-shut -- recorded as a
     * gap on sockets-api-gaps, not hidden. */
    return ovmx_shutdown(h, how);
}

int ovmx_decc_close(int fd)
{
    int h = fd_handle(fd);

    if (h != 0) {
        fd_set_handle(fd, 0);
        if (fd_is_bgconn(fd))
            (void)ovmx_socket_close(h);   /* $DASSGN this image's channel */
    }
    return close(fd);
}

/* ---- data: the fd's own read/write, which vms.ko routes to the socket ---- */

#define DECC_SEND_FLAGS_OK (MSG_DONTWAIT | MSG_NOSIGNAL | MSG_EOR | MSG_MORE)
#define DECC_RECV_FLAGS_OK (MSG_DONTWAIT | MSG_WAITALL | MSG_NOSIGNAL | MSG_CMSG_CLOEXEC)

/* write the whole buffer (a blocking stream send does), or what fits without
 * blocking when non-blocking. MSG_NOSIGNAL: SIGPIPE is held and any one this
 * write raised is consumed, so a broken connection reports EPIPE only. */
static ssize_t send_fd(int fd, const void *buf, size_t len, int flags)
{
    const char *p = buf;
    size_t off = 0;
    int nb = (flags & MSG_DONTWAIT) || fd_nonblocking(fd);
    sigset_t pipe_set, old;
    ssize_t rc = 0;

    if (flags & ~DECC_SEND_FLAGS_OK) { errno = EOPNOTSUPP; return -1; }
    if ((flags & MSG_DONTWAIT) && !fd_ready(fd, POLLOUT)) { errno = EAGAIN; return -1; }
    if (flags & MSG_NOSIGNAL) {
        sigemptyset(&pipe_set);
        sigaddset(&pipe_set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe_set, &old);
    }
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (off == 0)
                rc = -1;
            break;
        }
        off += (size_t)n;
        if (nb)
            break;
    }
    if (flags & MSG_NOSIGNAL) {
        int e = errno;
        if (rc < 0 && e == EPIPE) {
            struct timespec zero = { 0, 0 };
            (void)sigtimedwait(&pipe_set, NULL, &zero);
        }
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        errno = e;
    }
    return rc < 0 ? -1 : (ssize_t)off;
}

static ssize_t recv_fd(int fd, void *buf, size_t len, int flags)
{
    char *p = buf;
    size_t off = 0;

    if (flags & ~DECC_RECV_FLAGS_OK) { errno = EOPNOTSUPP; return -1; }   /* MSG_PEEK, MSG_OOB */
    if ((flags & MSG_DONTWAIT) && !fd_ready(fd, POLLIN)) { errno = EAGAIN; return -1; }
    for (;;) {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return off ? (ssize_t)off : -1;
        }
        off += (size_t)n;
        if (n == 0 || !(flags & MSG_WAITALL) || off >= len)
            return (ssize_t)off;
    }
}

ssize_t ovmx_decc_send(int fd, const void *buf, size_t len, int flags)
{
    if (!is_sock(fd)) { errno = ENOTSOCK; return -1; }
    return send_fd(fd, buf, len, flags);
}

ssize_t ovmx_decc_recv(int fd, void *buf, size_t len, int flags)
{
    if (!is_sock(fd)) { errno = ENOTSOCK; return -1; }
    return recv_fd(fd, buf, len, flags);
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

static void peer_into(int fd, struct sockaddr *from, socklen_t *fromlen)
{
    if (from != NULL && fromlen != NULL &&
        ovmx_fd_getname(fd, 1, from, fromlen) != 0)
        *fromlen = 0;
}

ssize_t ovmx_decc_recvfrom(int fd, void *buf, size_t len, int flags,
                           struct sockaddr *from, socklen_t *fromlen)
{
    ssize_t n = ovmx_decc_recv(fd, buf, len, flags);
    if (n >= 0)
        peer_into(fd, from, fromlen);
    return n;
}

/* msg_iovlen / msg_controllen are read through their low 32 bits: correct for
 * both the musl (int + pad) and glibc (size_t) struct msghdr on LP64. */
ssize_t ovmx_decc_sendmsg(int fd, const struct msghdr *m, int flags)
{
    int i, iovlen;
    ssize_t total = 0;

    if (!is_sock(fd)) { errno = ENOTSOCK; return -1; }
    if (m == NULL) { errno = EFAULT; return -1; }
    if ((socklen_t)m->msg_controllen != 0) { errno = EOPNOTSUPP; return -1; }  /* no SCM_RIGHTS over BGn: */
    iovlen = (int)m->msg_iovlen;
    for (i = 0; i < iovlen; i++) {
        ssize_t n;
        if (m->msg_iov[i].iov_len == 0)
            continue;
        n = send_fd(fd, m->msg_iov[i].iov_base, m->msg_iov[i].iov_len, flags);
        if (n < 0)
            return total ? total : -1;
        total += n;
        if ((size_t)n < m->msg_iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t ovmx_decc_recvmsg(int fd, struct msghdr *m, int flags)
{
    ssize_t n;

    if (!is_sock(fd)) { errno = ENOTSOCK; return -1; }
    if (m == NULL) { errno = EFAULT; return -1; }
    if (flags & ~(MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) { errno = EOPNOTSUPP; return -1; }
    if ((flags & MSG_DONTWAIT) && !fd_ready(fd, POLLIN)) { errno = EAGAIN; return -1; }
    n = readv(fd, m->msg_iov, (int)m->msg_iovlen);
    if (n >= 0) {
        m->msg_flags = 0;
        if (m->msg_name != NULL) {
            socklen_t nl = m->msg_namelen;
            peer_into(fd, (struct sockaddr *)m->msg_name, &nl);
            m->msg_namelen = nl;
        }
        if (m->msg_control != NULL)
            m->msg_controllen = 0;
    }
    return n;
}

/* ---- names and options: asked of the executive through the fd ------------ */

static int fd_name(int fd, int peer, struct sockaddr *addr, socklen_t *len)
{
    int r = ovmx_fd_getname(fd, peer, addr, len);
    if (r == 1) { errno = ENOTSOCK; return -1; }
    return r;
}

int ovmx_decc_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    return fd_name(fd, 0, addr, len);
}

int ovmx_decc_getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    return fd_name(fd, 1, addr, len);
}

int ovmx_decc_setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    int r;
    if (!is_sock(fd)) { errno = ENOTSOCK; return -1; }
    if (val == NULL || len != (socklen_t)sizeof(int)) {
        /* Only integer options cross the BGn: option request (SO_LINGER,
         * SO_RCVTIMEO ... are structs): refused, never silently swallowed. */
        errno = ENOPROTOOPT;
        return -1;
    }
    r = ovmx_fd_setsockopt(fd, level, name, val, len);
    if (r == 1) { errno = ENOTSOCK; return -1; }
    return r;
}

int ovmx_decc_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    int r;
    if (!is_sock(fd)) { errno = ENOTSOCK; return -1; }
    r = ovmx_fd_getsockopt(fd, level, name, val, len);
    if (r == 1) { errno = ENOTSOCK; return -1; }
    return r;
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
