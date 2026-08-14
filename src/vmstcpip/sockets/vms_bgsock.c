/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_bgsock.c - OVMX BSD-sockets RTL veneer over BGn: (DECC$SOCKET-equivalent).
 * See vms_bgsock.h for the layering, the pollable-fd model, and the Rule 9 /
 * INV-6 honest-failure contract.
 *
 * The connect/read/write framing mirrors the already-proven
 * src/vmstcpip/services/tcpip_client.h (vms-527 / vms-dbb): $ASSIGN a channel to
 * TCPIP$DEVICE:, IO$_SETMODE to create the executive socket, IO$_ACCESS to
 * connect, then IO$_WRITEVBLK / IO$_READVBLK to move bytes, IO$_DEACCESS +
 * $DASSGN to close. The veneer adds the sockets-API surface + the pollable-fd
 * bridge (a socketpair the pump threads drive) so the application uses ordinary
 * socket()/connect()/read()/write()/poll().
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* OVMX system-service surface (src/libvms/include) -- the same headers
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
 * Maps an app-visible fd (the socketpair end handed out) to its BGn: channel +
 * pump state. Small fixed table under a mutex -- a client process holds a
 * handful of sockets. */
struct bg_sock {
    int             in_use;
    int             app_fd;     /* fd handed to the app (socketpair[0]) */
    int             int_fd;     /* internal end the pumps drive (socketpair[1]) */
    uint16_t        chan;       /* $ASSIGN channel */
    int             connected;  /* IO$_ACCESS succeeded, pumps running */
    pthread_t       t_out, t_in;
    pthread_mutex_t lock;       /* guards refs/closed for the two pumps */
    int             refs;       /* live pump threads */
    int             closed;     /* channel already released */
};

#define BG_SOCK_MAX 64
static struct bg_sock g_socks[BG_SOCK_MAX];
static pthread_mutex_t g_socks_lock = PTHREAD_MUTEX_INITIALIZER;

static struct bg_sock *sock_find(int app_fd)
{
    struct bg_sock *s = NULL;
    int i;
    pthread_mutex_lock(&g_socks_lock);
    for (i = 0; i < BG_SOCK_MAX; i++) {
        if (g_socks[i].in_use && g_socks[i].app_fd == app_fd) {
            s = &g_socks[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_socks_lock);
    return s;
}

static struct bg_sock *sock_alloc(void)
{
    struct bg_sock *s = NULL;
    int i;
    pthread_mutex_lock(&g_socks_lock);
    for (i = 0; i < BG_SOCK_MAX; i++) {
        if (!g_socks[i].in_use) {
            s = &g_socks[i];
            memset(s, 0, sizeof(*s));
            s->in_use = 1;
            pthread_mutex_init(&s->lock, NULL);
            break;
        }
    }
    pthread_mutex_unlock(&g_socks_lock);
    return s;
}

static void sock_free(struct bg_sock *s)
{
    pthread_mutex_lock(&g_socks_lock);
    pthread_mutex_destroy(&s->lock);
    memset(s, 0, sizeof(*s));   /* clears in_use */
    pthread_mutex_unlock(&g_socks_lock);
}

/* Release one pump ref; the last one out IO$_DEACCESS + $DASSGNs the channel. */
static void pump_release(struct bg_sock *s)
{
    int last;
    pthread_mutex_lock(&s->lock);
    last = (--s->refs == 0);
    if (last && !s->closed) {
        struct _iosb iosb;
        sys$qiow(0, s->chan, IO$_DEACCESS, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
        sys$dassgn(s->chan);
        s->closed = 1;
    }
    pthread_mutex_unlock(&s->lock);
}

/* app -> wire: read from the socketpair, IO$_WRITEVBLK to the BGn: channel. */
static void *pump_out(void *arg)
{
    struct bg_sock *s = arg;
    unsigned char buf[OVMX_BG_XFER_MAX];
    for (;;) {
        ssize_t n = read(s->int_fd, buf, sizeof(buf));
        uint32_t off = 0;
        if (n <= 0)
            break;
        while (off < (uint32_t)n) {
            struct _iosb iosb;
            uint32_t chunk = (uint32_t)n - off;
            uint32_t st;
            if (chunk > OVMX_BG_XFER_MAX)
                chunk = OVMX_BG_XFER_MAX;
            st = sys$qiow(0, s->chan, IO$_WRITEVBLK, &iosb, 0, 0,
                          buf + off, chunk, 0, 0, 0, 0);
            if (!(st & 1) || iosb.iosb$w_bcnt == 0)
                goto done;
            off += iosb.iosb$w_bcnt;
        }
    }
done:
    shutdown(s->int_fd, SHUT_RDWR);
    pump_release(s);
    return NULL;
}

/* wire -> app: IO$_READVBLK off the channel, write into the socketpair. */
static void *pump_in(void *arg)
{
    struct bg_sock *s = arg;
    unsigned char buf[OVMX_BG_XFER_MAX];
    for (;;) {
        struct _iosb iosb;
        uint32_t st = sys$qiow(0, s->chan, IO$_READVBLK, &iosb, 0, 0,
                               buf, sizeof(buf), 0, 0, 0, 0);
        uint32_t got, off = 0;
        if (!(st & 1))
            break;
        got = iosb.iosb$w_bcnt;         /* NEGCTL bgsock-recv-length-zeroed */
        if (got == 0)
            break;                      /* EOF */
        while (off < got) {
            ssize_t w = write(s->int_fd, buf + off, got - off);
            if (w <= 0)
                goto done;
            off += (uint32_t)w;
        }
    }
done:
    shutdown(s->int_fd, SHUT_RDWR);
    pump_release(s);
    return NULL;
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
    struct bg_sock *s;
    struct _iosb iosb;
    int sv[2] = { -1, -1 };
    uint16_t chan = 0;
    uint32_t st;

    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM) { errno = EPROTOTYPE; return -1; }
    if (protocol != 0 && protocol != IPPROTO_TCP) {
        errno = EPROTONOSUPPORT; return -1;
    }

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;

    /* $ASSIGN TCPIP$DEVICE: -- SS$_NOSUCHDEV here means no /dev/vms; fail
     * honestly, never a raw socket() fallback (INV-6). */
    st = sys$assign(&dev, &chan, 0, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }

    /* IO$_SETMODE creates the executive-resident socket. */
    st = sys$qiow(0, chan, IO$_SETMODE, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); sys$dassgn(chan); return -1; }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        int e = errno;
        sys$dassgn(chan);
        errno = e;
        return -1;
    }

    s = sock_alloc();
    if (s == NULL) {
        close(sv[0]); close(sv[1]);
        sys$dassgn(chan);
        errno = EMFILE;
        return -1;
    }
    s->app_fd    = sv[0];
    s->int_fd    = sv[1];
    s->chan      = chan;
    s->connected = 0;
    return sv[0];
}

int ovmx_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    struct bg_sock *s = sock_find(fd);
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    struct bg_sockaddr_in bsa;
    struct _iosb iosb;
    uint32_t st;

    if (s == NULL) { errno = EBADF; return -1; }
    if (s->connected) { errno = EISCONN; return -1; }
    if (addr == NULL || (size_t)addrlen < sizeof(*sin) ||
        addr->sa_family != AF_INET) {
        errno = EAFNOSUPPORT; return -1;
    }

    /* IO$_ACCESS connects (network-order port + addr, straight from sockaddr). */
    bsa.family = 2;                     /* AF_INET as the driver expects */
    bsa.port   = sin->sin_port;         /* already network order */
    bsa.addr   = sin->sin_addr.s_addr;  /* already network order */
    st = sys$qiow(0, s->chan, IO$_ACCESS, &iosb, 0, 0,
                  &bsa, (uint32_t)sizeof(bsa), 0, 0, 0, 0);
    if (!(st & 1)) { errno = bg_status_to_errno(st); return -1; }

    /* Start the pumps: refs=2, one per direction; last one out releases. */
    pthread_mutex_lock(&s->lock);
    s->refs = 1;
    pthread_mutex_unlock(&s->lock);
    if (pthread_create(&s->t_out, NULL, pump_out, s) != 0) {
        pthread_mutex_lock(&s->lock); s->refs = 0; pthread_mutex_unlock(&s->lock);
        errno = EIO; return -1;
    }
    pthread_mutex_lock(&s->lock);
    s->refs = 2;
    pthread_mutex_unlock(&s->lock);
    if (pthread_create(&s->t_in, NULL, pump_in, s) != 0) {
        /* t_out owns the channel now; drop our ref + shut the pipe so it exits. */
        pthread_mutex_lock(&s->lock); s->refs = 1; pthread_mutex_unlock(&s->lock);
        shutdown(s->int_fd, SHUT_RDWR);
        pthread_join(s->t_out, NULL);
        errno = EIO;
        return -1;
    }
    pthread_detach(s->t_out);
    pthread_detach(s->t_in);
    s->connected = 1;
    return 0;
}

/* SERVER path: needs the BGn: bind/listen/accept executive path (vms-698).
 * Honest ENOSYS until then -- never a userspace fake (Rule 9 / INV-6). */
int ovmx_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)fd; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
}
int ovmx_listen(int fd, int backlog)
{
    (void)fd; (void)backlog; errno = ENOSYS; return -1;
}
int ovmx_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)fd; (void)addr; (void)addrlen; errno = ENOSYS; return -1;
}

ssize_t ovmx_send(int fd, const void *buf, size_t len, int flags)
{
    (void)flags;
    return write(fd, buf, len);         /* pump does the IO$_WRITEVBLK */
}
ssize_t ovmx_recv(int fd, void *buf, size_t len, int flags)
{
    (void)flags;
    return read(fd, buf, len);          /* pump does the IO$_READVBLK */
}

int ovmx_shutdown(int fd, int how)
{
    struct bg_sock *s = sock_find(fd);
    if (s == NULL) { errno = EBADF; return -1; }
    /* Half-close the app end toward the pumps; the wire is torn down when the
     * pumps see EOF, which IO$_DEACCESSes the channel. */
    return shutdown(s->app_fd, how);
}

int ovmx_socket_close(int fd)
{
    struct bg_sock *s = sock_find(fd);
    if (s == NULL) {
        /* Not a veneer fd -- behave like close() so callers can use it
         * uniformly. */
        return close(fd);
    }
    /* Closing the app fd makes pump_out's read() return EOF; the pumps then
     * IO$_DEACCESS + $DASSGN the channel (last one out). If never connected,
     * release the channel here. */
    close(s->app_fd);
    pthread_mutex_lock(&s->lock);
    if (!s->connected && !s->closed) {
        struct _iosb iosb;
        sys$qiow(0, s->chan, IO$_DEACCESS, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
        sys$dassgn(s->chan);
        s->closed = 1;
        close(s->int_fd);
    }
    pthread_mutex_unlock(&s->lock);
    sock_free(s);
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
