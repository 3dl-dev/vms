/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_ssh_glue.c - OVMX port layer: route OpenSSH's transport + event loop
 * onto the OVMX BSD-sockets veneer over BGn:. See ovmx_ssh_glue.h for the model.
 *
 * Built INSIDE the OpenSSH source tree (build-openssh.sh copies it there), so it
 * may use OpenSSH's sshbuf/ssherr headers. It talks to the OVMX veneer through
 * the veneer's public API only (ovmx_socket/ovmx_connect/ovmx_send/ovmx_recv/
 * ovmx_pollfd/ovmx_socket_close) -- no $QIO/vms_kif here.
 */

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "sshbuf.h"      /* OpenSSH: struct sshbuf, sshbuf_reserve/consume_end */
#include "ssherr.h"      /* OpenSSH: SSH_ERR_* */

#include "ovmx/vms_bgsock.h"   /* the OVMX veneer (copied under ovmx/ at build) */
#include "ovmx/ovmx_ssh_glue.h"

/* fd (veneer readiness fd) -> veneer socket handle. A client process holds one
 * or two connections; a tiny fixed map under a mutex suffices. */
struct conn_map { int in_use; int fd; int handle; };
#define OVMX_SSH_CONN_MAX 8
static struct conn_map g_conns[OVMX_SSH_CONN_MAX];
static pthread_mutex_t g_conns_lock = PTHREAD_MUTEX_INITIALIZER;

static int handle_for(int fd)
{
    int i, h = -1;
    pthread_mutex_lock(&g_conns_lock);
    for (i = 0; i < OVMX_SSH_CONN_MAX; i++)
        if (g_conns[i].in_use && g_conns[i].fd == fd) { h = g_conns[i].handle; break; }
    pthread_mutex_unlock(&g_conns_lock);
    return h;
}

static void conn_add(int fd, int handle)
{
    int i;
    pthread_mutex_lock(&g_conns_lock);
    for (i = 0; i < OVMX_SSH_CONN_MAX; i++)
        if (!g_conns[i].in_use) {
            g_conns[i].in_use = 1; g_conns[i].fd = fd; g_conns[i].handle = handle;
            break;
        }
    pthread_mutex_unlock(&g_conns_lock);
}

int ovmx_ssh_is_conn(int fd)
{
    return fd >= 0 && handle_for(fd) >= 0;
}

int ovmx_ssh_connect(const struct sockaddr *sa, socklen_t salen)
{
    int handle, pfd;

    if (sa == NULL || (size_t)salen < sizeof(struct sockaddr_in) ||
        sa->sa_family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    handle = ovmx_socket(AF_INET, SOCK_STREAM, 0);
    if (handle < 0)
        return -1;                      /* errno set (ENODEV if no /dev/vms) */

    if (ovmx_connect(handle, sa, salen) != 0) {
        int e = errno;
        ovmx_socket_close(handle);
        errno = e;
        return -1;
    }

    /* Hand OpenSSH the REAL readiness fd as its connection fd: its poll/ppoll
     * paths then work unchanged; the data shims below map it back to `handle`. */
    pfd = ovmx_pollfd(handle);
    if (pfd < 0) {
        int e = errno;
        ovmx_socket_close(handle);
        errno = e;
        return -1;
    }
    conn_add(pfd, handle);
    return pfd;
}

ssize_t ovmx_ssh_read(int fd, void *buf, size_t n)
{
    int h = handle_for(fd);
    if (h < 0)
        return read(fd, buf, n);        /* not a veneer connection */
    return ovmx_recv(h, buf, n, 0);     /* 0 = EOF, -1 = error (errno set) */
}

ssize_t ovmx_ssh_write(int fd, const void *buf, size_t n)
{
    int h = handle_for(fd);
    if (h < 0)
        return write(fd, buf, n);
    return ovmx_send(h, buf, n, 0);
}

/* Mirror sshbuf_read() but move bytes through the veneer for a connection fd:
 * reserve maxlen, ovmx_recv into it, trim to the actual count. */
int ovmx_ssh_sshbuf_read(int fd, struct sshbuf *buf, size_t maxlen, size_t *rlen)
{
    int h = handle_for(fd);
    u_char *d;
    ssize_t len;
    int r;

    if (h < 0)
        return sshbuf_read(fd, buf, maxlen, rlen);   /* non-connection fd */

    if (rlen != NULL)
        *rlen = 0;
    if ((r = sshbuf_reserve(buf, maxlen, &d)) != 0)
        return r;
    len = ovmx_recv(h, d, maxlen, 0);
    if (len == -1) {
        (void)sshbuf_consume_end(buf, maxlen);
        return SSH_ERR_SYSTEM_ERROR;
    }
    if (len == 0) {
        (void)sshbuf_consume_end(buf, maxlen);
        return SSH_ERR_CONN_CLOSED;      /* orderly peer close (EOF) */
    }
    if ((r = sshbuf_consume_end(buf, maxlen - (size_t)len)) != 0)
        return r;
    if (rlen != NULL)
        *rlen = (size_t)len;
    return 0;
}
