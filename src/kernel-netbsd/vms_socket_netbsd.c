/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_socket_netbsd.c - the NetBSD realization of the exec_socket_* host TCP
 * client-socket seam (rd vms-9951, §12 of exec_kbackend.h; design record
 * docs/design-exec-socket-seam.md).
 *
 * This is the NetBSD TWIN of the Linux backend that lives static-inline in
 * exec_kbackend_linux.h. Like the block-device twin (vms_blockdev_netbsd.c), the
 * host-socket-coupled bodies stay OUT of the shared header exec_kbackend_netbsd.h
 * (which carries only the op declarations, the substrate-neutrality anchor) and
 * live here, where <sys/socketvar.h> and the in-kernel socket(9)/uio(9) KPIs are
 * in scope.
 *
 * SCOPE (vms-9951 = CONTRACT-ONLY, type-checked, never run). This TU compiles and
 * type-checks the seam against the REAL NetBSD in-kernel socket API -- proof the
 * §12 contract is genuinely implementable on NetBSD, not merely declarable. It is
 * NOT yet wired to a NetBSD BGn: device: the NetBSD executive has no bg_channels /
 * TCPIP$DEVICE: dispatch, so nothing calls these ops (vms_bg.c stays a Linux build
 * for now -- struct vms_proc has no bg_channels on NetBSD). A RUNNABLE NetBSD BGn:
 * -- wiring the device, and hardening the two points marked >>> vms-024 <<< below
 * (the asynchronous soconnect wait and the protocol-sockaddr path) against a real
 * QEMU/SIMH proof -- is rd vms-024. Until then these are honest, compiled, unrun.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): OVMX glue over the OPEN NetBSD socket(9) API; no
 * VMS/VSI source. "The IP stack itself is the host kernel's" -- OVMX never
 * reimplements a transport, on either substrate.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kmem.h>
#include <sys/atomic.h>
#include <sys/proc.h>            /* curlwp */
#include <sys/socket.h>
#include <sys/socketvar.h>      /* struct socket + socreate/soclose/soconnect/so*,
                                 * AND struct sockopt + sockopt_init/setint/getint/
                                 * destroy (NetBSD has no separate <sys/sockopt.h>) */
#include <sys/protosw.h>        /* so_proto->pr_usrreqs (pr_sockaddr/pr_peeraddr) */
#include <sys/uio.h>            /* struct uio, UIO_SETUP_SYSSPACE */
#include <sys/errno.h>
#include <netinet/in.h>         /* struct sockaddr_in, IPPROTO_TCP/IP */
#include <netinet/tcp.h>        /* TCP_NODELAY */

#include "exec_kbackend.h"      /* the op declarations (via exec_kbackend_netbsd.h) */

/* The opaque handle the seam passes around: a reference-counted holder over the
 * NetBSD in-kernel socket. exec_kbackend_netbsd.h forward-declares
 * `struct exec_socket_holder`; only this TU completes it (the Linux twin
 * completes its own in exec_kbackend_linux.h). NetBSD sockets carry no kref, so
 * the holder owns an atomic refcount; the channel holds one reference, a future
 * kqueue readiness rind (vms-024) a second. */
struct exec_socket_holder {
	struct socket	*so;
	unsigned int	 refcnt;
};

int
exec_socket_create(exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *so;
	int rc;

	*out = NULL;
	rc = socreate(AF_INET, &so, SOCK_STREAM, IPPROTO_TCP, curlwp, NULL);
	if (rc)
		return rc;
	h = kmem_alloc(sizeof(*h), KM_SLEEP);
	h->so = so;
	h->refcnt = 1;                  /* the channel's reference */
	*out = h;
	return 0;
}

/* Raw ICMP socket for PING (vms-80b): the SOCK_RAW/IPPROTO_ICMP twin of
 * exec_socket_create. Contract-only until a runnable NetBSD BGn: (vms-024). */
int
exec_socket_create_icmp(exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *so;
	int rc;

	*out = NULL;
	rc = socreate(AF_INET, &so, SOCK_RAW, IPPROTO_ICMP, curlwp, NULL);
	if (rc)
		return rc;
	h = kmem_alloc(sizeof(*h), KM_SLEEP);
	h->so = so;
	h->refcnt = 1;                  /* the channel's reference */
	*out = h;
	return 0;
}

void
exec_socket_get(exec_socket_t s)
{
	atomic_inc_uint(&s->refcnt);
}

void
exec_socket_release(exec_socket_t s)
{
	if (atomic_dec_uint_nv(&s->refcnt) == 0) {
		if (s->so)
			soclose(s->so);
		kmem_free(s, sizeof(*s));
	}
}

int
exec_socket_connect(exec_socket_t s, uint16_t family, uint16_t port_be,
    uint32_t addr_be)
{
	struct sockaddr_in sin;
	int rc;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = family;
	sin.sin_port = port_be;         /* network byte order, straight through */
	sin.sin_addr.s_addr = addr_be;

	solock(s->so);
	rc = soconnect(s->so, (struct sockaddr *)&sin, curlwp);
	/* >>> vms-024 <<< soconnect is ASYNCHRONOUS (unlike Linux kernel_connect
	 * with a 0 flag): the connection completes later. Wait for it to settle so
	 * the client-path caller sees a connected socket, exactly as the blocking
	 * Linux path does. The full hardening (interrupt handling, timeout policy)
	 * is the runnable-BGn: item; this loop is the documented shape. */
	if (rc == 0) {
		while ((s->so->so_state & SS_ISCONNECTING) != 0 &&
		    s->so->so_error == 0) {
			rc = sowait(s->so, true, 0);
			if (rc)
				break;
		}
		if (rc == 0) {
			rc = s->so->so_error;
			s->so->so_error = 0;
		}
	}
	sounlock(s->so);
	return rc;
}

long
exec_socket_send(exec_socket_t s, const void *buf, size_t len)
{
	struct uio uio;
	struct iovec iov;
	int rc;

	iov.iov_base = __UNCONST(buf);
	iov.iov_len = len;
	memset(&uio, 0, sizeof(uio));
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_resid = len;
	uio.uio_rw = UIO_WRITE;
	UIO_SETUP_SYSSPACE(&uio);

	rc = sosend(s->so, NULL, &uio, NULL, NULL, 0, curlwp);
	if (rc)
		return -rc;
	return (long)(len - uio.uio_resid);     /* bytes actually sent */
}

long
exec_socket_recv(exec_socket_t s, void *buf, size_t len)
{
	struct uio uio;
	struct iovec iov;
	int flags = 0;
	int rc;

	iov.iov_base = buf;
	iov.iov_len = len;
	memset(&uio, 0, sizeof(uio));
	uio.uio_iov = &iov;
	uio.uio_iovcnt = 1;
	uio.uio_offset = 0;
	uio.uio_resid = len;
	uio.uio_rw = UIO_READ;
	UIO_SETUP_SYSSPACE(&uio);

	rc = soreceive(s->so, NULL, &uio, NULL, NULL, &flags);
	if (rc)
		return -rc;
	return (long)(len - uio.uio_resid);     /* 0 = orderly peer close (EOF) */
}

int
exec_socket_shutdown(exec_socket_t s)
{
	return soshutdown(s->so, SHUT_RDWR);
}

int
exec_socket_getname(exec_socket_t s, int peer, uint16_t *family,
    uint16_t *port_be, uint32_t *addr_be)
{
	struct sockaddr_in *sin;
	struct sockaddr_storage ss;
	int rc;

	memset(&ss, 0, sizeof(ss));
	/* >>> vms-024 <<< the in-kernel local/peer address comes from the protocol
	 * via pr_usrreqs (PRU_SOCKADDR / PRU_PEERADDR), under the socket lock. The
	 * exact call shape is the runnable-BGn: hardening point; the contract here is
	 * "fill the same net-order AF_INET tuple the Linux twin does". */
	solock(s->so);
	if (peer)
		rc = (*s->so->so_proto->pr_usrreqs->pr_peeraddr)(s->so,
		    (struct sockaddr *)&ss);
	else
		rc = (*s->so->so_proto->pr_usrreqs->pr_sockaddr)(s->so,
		    (struct sockaddr *)&ss);
	sounlock(s->so);
	if (rc)
		return rc;
	if (ss.ss_family != AF_INET)
		return -EAFNOSUPPORT;           /* IPv6 not carried by this tuple yet */
	sin = (struct sockaddr_in *)&ss;
	*family = sin->sin_family;
	*port_be = sin->sin_port;
	*addr_be = sin->sin_addr.s_addr;
	return 0;
}

int
exec_socket_setopt_int(exec_socket_t s, int level, int name, int val)
{
	struct sockopt sopt;
	int rc;

	sockopt_init(&sopt, level, name, sizeof(int));
	rc = sockopt_setint(&sopt, val);
	if (rc == 0)
		rc = sosetopt(s->so, &sopt);
	sockopt_destroy(&sopt);
	return rc;
}

int
exec_socket_getopt_int(exec_socket_t s, int level, int name, int *out)
{
	struct sockopt sopt;
	int rc;

	/* Honest whitelist, mirroring the Linux twin: only the integer options
	 * OpenSSH probes are answered, each from the socket's genuine current state
	 * via sogetopt; anything else is an honest -ENOPROTOOPT (-> SS$_BADPARAM),
	 * never a faked value (INV-6). sogetopt applies over the documented int
	 * options; the whitelist keeps the honest-fail boundary identical across
	 * substrates. */
	if (!((level == SOL_SOCKET &&
	       (name == SO_KEEPALIVE || name == SO_REUSEADDR || name == SO_ERROR)) ||
	      (level == IPPROTO_TCP && name == TCP_NODELAY) ||
	      (level == IPPROTO_IP && name == IP_TOS)))
		return -ENOPROTOOPT;

	sockopt_init(&sopt, level, name, sizeof(int));
	rc = sogetopt(s->so, &sopt);
	if (rc == 0)
		rc = sockopt_getint(&sopt, out);
	sockopt_destroy(&sopt);
	return rc;
}

/* ---- server path (vms-698): bind / listen / accept ------------------------ */

int
exec_socket_bind(exec_socket_t s, uint16_t family, uint16_t port_be,
    uint32_t addr_be)
{
	struct sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = family;
	sin.sin_port = port_be;         /* network byte order, straight through */
	sin.sin_addr.s_addr = addr_be;
	return sobind(s->so, (struct sockaddr *)&sin, curlwp);
}

int
exec_socket_listen(exec_socket_t s, int backlog)
{
	return solisten(s->so, backlog, curlwp);
}

int
exec_socket_accept(exec_socket_t s, exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct socket *so = s->so, *so2;
	struct sockaddr_storage ss;
	int rc = 0;

	*out = NULL;
	solock(so);
	/* >>> vms-024 <<< block until a connection is queued, exactly the shape
	 * do_sys_accept uses (uipc_syscalls.c): wait on so_qlen, then pull the head
	 * of so_q with soqremque and soaccept it. The interrupt/timeout hardening is
	 * the runnable-BGn: item; this is the documented in-kernel accept path. */
	while (so->so_qlen == 0 && so->so_error == 0) {
		rc = sowait(so, true, 0);
		if (rc)
			break;
	}
	if (rc == 0 && so->so_error != 0) {
		rc = so->so_error;
		so->so_error = 0;
	}
	if (rc != 0) {
		sounlock(so);
		return rc;
	}
	so2 = TAILQ_FIRST(&so->so_q);
	if (so2 == NULL || soqremque(so2, 1) == 0) {
		sounlock(so);
		return -EINVAL;
	}
	memset(&ss, 0, sizeof(ss));
	rc = soaccept(so2, (struct sockaddr *)&ss);     /* finalize the accepted socket */
	sounlock(so);
	if (rc != 0) {
		soclose(so2);
		return rc;
	}
	h = kmem_alloc(sizeof(*h), KM_SLEEP);
	h->so = so2;
	h->refcnt = 1;                  /* the accepting channel's reference */
	*out = h;
	return 0;
}
