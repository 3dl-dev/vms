/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_ssh_glue.c - OVMX port layer: route OpenSSH's transport onto the OVMX
 * BSD-sockets veneer over BGn:. See ovmx_ssh_glue.h for the model.
 *
 * WHY A SOCKETPAIR BRIDGE (not the raw veneer handle / a readiness fd). OpenSSH
 * treats its connection fd as a full BSD socket: besides read()/write()/poll()
 * it does getsockname()/getpeername()/setsockopt() and, crucially, the SSH
 * version-banner exchange (kex.c) does its own atomicio(read/vwrite) on the fd,
 * OUTSIDE the packet layer. A veneer HANDLE is not an fd, and the executive
 * readiness fd has no read/write file-ops -- either makes those raw fd calls
 * fail (EBADF / "Not a socket"). So ovmx_ssh_connect() hands OpenSSH a REAL
 * fd -- one end of an AF_UNIX socketpair -- and a pair of pump threads shuttle
 * every byte between the other end and the executive-resident BGn: socket via
 * the veneer (ovmx_send/ovmx_recv). ALL of OpenSSH's socket operations then work
 * unchanged, and every wire byte still transits the veneer -> $QIO -> executive.
 * The veneer's BGn: channel is process-wide (keyed on the executive proc, not a
 * thread) and full-duplex, so a blocking recv and a blocking send run
 * concurrently in the two pump threads (proven by test_syssvc_bgsock_echo).
 *
 * The fd OpenSSH gets is an ordinary socketpair end, so the packet-layer shims
 * (ovmx_ssh_read/write/sshbuf_read) simply fall through to read()/write()/
 * sshbuf_read on it -- they add nothing here but stay harmless.
 *
 * INV-6 / Rule 9. ovmx_ssh_connect() fails (-1) when /dev/vms is absent (the
 * veneer's ovmx_socket returns ENODEV) -- never a raw socket() fallback.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "sshbuf.h"      /* OpenSSH: sshbuf_read (fall-through path) */
#include "ssherr.h"

#include "ovmx/vms_bgsock.h"
#include "ovmx/ovmx_ssh_glue.h"

#ifndef OVMX_WRAP   /* the socketpair+pump bridge -- retired in the --wrap de-veneer build */
/* Per-connection pump state. Refcounted so the last pump thread out releases the
 * veneer socket. OVMX_BG_XFER_MAX mirrors the veneer's per-op cap. */
#define OVMX_SSH_XFER 4096
struct ovmx_ssh_pump {
    int             handle;     /* veneer socket handle (ovmx_*) */
    int             int_fd;     /* internal socketpair end the pumps drive */
    pthread_mutex_t lock;
    int             refs;
    int             closed;
};

static void pump_release(struct ovmx_ssh_pump *p)
{
    int last;
    pthread_mutex_lock(&p->lock);
    last = (--p->refs == 0);
    if (last && !p->closed) { ovmx_socket_close(p->handle); p->closed = 1; }
    pthread_mutex_unlock(&p->lock);
    if (last) { close(p->int_fd); pthread_mutex_destroy(&p->lock); free(p); }
}

/* app -> wire: read what OpenSSH wrote to the socketpair, ovmx_send it. */
static void *pump_out(void *arg)
{
    struct ovmx_ssh_pump *p = arg;
    unsigned char buf[OVMX_SSH_XFER];
    for (;;) {
        ssize_t n = read(p->int_fd, buf, sizeof(buf));
        ssize_t off = 0;
        if (n <= 0) break;
        while (off < n) {
            ssize_t w = ovmx_send(p->handle, buf + off, (size_t)(n - off), 0);
            if (w <= 0) goto done;
            off += w;
        }
    }
done:
    shutdown(p->int_fd, SHUT_RDWR);
    pump_release(p);
    return NULL;
}

/* wire -> app: ovmx_recv from the executive socket, write into the socketpair. */
static void *pump_in(void *arg)
{
    struct ovmx_ssh_pump *p = arg;
    unsigned char buf[OVMX_SSH_XFER];
    for (;;) {
        ssize_t n = ovmx_recv(p->handle, buf, sizeof(buf), 0);
        ssize_t off = 0;
        if (n <= 0) break;              /* 0 = orderly peer close (EOF) */
        while (off < n) {
            ssize_t w = write(p->int_fd, buf + off, (size_t)(n - off));
            if (w <= 0) goto done;
            off += w;
        }
    }
done:
    shutdown(p->int_fd, SHUT_RDWR);
    pump_release(p);
    return NULL;
}
#endif /* !OVMX_WRAP */

int ovmx_ssh_connect(const struct sockaddr *sa, socklen_t salen)
{
#ifndef OVMX_WRAP
    struct ovmx_ssh_pump *p;
    pthread_t t_out, t_in;
    int sv[2] = { -1, -1 };
#endif
    int handle;

    if (sa == NULL || (size_t)salen < sizeof(struct sockaddr_in) ||
        sa->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    handle = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0)
        return -1;                      /* errno set (ENODEV if no /dev/vms) */
    if (ovmx_connect(handle, sa, salen) != 0) {
        int e = errno; ovmx_socket_close(handle); errno = e; return -1;
    }

#ifdef OVMX_WRAP
    /*
     * DE-VENEER (vms-4bf): return the veneer HANDLE directly -- no socketpair,
     * no pump threads. The `ssh` binary is linked with `-Wl,--wrap=read,write,
     * getpeername,setsockopt,poll,ppoll,...` (ovmx_ssh_wrap.c), so every fd
     * syscall OpenSSH issues on this handle -- the kex.c banner atomicio,
     * getpeername for known_hosts, setsockopt(TCP_NODELAY), the clientloop poll
     * set -- dispatches to a real $QIO on BGn:. The handle is >= OVMX_BGSOCK_BASE
     * so the wrappers can tell it from a real fd.
     */
    return handle;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        int e = errno; ovmx_socket_close(handle); errno = e; return -1;
    }
    p = calloc(1, sizeof(*p));
    if (p == NULL) { close(sv[0]); close(sv[1]); ovmx_socket_close(handle); errno = ENOMEM; return -1; }
    p->handle = handle;
    p->int_fd = sv[1];
    pthread_mutex_init(&p->lock, NULL);

    pthread_mutex_lock(&p->lock); p->refs = 1; pthread_mutex_unlock(&p->lock);
    if (pthread_create(&t_out, NULL, pump_out, p) != 0) {
        pthread_mutex_destroy(&p->lock); free(p);
        close(sv[0]); close(sv[1]); ovmx_socket_close(handle);
        errno = EIO; return -1;
    }
    pthread_mutex_lock(&p->lock); p->refs = 2; pthread_mutex_unlock(&p->lock);
    if (pthread_create(&t_in, NULL, pump_in, p) != 0) {
        /* t_out owns the veneer socket now; drop our ref, shut the pipe so it
         * exits and does the final release. From here p belongs to t_out. */
        pthread_mutex_lock(&p->lock); p->refs = 1; pthread_mutex_unlock(&p->lock);
        shutdown(sv[1], SHUT_RDWR);
        close(sv[0]);
        errno = EIO; return -1;
    }
    pthread_detach(t_out);
    pthread_detach(t_in);
    return sv[0];                       /* OpenSSH's connection fd: a real socket */
#endif /* OVMX_WRAP */
}

int ovmx_ssh_is_conn(int fd)
{
    (void)fd;
    return 0;   /* the connection fd is a real socketpair end; no special mapping */
}

/* The connection fd is a real socket, so these are plain pass-throughs; the
 * packet-layer patch calls them but they add nothing over read()/write(). */
ssize_t ovmx_ssh_read(int fd, void *buf, size_t n)  { return read(fd, buf, n); }
ssize_t ovmx_ssh_write(int fd, const void *buf, size_t n) { return write(fd, buf, n); }
int ovmx_ssh_sshbuf_read(int fd, struct sshbuf *buf, size_t maxlen, size_t *rlen)
{
    return sshbuf_read(fd, buf, maxlen, rlen);
}
