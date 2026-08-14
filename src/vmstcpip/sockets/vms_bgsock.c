/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bgsock.c - OVMX BSD-sockets RTL veneer over BGn: (DECC$SOCKET-equivalent).
 * See vms_bgsock.h for the layering, the full-duplex blocking model, and the
 * Rule 9 / INV-6 honest-failure contract.
 *
 * Each sockets call becomes ONE public $ASSIGN/$QIO/$DASSGN op, exactly as
 * src/vmstcpip/services/tcpip_client.h and test_syssvc_bg_echo.c do -- there is
 * no userspace socket stack. ovmx_send()/ovmx_recv() each do a single BLOCKING
 * $QIO; the executive supports a blocking read and a blocking write outstanding
 * at once on one channel (no lock is held across the socket op at any layer, and
 * the host kernel socket is full-duplex), so an app can read and write the same
 * connection concurrently from two threads.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* OVMX system-service surface (src/libvms/include) -- the same public services
 * tcpip_client.h speaks; no kernel header, no userspace socket stack. */
#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"

#include "vms_bgsock.h"

/* The BGn: driver caps one $QIO transfer at 4096 bytes (VMS_BG_IOCTL_MAXLEN, an
 * OVMX design cap; see src/kernel/vms_bg.h and TCPIP_XFER_MAX in
 * tcpip_client.h). Repeated here so this TU depends on no kernel header. */
#define OVMX_BG_XFER_MAX 4096u

/* The 8-byte socket address the BGn: IO$_ACCESS handler reads: AF_INET(2),
 * port (network order), IPv4 addr (network order) -- matches the executive's
 * struct bg_sockaddr_in (src/libvms/syssvc/sys_qio.c) and tcpip_client.h. */
struct bg_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
};

/* ---- veneer socket table ---------------------------------------------------
 * Maps an OVMX socket handle to its $ASSIGN channel. A blocking send and a
 * blocking recv on the SAME handle run concurrently from two threads: they read
 * only the immutable `chan` here and then each block in its own $QIO, so no lock
 * is held across the executive op (the table lock guards only alloc/find/free). */
struct bg_sock {
    int      in_use;
    uint16_t chan;              /* $ASSIGN channel (sys$ channel number) */
    int      connected;         /* IO$_ACCESS succeeded */
};

#define BG_SOCK_MAX 64
static struct bg_sock g_socks[BG_SOCK_MAX];
static pthread_mutex_t g_socks_lock = PTHREAD_MUTEX_INITIALIZER;

/* handle <-> table index */
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

/* Allocate a table slot; returns the handle (>= OVMX_BGSOCK_BASE) or -1. */
static int sock_alloc(uint16_t chan)
{
    int i, handle = -1;
    pthread_mutex_lock(&g_socks_lock);
    for (i = 0; i < BG_SOCK_MAX; i++) {
        if (!g_socks[i].in_use) {
            g_socks[i].in_use    = 1;
            g_socks[i].chan      = chan;
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
    static const char devname[] = "TCPIP$DEVICE:";
    struct dsc$descriptor_s dev;
    struct _iosb iosb;
    uint16_t chan = 0;
    uint32_t st;
    int handle;

    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM) { errno = EPROTOTYPE; return -1; }
    if (protocol != 0 && protocol != IPPROTO_TCP) {
        errno = EPROTONOSUPPORT; return -1;
    }

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;

    /* $ASSIGN TCPIP$DEVICE: -- SS$_NOSUCHDEV means no /dev/vms; fail honestly,
     * never a raw socket() fallback (INV-6). */
    st = sys$assign(&dev, &chan, 0, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }

    /* IO$_SETMODE creates the executive-resident socket. */
    st = sys$qiow(0, chan, IO$_SETMODE, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); sys$dassgn(chan); return -1; }

    handle = sock_alloc(chan);
    if (handle < 0) { sys$dassgn(chan); errno = EMFILE; return -1; }
    return handle;
}

int ovmx_connect(int s, const struct sockaddr *addr, socklen_t addrlen)
{
    struct bg_sock *p = sock_get(s);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    struct bg_sockaddr_in bsa;
    struct _iosb iosb;
    uint32_t st;

    if (p == NULL) { errno = EBADF; return -1; }
    if (p->connected) { errno = EISCONN; return -1; }
    if (addr == NULL || (size_t)addrlen < sizeof(*sin) ||
        addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT; return -1;
    }

    bsa.family = 2;                     /* AF_INET as the driver expects */
    bsa.port   = sin->sin_port;         /* already network order */
    bsa.addr   = sin->sin_addr.s_addr;  /* already network order */
    st = sys$qiow(0, p->chan, IO$_ACCESS, &iosb, 0, 0,
                  &bsa, (uint32_t)sizeof(bsa), 0, 0, 0, 0);
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
     * ovmx_recv on this same handle (full-duplex). */
    while (off < len) {
        struct _iosb iosb;
        uint32_t chunk = (uint32_t)(len - off);
        uint32_t st;
        if (chunk > OVMX_BG_XFER_MAX)
            chunk = OVMX_BG_XFER_MAX;
        st = sys$qiow(0, p->chan, IO$_WRITEVBLK, &iosb, 0, 0,
                      (void *)(b + off), chunk, 0, 0, 0, 0);
        if (!(st & 1)) { errno = bg_status_to_errno(st); return (off ? (ssize_t)off : -1); }
        if (iosb.iosb$w_bcnt == 0)
            break;                      /* wrote nothing -- treat as done */
        off += iosb.iosb$w_bcnt;
    }
    return (ssize_t)off;
}

ssize_t ovmx_recv(int s, void *buf, size_t len, int flags)
{
    struct bg_sock *p = sock_get(s);
    struct _iosb iosb;
    uint32_t bufsz, st;
    ssize_t n;
    (void)flags;

    if (p == NULL) { errno = EBADF; return -1; }
    if (buf == NULL) { errno = EFAULT; return -1; }

    bufsz = (len > OVMX_BG_XFER_MAX) ? OVMX_BG_XFER_MAX : (uint32_t)len;

    /* One BLOCKING IO$_READVBLK. Returns 0 on an orderly peer close (EOF). */
    st = sys$qiow(0, p->chan, IO$_READVBLK, &iosb, 0, 0,
                  buf, bufsz, 0, 0, 0, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }
    n = iosb.iosb$w_bcnt;               /* NEGCTL bgsock-recv-length-zeroed */
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
    struct _iosb iosb;
    (void)how;
    if (p == NULL) { errno = EBADF; return -1; }
    if (p->connected)
        sys$qiow(0, p->chan, IO$_DEACCESS, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
    return 0;
}

int ovmx_socket_close(int s)
{
    struct bg_sock *p = sock_get(s);
    struct _iosb iosb;
    if (p == NULL) { errno = EBADF; return -1; }
    if (p->connected)
        sys$qiow(0, p->chan, IO$_DEACCESS, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
    sys$dassgn(p->chan);
    sock_release(s);
    return 0;
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
