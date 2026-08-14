/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_bg_transport.c - OVMX port layer for OpenSSH clients: obtain the ssh
 * transport over the executive-resident BGn: INET device via the public
 * $ASSIGN TCPIP$DEVICE: + $QIO path, NOT a raw Linux socket().  See the header
 * for the full rationale (item vms-22a, docs/design-openssh-port-ovmx.md §2.3).
 *
 * The connect/read/write framing mirrors, byte for byte, the already-proven
 * src/vmstcpip/services/tcpip_client.h (vms-527 / vms-dbb): $ASSIGN a channel to
 * TCPIP$DEVICE:, IO$_SETMODE to create the socket, IO$_ACCESS with the 8-byte
 * AF_INET sockaddr to connect, then IO$_WRITEVBLK / IO$_READVBLK to move bytes,
 * IO$_DEACCESS + $DASSGN to close.  The one thing added on top is the fd bridge:
 * a socketpair gives OpenSSH an ordinary pollable fd, and two pump threads
 * shuttle bytes between that fd and the VMS channel, so the vendored OpenSSH
 * packet/poll loop is untouched.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* OVMX system-service surface (src/libvms/include) -- exactly the headers
 * tcpip_client.h speaks; no kernel header, no userspace socket stack. */
#include "starlet.h"
#include "descrip.h"
#include "iodef.h"
#include "iosbdef.h"
#include "ssdef.h"

#include "ovmx/ovmx_bg_transport.h"

/* The BGn: driver caps one $QIO transfer at 4096 bytes (VMS_BG_IOCTL_MAXLEN, an
 * OVMX design cap; see src/kernel/vms_bg.h and TCPIP_XFER_MAX in
 * tcpip_client.h).  Repeated here so this TU depends on no kernel header. */
#define OVMX_BG_XFER_MAX 4096u

/* The 8-byte socket address the BGn: IO$_ACCESS handler reads: AF_INET(2),
 * port (network order), IPv4 addr (network order) -- matches the executive's
 * struct bg_sockaddr_in (src/libvms/syssvc/sys_qio.c) and tcpip_client.h's
 * on-wire sockaddr. */
struct bg_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
};

/* Shared pump context.  Refcounted so the LAST pump thread to exit performs the
 * single IO$_DEACCESS + $DASSGN on the channel. */
struct bg_pump {
    uint16_t        chan;       /* $ASSIGN channel */
    int             ssh_fd;     /* internal end of the socketpair (peer of the
                                 * fd handed to OpenSSH) */
    pthread_mutex_t lock;
    int             refs;       /* pump threads still alive */
    int             closed;     /* channel already deaccessed */
};

static void bg_pump_release(struct bg_pump *p)
{
    int last;
    pthread_mutex_lock(&p->lock);
    last = (--p->refs == 0);
    if (last && !p->closed) {
        struct _iosb iosb;
        /* Graceful shutdown then release, exactly as tcpip_close() does. */
        sys$qiow(0, p->chan, IO$_DEACCESS, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
        sys$dassgn(p->chan);
        p->closed = 1;
    }
    pthread_mutex_unlock(&p->lock);
    if (last) {
        close(p->ssh_fd);
        pthread_mutex_destroy(&p->lock);
        free(p);
    }
}

/* ssh -> wire: read application bytes off the socketpair, IO$_WRITEVBLK them to
 * the BGn: channel (chunked to the driver's per-QIO cap). */
static void *bg_pump_out(void *arg)
{
    struct bg_pump *p = arg;
    unsigned char buf[OVMX_BG_XFER_MAX];

    for (;;) {
        ssize_t n = read(p->ssh_fd, buf, sizeof(buf));
        uint32_t off = 0;
        if (n <= 0)
            break;                      /* OpenSSH closed its end (EOF) or error */
        while (off < (uint32_t)n) {
            struct _iosb iosb;
            uint32_t chunk = (uint32_t)n - off;
            uint32_t st;
            if (chunk > OVMX_BG_XFER_MAX)
                chunk = OVMX_BG_XFER_MAX;
            st = sys$qiow(0, p->chan, IO$_WRITEVBLK, &iosb, 0, 0,
                          buf + off, chunk, 0, 0, 0, 0);
            if (!(st & 1) || iosb.iosb$w_bcnt == 0)
                goto done;              /* wire write failed -- tear down */
            off += iosb.iosb$w_bcnt;
        }
    }
done:
    /* Half-close toward the peer so the server sees EOF and closes back, which
     * releases the inbound thread's blocking IO$_READVBLK. */
    shutdown(p->ssh_fd, SHUT_RDWR);
    bg_pump_release(p);
    return NULL;
}

/* wire -> ssh: IO$_READVBLK off the BGn: channel, write the bytes into the
 * socketpair so OpenSSH read()s them as if off a TCP socket.  bcnt==0 is an
 * orderly peer close (EOF). */
static void *bg_pump_in(void *arg)
{
    struct bg_pump *p = arg;
    unsigned char buf[OVMX_BG_XFER_MAX];

    for (;;) {
        struct _iosb iosb;
        uint32_t st = sys$qiow(0, p->chan, IO$_READVBLK, &iosb, 0, 0,
                               buf, sizeof(buf), 0, 0, 0, 0);
        uint32_t got, off = 0;
        if (!(st & 1))
            break;                      /* recv error */
        got = iosb.iosb$w_bcnt;
        if (got == 0)
            break;                      /* EOF */
        while (off < got) {
            ssize_t w = write(p->ssh_fd, buf + off, got - off);
            if (w <= 0)
                goto done;              /* OpenSSH gone */
            off += (uint32_t)w;
        }
    }
done:
    shutdown(p->ssh_fd, SHUT_RDWR);     /* deliver EOF to OpenSSH */
    bg_pump_release(p);
    return NULL;
}

static int bg_status_to_errno(uint32_t st)
{
    if (st == SS$_NOSUCHDEV)
        return ENODEV;                  /* no executive INET device (Rule 9) */
    return ECONNREFUSED;
}

int ovmx_bg_connect(const struct sockaddr *sa, socklen_t salen)
{
    static const char devname[] = "TCPIP$DEVICE:";
    struct dsc$descriptor_s dev;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
    struct bg_sockaddr_in bsa;
    struct _iosb iosb;
    struct bg_pump *p = NULL;
    pthread_t t_out, t_in;
    int sv[2] = { -1, -1 };
    uint16_t chan = 0;
    uint32_t st;

    if (sa == NULL || (size_t)salen < sizeof(*sin) || sa->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    dev.dsc$w_length  = (uint16_t)(sizeof(devname) - 1);
    dev.dsc$b_dtype   = DSC$K_DTYPE_T;
    dev.dsc$b_class   = DSC$K_CLASS_S;
    dev.dsc$a_pointer = (char *)devname;

    /* $ASSIGN TCPIP$DEVICE:  -- SS$_NOSUCHDEV here means no /dev/vms; fail
     * honestly, never a raw socket() fallback (INV-6). */
    st = sys$assign(&dev, &chan, 0, 0);
    if (!(st & 1)) {
        errno = bg_status_to_errno(st);
        return -1;
    }

    /* IO$_SETMODE creates the executive-resident socket. */
    st = sys$qiow(0, chan, IO$_SETMODE, &iosb, 0, 0, 0, 0, 0, 0, 0, 0);
    if (!(st & 1)) {
        errno = bg_status_to_errno(st);
        goto fail_chan;
    }

    /* IO$_ACCESS connects to the peer (network-order port + addr, taken
     * straight from the AF_INET sockaddr the ssh client resolved). */
    bsa.family = 2;                     /* AF_INET as the driver expects */
    bsa.port   = sin->sin_port;         /* already network order */
    bsa.addr   = sin->sin_addr.s_addr;  /* already network order */
    st = sys$qiow(0, chan, IO$_ACCESS, &iosb, 0, 0,
                  &bsa, (uint32_t)sizeof(bsa), 0, 0, 0, 0);
    if (!(st & 1)) {
        errno = bg_status_to_errno(st);
        goto fail_chan;
    }

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        goto fail_chan;                 /* errno set by socketpair */

    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        errno = ENOMEM;
        goto fail_sv;
    }
    p->chan   = chan;
    p->ssh_fd = sv[1];                  /* the pump drives the internal end */
    p->refs   = 0;
    pthread_mutex_init(&p->lock, NULL);

    /* refs counts LIVE pump threads; each thread's bg_pump_release() decrements
     * and the last one deaccesses the channel + frees p.  Bump refs BEFORE each
     * create so a thread that runs to completion immediately still sees a
     * correct count. */
    pthread_mutex_lock(&p->lock);
    p->refs = 1;
    pthread_mutex_unlock(&p->lock);
    if (pthread_create(&t_out, NULL, bg_pump_out, p) != 0) {
        /* no thread ever ran; tear p down by hand (channel released below). */
        pthread_mutex_destroy(&p->lock);
        free(p);
        errno = EIO;
        goto fail_sv;
    }
    pthread_detach(t_out);

    pthread_mutex_lock(&p->lock);
    p->refs = 2;
    pthread_mutex_unlock(&p->lock);
    if (pthread_create(&t_in, NULL, bg_pump_in, p) != 0) {
        /* t_out is live and owns the channel now.  Drop the ref we just added
         * and shut the socketpair so t_out exits and does the final release.
         * From here p and chan belong to t_out -- do not touch them. */
        pthread_mutex_lock(&p->lock);
        p->refs = 1;
        pthread_mutex_unlock(&p->lock);
        shutdown(sv[1], SHUT_RDWR);
        close(sv[0]);
        errno = EIO;
        return -1;
    }
    pthread_detach(t_in);

    return sv[0];                       /* OpenSSH's pollable transport fd */

fail_sv:
    if (sv[0] >= 0)
        close(sv[0]);
    if (sv[1] >= 0)
        close(sv[1]);
    /* fallthrough: channel released by fail_chan */
fail_chan:
    if (chan) {
        struct _iosb io2;
        sys$qiow(0, chan, IO$_DEACCESS, &io2, 0, 0, 0, 0, 0, 0, 0, 0);
        sys$dassgn(chan);
    }
    return -1;
}
