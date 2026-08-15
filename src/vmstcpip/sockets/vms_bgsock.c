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
#include <sys/socket.h>
#include <netinet/in.h>

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

    bufsz = (len > OVMX_BG_XFER_MAX) ? OVMX_BG_XFER_MAX : (uint32_t)len;

    /* One BLOCKING IO$_READVBLK. got==0 with success = orderly peer close (EOF). */
    st = vms_kif_bg_recv(p->exec_chan, buf, bufsz, &got);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }
    n = got;                            /* NEGCTL bgsock-recv-length-zeroed */
    return n;
}

/* SERVER path: needs the BGn: bind/listen/accept executive path (vms-698).
 * Honest ENOSYS until then -- never a userspace fake (Rule 9 / INV-6). */
int ovmx_bind(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)s; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
}
int ovmx_listen(int s, int backlog)
{
    (void)s; (void)backlog; errno = ENOSYS; return -1;
}
int ovmx_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)s; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
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
    if (p == NULL) { errno = EBADF; return -1; }
    if (p->connected)
        vms_kif_bg_deaccess(p->exec_chan);
    vms_kif_bg_dassgn(p->exec_chan);
    sock_release(s);
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
