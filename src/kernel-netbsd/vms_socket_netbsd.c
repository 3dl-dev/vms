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
 * SS13 (the raw datalink, exec_l2_*) is the EXCEPTION, and is REAL (rd vms-9394):
 * src/kernel-core/vms_l2.c is in this module's SRCS and the bottom of this file
 * binds its exec_l2_* to the interface (promiscuous receive into a per-handle
 * ring through an `_if_input' interposer, if_transmit_lock(9) transmit) -- the
 * executive datalink a non-root NETACP opens with PHY_IO on OVMX/VAX.
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
struct ovmx_l2_port;             /* the SS13 datalink port (below, rd vms-9394) */

struct exec_socket_holder {
	struct socket	*so;
	unsigned int	 refcnt;
	struct ovmx_l2_port *l2;       /* non-NULL: an exec_l2_open handle, no `so' */
};

static void ovmx_l2_port_close(struct ovmx_l2_port *);

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
	h->l2 = NULL;
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
	h->l2 = NULL;
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
		if (s->l2)
			ovmx_l2_port_close(s->l2);   /* rd vms-9394 */
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
	h->l2 = NULL;
	*out = h;
	return 0;
}

/* ---- SS13: the executive raw datalink, NetBSD binding (rd vms-9394) --------
 *
 * WHAT THIS IS. The NetBSD half of exec_kbackend.h SS13: the substrate under
 * src/kernel-core/vms_l2.c, so a NON-ROOT VMS process holding PHY_IO (NETACP,
 * SYS$SYSTEM:DECNETD.EXE) gets its raw Ethernet datalink -- DECnet Phase IV,
 * ethertype 0x6003 -- from the EXECUTIVE. The facility above owns the policy
 * (PHY_IO at OPEN, the station address, the executive-stamped source field);
 * this binding owns only the frames. NetBSD has no in-kernel socket(9) domain
 * for raw Ethernet (Linux's AF_PACKET), and bpf(4) is a userspace device, so
 * there is no socket here at all: an L2 "socket" (exec_socket_t) is a holder
 * whose `l2' points at an ovmx_l2_port -- a per-handle receive ring bound to
 * one interface + ethertype.
 *
 * RECEIVE. A DECnet node's station address is ALGORITHMIC (AA-00-04-00-
 * <LE16(area<<10|node)>, vms_l2_station.h), not the NIC's burned-in address,
 * and qt(4)/qe(4) have no secondary-unicast filter -- so, exactly like the
 * Linux binding (dev_set_promiscuity, vms-a84d), OPEN puts the NIC into
 * promiscuous mode (ifpromisc(9), reference-counted, dropped at CLOSE). That
 * is also why the frames cannot be taken from pfil(9) the way the cluster wire
 * is (vms_lan_netbsd.c): ether_input() marks a promiscuously-received frame
 * M_PROMISC and SKIPS the pfil hooks for it (docs/research-netbsd-lan-
 * binding.md SS1), so unicast to the Phase IV station would never reach a
 * pfil hook. Instead the binding interposes the interface's input routine,
 * `_if_input' -- the documented hook bridge(4) uses (if_bridge.c; the
 * research doc's "if_input interposition" alternative): ovmx_l2_if_input()
 * runs where ether_input() would (the driver's softint, if_percpuq_softint /
 * if_input), copies a frame whose ethertype an open port is bound to into
 * that port's ring, and hands EVERY other frame -- IP, ARP, the cluster's
 * 0x6007 -- to the saved routine untouched, so ether_input() and the
 * cluster's pfil hook see exactly the frames they saw before. A port receives
 * every frame of its ethertype on its interface, the same set a Linux
 * AF_PACKET socket bound to that ethertype on a promiscuous NIC receives:
 * the station unicast, the DECnet multicasts (AB-00-00-03-00-00 all-routers,
 * AB-00-00-04-00-00 all-endnodes) and broadcast alike; NETACP's routing layer
 * does its own addressing, as it does on Linux.
 *
 * CLUSTER PATH UNCHANGED. vms_lan_netbsd.c (the 0x6007 cluster wire) is not
 * touched: no frame it sends or receives changes. Its pfil hook still runs
 * inside ether_input() for every 0x6007 frame (this interposer passes them
 * through), and promiscuous mode only ADDS frames addressed to other stations,
 * which ether_input() marks M_PROMISC and drops before pfil exactly as it
 * would for any other promiscuous listener (tcpdump, bridge).
 *
 * TRANSMIT. if_transmit_lock(9) on a fresh mbuf -- the cluster binding's own
 * exec_lan_xmit shape -- carrying the complete frame the facility built and
 * source-stamped (vms_l2.c owns bytes 6..11), padded to the Ethernet minimum.
 *
 * CONTEXT RULES. The ring and the port list live under ONE spin mutex at
 * IPL_NET (the input routine runs in softint); nothing in the receive path
 * allocates, sleeps or calls the facility. RECV waits on the port's condvar
 * with cv_timedwait_sig, so a NETACP being deleted is not held by a wait.
 *
 * LIFETIME / KNOWN LIMIT. The bound interface is the node's primary NIC,
 * attached at boot (qt0 under SIMH/pcjs); like vms_lan_netbsd.c's g_lan_ifp
 * this binding keeps the ifnet pointer for the port's life and does not
 * follow a detach. The interposition is installed on first OPEN of an
 * interface and stays until module unload (vms_l2_netbsd_fini), where the
 * saved routine is put back and xc_barrier(9) waits out any CPU still in it.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): OVMX glue over public NetBSD KPIs -- ifunit,
 * ifpromisc, if_ioctl(SIOCSIFFLAGS), if_transmit_lock, the mbuf(9) copy
 * routines, `_if_input' (net/if.h), condvar(9), mutex(9), xcall(9) -- read
 * from the pinned 10.1 syssrc headers/sources to confirm their shape. */

#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/sockio.h>          /* SIOCSIFFLAGS */
#include <sys/xcall.h>           /* xc_barrier (module unload) */
#include <net/if.h>
#include <net/if_dl.h>           /* CLLADDR */
#include <net/if_ether.h>        /* ETHER_ADDR_LEN / ETHER_MIN_LEN / ETHER_CRC_LEN */
#include <net/if_types.h>        /* IFT_ETHER */

#include "vms_l2_ring.h"         /* the ring bookkeeping (host-unit-tested) */

#define OVMX_L2_RING       16     /* frames queued per handle before drop-oldest */
#define OVMX_L2_FRAME_MAX  1536   /* >= ETHER_MAX_LEN - ETHER_CRC_LEN; a longer
                                   * frame is truncated (never over-read) */
#define OVMX_L2_MAX_TAPS   4      /* interfaces whose input routine we hold */

struct ovmx_l2_port {
	struct ovmx_l2_port *next;     /* in g_l2_ports, under g_l2_mtx */
	struct ifnet	*ifp;          /* the bound interface */
	uint16_t	 ethertype;    /* host order */
	int		 promisc;      /* holds one ifpromisc() reference */
	int		 closing;      /* CLOSE in progress: wake + refuse waits */
	struct vms_l2_ring ring;       /* slot bookkeeping (vms_l2_ring.h) */
	unsigned int	 waiters;      /* RECVs inside cv_timedwait_sig */
	kcondvar_t	 cv;           /* paired with g_l2_mtx */
	uint16_t	 flen[OVMX_L2_RING];
	uint8_t		 fbuf[OVMX_L2_RING][OVMX_L2_FRAME_MAX];
};

struct ovmx_l2_tap {
	struct ifnet	*ifp;
	void		(*orig)(struct ifnet *, struct mbuf *);
};

static kmutex_t			g_l2_mtx;        /* IPL_NET spin */
static kmutex_t			g_l2_cfg_mtx;    /* adaptive: serializes OPEN/CLOSE setup */
static int			g_l2_inited;
static struct ovmx_l2_port	*g_l2_ports;
static struct ovmx_l2_tap	g_l2_taps[OVMX_L2_MAX_TAPS];

/*
 * vms_l2_netbsd_init - module load (MODULE_CMD_INIT, before the cdevsw is
 * attached, so no ioctl can reach exec_l2_* first).
 */
void
vms_l2_netbsd_init(void)
{
	mutex_init(&g_l2_mtx, MUTEX_DEFAULT, IPL_NET);
	mutex_init(&g_l2_cfg_mtx, MUTEX_DEFAULT, IPL_NONE);
	g_l2_inited = 1;
}

/* The input routine saved for `ifp' (NULL: not tapped). Caller holds
 * g_l2_mtx or is the only writer (cfg mutex). */
static void (*ovmx_l2_tap_orig(struct ifnet *ifp))(struct ifnet *, struct mbuf *)
{
	int i;

	for (i = 0; i < OVMX_L2_MAX_TAPS; i++)
		if (g_l2_taps[i].ifp == ifp)
			return g_l2_taps[i].orig;
	return NULL;
}

/*
 * ovmx_l2_deliver - copy one received frame into every open port bound to
 * (ifp, ethertype). Returns how many ports took it. Receive context: spin
 * mutex only, no allocation, drop-OLDEST when a ring is full (a datalink
 * loses frames under overload; DECnet's NSP retransmits).
 */
static int
ovmx_l2_deliver(struct ifnet *ifp, uint16_t ethertype, struct mbuf *m)
{
	struct ovmx_l2_port *p;
	int len = m->m_pkthdr.len, n = 0;
	unsigned int slot;

	if (len > OVMX_L2_FRAME_MAX)
		len = OVMX_L2_FRAME_MAX;

	mutex_spin_enter(&g_l2_mtx);
	for (p = g_l2_ports; p != NULL; p = p->next) {
		if (p->ifp != ifp || p->ethertype != ethertype || p->closing)
			continue;
		slot = vms_l2_ring_push(&p->ring);   /* drop-oldest when full */
		m_copydata(m, 0, len, p->fbuf[slot]);
		p->flen[slot] = (uint16_t)len;
		cv_broadcast(&p->cv);
		n++;
	}
	mutex_spin_exit(&g_l2_mtx);
	return n;
}

/*
 * ovmx_l2_if_input - the interposed `_if_input'. Frames of an ethertype an
 * open port is bound to are taken (and consumed -- NetBSD has no protocol for
 * them; ether_input() would only count them as noproto and drop them); every
 * other frame goes to the saved routine (ether_input) untouched.
 */
static void
ovmx_l2_if_input(struct ifnet *ifp, struct mbuf *m)
{
	void (*orig)(struct ifnet *, struct mbuf *);
	uint8_t eh[2 * ETHER_ADDR_LEN + 2];

	mutex_spin_enter(&g_l2_mtx);
	orig = ovmx_l2_tap_orig(ifp);
	mutex_spin_exit(&g_l2_mtx);

	if ((m->m_flags & M_PKTHDR) != 0 &&
	    m->m_pkthdr.len >= (int)(2 * ETHER_ADDR_LEN + 2)) {
		m_copydata(m, 0, sizeof(eh), eh);
		if (ovmx_l2_deliver(ifp,
		    (uint16_t)vms_l2_frame_ethertype(eh), m) > 0) {
			m_freem(m);
			return;
		}
	}
	if (orig != NULL)
		orig(ifp, m);
	else
		m_freem(m);   /* unreachable: we are only installed with a saved routine */
}

/* Install the interposer on `ifp' once. Caller holds g_l2_cfg_mtx. */
static int
ovmx_l2_tap(struct ifnet *ifp)
{
	int i, freei = -1;

	for (i = 0; i < OVMX_L2_MAX_TAPS; i++) {
		if (g_l2_taps[i].ifp == ifp)
			return 0;                      /* already ours */
		if (g_l2_taps[i].ifp == NULL && freei < 0)
			freei = i;
	}
	if (freei < 0)
		return ENOSPC;
	if (ifp->_if_input == ovmx_l2_if_input)
		return 0;
	/* Record the saved routine BEFORE the swap, under the rx mutex, so the
	 * interposer can never run without finding it. */
	mutex_spin_enter(&g_l2_mtx);
	g_l2_taps[freei].orig = ifp->_if_input;
	g_l2_taps[freei].ifp = ifp;
	mutex_spin_exit(&g_l2_mtx);
	membar_producer();
	ifp->_if_input = ovmx_l2_if_input;
	return 0;
}

/*
 * The same `ifconfig <if> up' the cluster binding performs before its first
 * transmit (vms_lan_netbsd.c, rd vms-613: qt(4) arms its transmit ring only
 * from its own SIOCSIFFLAGS IFF_UP arm). Idempotent; a no-op when up.
 */
static int
ovmx_l2_ifup(struct ifnet *ifp)
{
	struct ifreq ifr;
	int error;

	if ((ifp->if_flags & IFF_UP) != 0)
		return 0;
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifp->if_xname, sizeof(ifr.ifr_name));
	ifr.ifr_flags = ifp->if_flags | IFF_UP;
	IFNET_LOCK(ifp);
	error = if_ioctl(ifp, SIOCSIFFLAGS, &ifr);
	IFNET_UNLOCK(ifp);
	return error;
}

/* An Ethernet interface by name, or NULL. */
static struct ifnet *
ovmx_l2_ifether(const char *ifname)
{
	struct ifnet *ifp = ifunit(ifname);

	if (ifp == NULL || ifp->if_type != IFT_ETHER || ifp->if_sadl == NULL)
		return NULL;
	return ifp;
}

int
exec_l2_open(const char *ifname, uint16_t ethertype, uint32_t *out_ifindex,
    exec_socket_t *out)
{
	struct exec_socket_holder *h;
	struct ovmx_l2_port *p;
	struct ifnet *ifp;
	int error;

	*out = NULL;
	if (!g_l2_inited)
		return -ENODEV;

	ifp = ovmx_l2_ifether(ifname);
	if (ifp == NULL)
		return -ENODEV;

	p = kmem_zalloc(sizeof(*p), KM_SLEEP);
	p->ifp = ifp;
	p->ethertype = ethertype;
	vms_l2_ring_init(&p->ring, OVMX_L2_RING);
	cv_init(&p->cv, "ovmxl2rx");

	mutex_enter(&g_l2_cfg_mtx);
	error = ovmx_l2_ifup(ifp);
	if (error == 0)
		error = ovmx_l2_tap(ifp);
	if (error == 0) {
		error = ifpromisc(ifp, 1);
		if (error == 0)
			p->promisc = 1;
	}
	if (error != 0) {
		mutex_exit(&g_l2_cfg_mtx);
		cv_destroy(&p->cv);
		kmem_free(p, sizeof(*p));
		return -error;
	}
	mutex_spin_enter(&g_l2_mtx);
	p->next = g_l2_ports;
	g_l2_ports = p;                  /* armed: frames start arriving now */
	mutex_spin_exit(&g_l2_mtx);
	mutex_exit(&g_l2_cfg_mtx);

	h = kmem_alloc(sizeof(*h), KM_SLEEP);
	h->so = NULL;
	h->refcnt = 1;                   /* the handle's reference */
	h->l2 = p;
	*out = h;
	if (out_ifindex)
		*out_ifindex = (uint32_t)ifp->if_index;
	return 0;
}

/* Unarm, wake and free a port (the holder's last reference is gone). */
static void
ovmx_l2_port_close(struct ovmx_l2_port *p)
{
	struct ovmx_l2_port **pp;

	mutex_enter(&g_l2_cfg_mtx);
	mutex_spin_enter(&g_l2_mtx);
	for (pp = &g_l2_ports; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	}
	p->closing = 1;
	cv_broadcast(&p->cv);
	while (p->waiters > 0)           /* a RECV still inside its wait */
		cv_wait(&p->cv, &g_l2_mtx);
	mutex_spin_exit(&g_l2_mtx);
	if (p->promisc)
		(void)ifpromisc(p->ifp, 0);
	mutex_exit(&g_l2_cfg_mtx);

	cv_destroy(&p->cv);
	kmem_free(p, sizeof(*p));
}

int
exec_l2_hwaddr(const char *ifname, uint8_t mac[6])
{
	struct ifnet *ifp = ovmx_l2_ifether(ifname);

	if (ifp == NULL)
		return -ENODEV;
	memcpy(mac, CLLADDR(ifp->if_sadl), ETHER_ADDR_LEN);
	return 0;
}

long
exec_l2_send(exec_socket_t s, int ifindex, uint16_t ethertype,
    const uint8_t dst_mac[6], const void *frame, size_t len)
{
	static const uint8_t zpad[ETHER_MIN_LEN - ETHER_CRC_LEN];
	struct ovmx_l2_port *p;
	struct mbuf *m;
	size_t wire;

	(void)ifindex;     /* the port is bound to exactly one interface */
	(void)ethertype;   /* the frame carries its own (the facility built it) */
	(void)dst_mac;     /* ... and its own destination: sent VERBATIM, as on
	                    * Linux (exec_kbackend_linux.h, vms-a84d) */
	if (s == NULL || (p = s->l2) == NULL)
		return -EBADF;
	if (len < 2 * ETHER_ADDR_LEN + 2 || len > OVMX_L2_FRAME_MAX)
		return -EINVAL;

	wire = len < sizeof(zpad) ? sizeof(zpad) : len;
	m = m_gethdr(M_WAIT, MT_DATA);
	m->m_pkthdr.len = 0;
	m->m_len = 0;
	m_copyback(m, 0, (int)len, frame);
	if (wire > len)                  /* pad a runt to the Ethernet minimum */
		m_copyback(m, (int)len, (int)(wire - len), zpad);
	if (m->m_pkthdr.len != (int)wire) {
		m_freem(m);              /* m_copyback could not grow the chain */
		return -ENOBUFS;
	}
	if (if_transmit_lock(p->ifp, m) != 0)   /* consumes m either way */
		return -EIO;
	return (long)len;
}

int
exec_l2_recv(exec_socket_t s, void *buf, size_t buf_len, uint32_t timeout_ms,
    size_t *out_len)
{
	struct ovmx_l2_port *p;
	unsigned int slot;
	size_t n;
	int error = 0, ticks;

	if (s == NULL || (p = s->l2) == NULL)
		return -EBADF;
	ticks = 0;                               /* 0 = wait indefinitely */
	if (timeout_ms != 0) {
		ticks = mstohz(timeout_ms);
		if (ticks < 1)
			ticks = 1;
	}

	mutex_spin_enter(&g_l2_mtx);
	while (p->ring.count == 0 && !p->closing && error == 0) {
		p->waiters++;
		error = cv_timedwait_sig(&p->cv, &g_l2_mtx, ticks);
		p->waiters--;
		if (p->closing)
			cv_broadcast(&p->cv);    /* let CLOSE see waiters == 0 */
	}
	if (!vms_l2_ring_pop(&p->ring, &slot)) {
		mutex_spin_exit(&g_l2_mtx);
		if (error == EWOULDBLOCK || error == 0)
			return -EAGAIN;          /* timed out (or closing): no frame */
		return -EINTR;                   /* interrupted (signal) */
	}
	n = p->flen[slot];
	if (n > buf_len)
		n = buf_len;
	memcpy(buf, p->fbuf[slot], n);
	mutex_spin_exit(&g_l2_mtx);
	*out_len = n;
	return 0;
}

/*
 * vms_l2_netbsd_fini - module unload: every handle is already gone (each
 * process's vms_l2_release_all ran in vms_proctab_teardown); put back each
 * interface's saved input routine, then wait out any CPU still executing the
 * interposer before this module's text can go away.
 */
void
vms_l2_netbsd_fini(void)
{
	int i, any = 0;

	if (g_l2_inited != 1)
		return;
	for (i = 0; i < OVMX_L2_MAX_TAPS; i++) {
		struct ifnet *ifp = g_l2_taps[i].ifp;

		if (ifp == NULL)
			continue;
		if (ifp->_if_input == ovmx_l2_if_input)
			ifp->_if_input = g_l2_taps[i].orig;
		any = 1;
	}
	if (any)
		xc_barrier(0);
	for (i = 0; i < OVMX_L2_MAX_TAPS; i++) {
		g_l2_taps[i].ifp = NULL;
		g_l2_taps[i].orig = NULL;
	}
	mutex_destroy(&g_l2_cfg_mtx);
	mutex_destroy(&g_l2_mtx);
	g_l2_inited = 0;
}
