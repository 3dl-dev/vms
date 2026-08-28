/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bgsock.c - OVMX BSD-sockets RTL veneer over BGn: (DECC$SOCKET-equivalent).
 * See vms_bgsock.h for the layering, the full-duplex blocking model, and the
 * Rule 9 / INV-6 honest-failure contract.
 *
 * PROCESS-WIDE CHANNELS (why this speaks vms_kif_bg_* directly). A socket is a
 * PROCESS resource: on VMS a channel assigned by one thread is usable by every
 * thread of the process, and a full-duplex app (SSH) reads on one thread while
 * it writes on another. The public sys$assign/sys$qiow path stores its channel
 * table in the Per-Process Control Block, whose pointer is currently __thread
 * (src/vmsprocess/vms_pcb.c) -- so a channel assigned on one thread is invisible
 * to another, and a reader thread's sys$qiow(recv) cannot find the socket the
 * main thread opened. The EXECUTIVE channel, by contrast, is genuinely
 * process-wide: vms.ko keys the BG channel list on current->tgid (shared by all
 * threads). So this veneer addresses the executive BGn: driver at the kif layer
 * by its executive channel -- the same IO$_SETMODE/IO$_ACCESS/IO$_WRITEVBLK/
 * IO$_READVBLK/IO$_DEACCESS ops, still the executive-resident socket, no
 * userspace socket stack -- which is what makes concurrent multi-thread r/w on
 * one connection work. (Making the userspace PCB's channels process-wide is the
 * VMS-faithful long-term fix and is filed as a finding; it is out of scope for
 * this increment and has a broad blast radius across every sys$ service.)
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* OVMX kernel-interface + status codes (freestanding libvmssys). vms_kif_bg_*
 * are the direct IO$_* ops against the executive-resident BGn: driver; no
 * userspace socket stack. */
#include "vms_kif.h"
#include "ssdef.h"

#include "vms_bgsock.h"

/* The BGn: driver caps one $QIO transfer at 4096 bytes (VMS_BG_IOCTL_MAXLEN, an
 * OVMX design cap; see src/kernel/vms_bg.h). Repeated here so this TU depends on
 * no kernel header. */
#define OVMX_BG_XFER_MAX 4096u

/* ---- veneer socket table ---------------------------------------------------
 * Maps an OVMX socket handle to its EXECUTIVE channel. A blocking send and a
 * blocking recv on the SAME handle run concurrently from two threads: they read
 * only the immutable exec_chan here and then each block in its own $QIO. The
 * table lock guards only alloc/find/free. */
struct bg_sock {
    int      in_use;
    uint32_t exec_chan;         /* executive BG channel (process-wide, by tgid) */
    int      connected;         /* IO$_ACCESS succeeded */
    int      nonblock;          /* O_NONBLOCK set via ovmx_fcntl */
    int      pollfd;            /* cached readiness fd for non-blocking gating, -1 */
};

#define BG_SOCK_MAX 64
static struct bg_sock g_socks[BG_SOCK_MAX];
static pthread_mutex_t g_socks_lock = PTHREAD_MUTEX_INITIALIZER;

static int idx_of(int s)
{
    int i = s - OVMX_BGSOCK_BASE;
    return (i >= 0 && i < BG_SOCK_MAX) ? i : -1;
}

static struct bg_sock *sock_get(int s)
{
    int i = idx_of(s);
    struct bg_sock *p = NULL;
    if (i < 0)
        return NULL;
    pthread_mutex_lock(&g_socks_lock);
    if (g_socks[i].in_use)
        p = &g_socks[i];
    pthread_mutex_unlock(&g_socks_lock);
    return p;
}

static int sock_alloc(uint32_t exec_chan)
{
    int i, handle = -1;
    pthread_mutex_lock(&g_socks_lock);
    for (i = 0; i < BG_SOCK_MAX; i++) {
        if (!g_socks[i].in_use) {
            g_socks[i].in_use    = 1;
            g_socks[i].exec_chan = exec_chan;
            g_socks[i].connected = 0;
            g_socks[i].nonblock  = 0;
            g_socks[i].pollfd    = -1;
            handle = OVMX_BGSOCK_BASE + i;
            break;
        }
    }
    pthread_mutex_unlock(&g_socks_lock);
    return handle;
}

static void sock_release(int s)
{
    int i = idx_of(s);
    if (i < 0)
        return;
    pthread_mutex_lock(&g_socks_lock);
    memset(&g_socks[i], 0, sizeof(g_socks[i]));
    pthread_mutex_unlock(&g_socks_lock);
}

static int bg_status_to_errno(uint32_t st)
{
    if (st == SS$_NOSUCHDEV)
        return ENODEV;                  /* no executive INET device (Rule 9) */
    return ECONNREFUSED;
}

/* Lazily create and cache the channel's readiness fd (executive-resident poll
 * fd, IO$_SETMODE readiness), used to gate non-blocking send/recv without ever
 * issuing a would-block $QIO. Returns the fd (>= 0) or -1. Concurrency-safe: a
 * racing second creator closes its duplicate. */
static int bg_get_pollfd(struct bg_sock *p)
{
    int fd = -1;
    uint32_t st;

    pthread_mutex_lock(&g_socks_lock);
    fd = p->pollfd;
    pthread_mutex_unlock(&g_socks_lock);
    if (fd >= 0)
        return fd;

    st = vms_kif_bg_pollfd(p->exec_chan, &fd);
    if (!(st & 1) || fd < 0)
        return -1;

    pthread_mutex_lock(&g_socks_lock);
    if (p->pollfd < 0) {
        p->pollfd = fd;
    } else {
        int dup = fd;
        fd = p->pollfd;                 /* someone raced us in; use theirs */
        pthread_mutex_unlock(&g_socks_lock);
        close(dup);
        return fd;
    }
    pthread_mutex_unlock(&g_socks_lock);
    return fd;
}

/* For a non-blocking socket, return 1 if the channel is ready for the wanted
 * direction (POLLIN for recv / POLLOUT for send), 0 if it would block (caller
 * sets EAGAIN), -1 on a poll error. Data still moves only through $QIO; this is
 * only the readiness gate. */
static int bg_nonblock_ready(struct bg_sock *p, short want)
{
    struct pollfd pfd;
    int fd = bg_get_pollfd(p);
    int rc;

    if (fd < 0)
        return 1;                       /* no readiness fd: fall back to blocking op */
    pfd.fd = fd;
    pfd.events = want;
    pfd.revents = 0;
    rc = poll(&pfd, 1, 0);
    if (rc < 0)
        return (errno == EINTR) ? 0 : -1;
    if (rc == 0)
        return 0;                       /* not ready -- would block */
    return (pfd.revents & (want | POLLERR | POLLHUP)) ? 1 : 0;
}

int ovmx_socket(int domain, int type, int protocol)
{
    uint32_t exec_chan = 0, unit = 0;
    uint32_t st;
    int handle;

    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM) { errno = EPROTOTYPE; return -1; }
    if (protocol != 0 && protocol != IPPROTO_TCP) {
        errno = EPROTONOSUPPORT; return -1;
    }

    /* $ASSIGN TCPIP$DEVICE: -- SS$_NOSUCHDEV means no /dev/vms; fail honestly,
     * never a raw socket() fallback (INV-6). */
    st = vms_kif_bg_create(&exec_chan, &unit, NULL, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }

    /* IO$_SETMODE creates the executive-resident socket. */
    st = vms_kif_bg_setmode(exec_chan);
    if (!(st & 1)) { errno = bg_status_to_errno(st); vms_kif_bg_dassgn(exec_chan); return -1; }

    handle = sock_alloc(exec_chan);
    if (handle < 0) { vms_kif_bg_dassgn(exec_chan); errno = EMFILE; return -1; }
    return handle;
}

int ovmx_connect(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    struct bg_sock *p = sock_get(s);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    if (p->connected) { errno = EISCONN; return -1; }
    if (addr == NULL || (size_t)addrlen < sizeof(*sin) ||
        addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT; return -1;
    }

    /* IO$_ACCESS connects. Family 2 = AF_INET; port + addr are network order,
     * straight from the sockaddr the caller resolved. */
    st = vms_kif_bg_connect(p->exec_chan, 2, sin->sin_port, sin->sin_addr.s_addr);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }

    p->connected = 1;
    return 0;
}

ssize_t ovmx_send(int s, const void *buf, size_t len, int flags)
{
    struct bg_sock *p = sock_get(s);
    const unsigned char *b = buf;
    size_t off = 0;
    (void)flags;

    if (p == NULL) { errno = EBADF; return -1; }
    if (buf == NULL) { errno = EFAULT; return -1; }

    if (p->nonblock) {
        int r = bg_nonblock_ready(p, POLLOUT);
        if (r < 0) { errno = EIO; return -1; }
        if (r == 0) { errno = EAGAIN; return -1; }
    }

    /* Send the whole buffer, chunked to the driver's per-QIO cap. Each chunk is
     * one BLOCKING IO$_WRITEVBLK -- safe to run while another thread blocks in
     * ovmx_recv on this same handle (the executive channel is full-duplex). */
    while (off < len) {
        uint32_t actlen = 0;
        uint32_t chunk = (uint32_t)(len - off);
        uint32_t st;
        if (chunk > OVMX_BG_XFER_MAX)
            chunk = OVMX_BG_XFER_MAX;
        st = vms_kif_bg_send(p->exec_chan, b + off, chunk, &actlen);
        if (!(st & 1)) { errno = bg_status_to_errno(st); return (off ? (ssize_t)off : -1); }
        if (actlen == 0)
            break;                      /* wrote nothing -- treat as done */
        off += actlen;
    }
    return (ssize_t)off;
}

ssize_t ovmx_recv(int s, void *buf, size_t len, int flags)
{
    struct bg_sock *p = sock_get(s);
    uint32_t bufsz, got = 0, st;
    ssize_t n;
    (void)flags;

    if (p == NULL) { errno = EBADF; return -1; }
    if (buf == NULL) { errno = EFAULT; return -1; }

    if (p->nonblock) {
        int r = bg_nonblock_ready(p, POLLIN);
        if (r < 0) { errno = EIO; return -1; }
        if (r == 0) { errno = EAGAIN; return -1; }
    }

    bufsz = (len > OVMX_BG_XFER_MAX) ? OVMX_BG_XFER_MAX : (uint32_t)len;

    /* One BLOCKING IO$_READVBLK. got==0 with success = orderly peer close (EOF). */
    st = vms_kif_bg_recv(p->exec_chan, buf, bufsz, &got);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }
    n = got;                            /* NEGCTL bgsock-recv-length-zeroed */
    return n;
}

/* SERVER path (vms-698): bind/listen/accept over the BGn: executive path
 * (vms_kif_bg_bind/listen/accept). No userspace fake -- with no /dev/vms the kif
 * returns SS$_NOSUCHDEV, rendered as an errno (Rule 9 / INV-6). */
int ovmx_bind(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    struct bg_sock *p = sock_get(s);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    if (addr == NULL || (size_t)addrlen < sizeof(*sin) ||
        addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT; return -1;
    }
    /* Bind the local address; the effective port (if ephemeral) is read later via
     * ovmx_getsockname, exactly as BSD sockets do. */
    st = vms_kif_bg_bind(p->exec_chan, 2, sin->sin_port, sin->sin_addr.s_addr,
                         NULL, NULL);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }
    return 0;
}
int ovmx_listen(int s, int backlog)
{
    struct bg_sock *p = sock_get(s);
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    st = vms_kif_bg_listen(p->exec_chan, backlog);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }
    return 0;
}
int ovmx_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    struct bg_sock *lp = sock_get(s);
    struct bg_sock *ap;
    uint32_t acc_chan = 0, unit = 0, st;
    uint16_t fam = 0, port = 0;
    uint32_t a4 = 0;
    int handle;

    if (lp == NULL) { errno = EBADF; return -1; }

    /* $ASSIGN a fresh, EMPTY BG channel to receive the accepted connection --
     * NO IO$_SETMODE: accept installs the accepted socket onto it (the executive
     * analogue of accept() minting a new fd). */
    st = vms_kif_bg_create(&acc_chan, &unit, NULL, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }

    st = vms_kif_bg_accept(lp->exec_chan, acc_chan, &fam, &port, &a4);
    if (!(st & 1)) {
        errno = bg_status_to_errno(st);
        vms_kif_bg_dassgn(acc_chan);
        return -1;
    }

    handle = sock_alloc(acc_chan);
    if (handle < 0) { vms_kif_bg_dassgn(acc_chan); errno = EMFILE; return -1; }
    ap = sock_get(handle);
    if (ap) ap->connected = 1;      /* the accepted socket is connected */

    /* Report the peer address (network byte order, as accept() does). */
    if (addr != NULL && addrlen != NULL &&
        (size_t)*addrlen >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *sin = (struct sockaddr_in *)addr;
        sin->sin_family = AF_INET;
        sin->sin_port = port;
        sin->sin_addr.s_addr = a4;
        *addrlen = sizeof(*sin);
    }
    return handle;
}

int ovmx_shutdown(int s, int how)
{
    struct bg_sock *p = sock_get(s);
    (void)how;
    if (p == NULL) { errno = EBADF; return -1; }
    if (p->connected)
        vms_kif_bg_deaccess(p->exec_chan);
    return 0;
}

int ovmx_socket_close(int s)
{
    struct bg_sock *p = sock_get(s);
    int cached_fd;
    if (p == NULL) { errno = EBADF; return -1; }
    cached_fd = p->pollfd;
    if (p->connected)
        vms_kif_bg_deaccess(p->exec_chan);
    vms_kif_bg_dassgn(p->exec_chan);
    sock_release(s);
    if (cached_fd >= 0)
        close(cached_fd);           /* readiness fd cached for non-blocking gating */
    return 0;
}

int ovmx_pollfd(int s)
{
    struct bg_sock *p = sock_get(s);
    int fd = -1;
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    /* Ask the executive for a real readiness-only pollable fd on the channel's
     * socket. The fd is poll()/select()-able and reflects the socket's true
     * readiness; data still moves via ovmx_send()/ovmx_recv(). */
    st = vms_kif_bg_pollfd(p->exec_chan, &fd);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }
    return fd;
}

/* Return the channel's CACHED executive readiness fd (creating it once), for a
 * poll()/select() layer that must fold veneer handles into a real fd set (the
 * --wrap dispatch, vms-4bf). Unlike ovmx_pollfd() -- which mints a FRESH fd the
 * caller must close -- this fd is owned by the veneer socket and closed at
 * ovmx_socket_close(). Returns the fd (>= 0) or -1. */
int ovmx_readyfd(int s)
{
    struct bg_sock *p = sock_get(s);
    if (p == NULL) { errno = EBADF; return -1; }
    return bg_get_pollfd(p);
}

/* ---- socket-name + option surface (the OpenSSH de-veneer, vms-4bf) ------- */

/* Fill *addr with the channel socket's local (which==0) or peer (which==1)
 * endpoint, read straight from the REAL host kernel socket via the executive.
 * This is the anti-veneer answer: getpeername returns the TRUE remote IP:port,
 * so OpenSSH's known_hosts records the right host -- an AF_UNIX socketpair
 * bridge would return AF_UNIX here instead. */
static int bg_getname(int s, uint32_t which, struct sockaddr *addr,
                      socklen_t *addrlen)
{
    struct bg_sock *p = sock_get(s);
    struct sockaddr_in sin;
    uint16_t family = 0, port = 0;
    uint32_t inaddr = 0, st;

    if (p == NULL) { errno = EBADF; return -1; }
    if (addr == NULL || addrlen == NULL) { errno = EFAULT; return -1; }

    st = vms_kif_bg_getname(p->exec_chan, which, &family, &port, &inaddr);
    if (!(st & 1)) {
        errno = (st == SS$_NOSUCHDEV) ? ENODEV : ENOTCONN;
        return -1;
    }

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = family;            /* AF_INET, from the kernel socket */
    sin.sin_port   = port;              /* network byte order, straight through */
    sin.sin_addr.s_addr = inaddr;       /* network byte order */

    {
        socklen_t n = *addrlen;
        if (n > (socklen_t)sizeof(sin))
            n = (socklen_t)sizeof(sin);
        memcpy(addr, &sin, n);
        *addrlen = (socklen_t)sizeof(sin);  /* full size, as getpeername reports */
    }
    return 0;
}

int ovmx_getpeername(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    return bg_getname(s, 1, addr, addrlen);
}

int ovmx_getsockname(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    return bg_getname(s, 0, addr, addrlen);
}

int ovmx_setsockopt(int s, int level, int optname, const void *optval,
                    socklen_t optlen)
{
    struct bg_sock *p = sock_get(s);
    int val;
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    if (optval == NULL || optlen < (socklen_t)sizeof(int)) {
        errno = EINVAL; return -1;
    }
    val = *(const int *)optval;

    st = vms_kif_bg_setsockopt(p->exec_chan, level, optname, val);
    if (!(st & 1)) {
        /* SS$_BADPARAM = the executive did not honor this option; report the
         * standard "option not supported" so the caller treats it as it would a
         * real socket that rejected the option -- honest, not swallowed. */
        errno = (st == SS$_NOSUCHDEV) ? ENODEV : ENOPROTOOPT;
        return -1;
    }
    return 0;
}

int ovmx_getsockopt(int s, int level, int optname, void *optval,
                    socklen_t *optlen)
{
    struct bg_sock *p = sock_get(s);
    int val = 0;
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    if (optval == NULL || optlen == NULL || *optlen < (socklen_t)sizeof(int)) {
        errno = EINVAL; return -1;
    }

    st = vms_kif_bg_getsockopt(p->exec_chan, level, optname, &val);
    if (!(st & 1)) {
        errno = (st == SS$_NOSUCHDEV) ? ENODEV : ENOPROTOOPT;
        return -1;
    }
    *(int *)optval = val;               /* the socket's REAL current value */
    *optlen = (socklen_t)sizeof(int);
    return 0;
}

/* Minimal fcntl: F_GETFL / F_SETFL for O_NONBLOCK only (what OpenSSH sets on the
 * connection fd). Non-blocking send/recv are gated on the executive readiness
 * fd (bg_nonblock_ready) so a would-block never issues a blocking $QIO; data
 * still moves only through IO$_READVBLK / IO$_WRITEVBLK. Other fcntl commands
 * are a no-op success (F_SETFD/FD_CLOEXEC has no meaning for a veneer handle). */
int ovmx_fcntl(int s, int cmd, int arg)
{
    struct bg_sock *p = sock_get(s);
    if (p == NULL) { errno = EBADF; return -1; }
    switch (cmd) {
    case F_GETFL:
        return p->nonblock ? O_NONBLOCK : 0;
    case F_SETFL:
        p->nonblock = (arg & O_NONBLOCK) ? 1 : 0;
        return 0;
    default:
        return 0;
    }
}

/* ---- numeric IPv4 resolution (no DNS yet) ------------------------------- */

int ovmx_inet_pton(int af, const char *src, void *dst)
{
    unsigned char oct[4];
    const char *p = src;
    int i;

    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (src == NULL || dst == NULL) return 0;

    for (i = 0; i < 4; i++) {
        unsigned int n = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10u + (unsigned int)(*p - '0');
            if (n > 255u) return 0;
            p++; digits++;
        }
        if (!digits) return 0;
        oct[i] = (unsigned char)n;
        if (i < 3) {
            if (*p != '.') return 0;
            p++;
        }
    }
    if (*p != '\0') return 0;
    memcpy(dst, oct, 4);                 /* network order (big-endian bytes) */
    return 1;
}

int ovmx_getaddrinfo_numeric(const char *host, uint16_t port,
                             struct sockaddr_in *out)
{
    struct in_addr a;
    if (host == NULL || out == NULL) return -1;
    if (ovmx_inet_pton(AF_INET, host, &a) != 1)
        return -1;                      /* not a dotted-quad literal (no DNS) */
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port   = htons(port);
    out->sin_addr   = a;
    return 0;
}
