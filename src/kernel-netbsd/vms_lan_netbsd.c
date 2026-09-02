/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_lan_netbsd.c - the NetBSD realization of the cluster seam, families
 * SS14..SS18 of exec_kbackend.h (FC-P0.4; design
 * docs/design-faithful-cluster-executive.md SS3.2.1/SS3.2.2).
 *
 * This is the NetBSD TWIN of the Linux binding that lives static-inline in
 * exec_kbackend_linux.h. It follows the vms_socket_netbsd.c / vms_blockdev_
 * netbsd.c pattern: exec_kbackend_netbsd.h carries only the declarations (the
 * substrate-neutrality anchor -- every signature is expressible in NetBSD terms)
 * and the bodies live here, where the link-layer, kthread(9) and callout(9) KPIs
 * belong.
 *
 * FC-P0.4 SCOPE (this landing). FC-P0.1 froze the contract as honest
 * SS$_NOSUCHDEV stubs; FC-P0.3's spike (docs/research-netbsd-lan-binding.md,
 * read against NetBSD 10.1's syssrc) recorded the concrete facts this file
 * binds to:
 *
 *   SS14 rx     pfil(9) on ifp->if_pfil, the hook ether_input() runs BEFORE
 *               ethertype dispatch (if_ethersubr.c). Delivered at
 *               IPL_SOFTNET/SOFTINT_NET on the qe(4) (DEQNA/DELQA) driver the
 *               NetBSD-VAX rail's SIMH target uses -- no protocol code runs in
 *               the hardware interrupt.
 *   SS14 tx     if_transmit_lock(ifp, m) on a pre-built mbuf (dhost/shost/
 *               ethertype already set by the codec) -- the exact shape
 *               bridge(4) uses to forward an already-Ethernet-framed mbuf,
 *               bypassing if_output/ARP entirely.
 *   SS14 mcast  if_mcast_op(ifp, SIOCADDMULTI/SIOCDELMULTI, sockaddr_dl) --
 *               "Use this, not if_ioctl, for the multicast commands. May
 *               sleep." (net/if.c). qe(4)'s hardware filter holds at most 12
 *               explicit addresses before falling back to IFF_ALLMULTI ->
 *               IFF_PROMISC, which makes ether_input() skip pfil for frames
 *               not addressed to us -- flagged at the mc_add call site below.
 *   SS15/SS16   kthread(9) (KTHREAD_MPSAFE|KTHREAD_MUSTJOIN, so kthread_join
 *               is legal -- kern_kthread.c KASSERTs LP_MUSTJOIN) and
 *               callout(9) (CALLOUT_MPSAFE), exactly the primitives
 *               exec_kthread_t / exec_timer_t (exec_kbackend_netbsd.h) were
 *               already sized for.
 *   SS17        getnanotime()/getnanouptime(), converted with the same public,
 *               documented 17-NOV-1858 VMS epoch arithmetic vms_proctab.c
 *               already uses for JPI$_LOGINTIM (VMS_EPOCH_OFFSET_SEC) --
 *               CLAUDE.md Rule 8: the offset is publicly documented, not
 *               reverse-engineered, and this file does not invent a second
 *               constant that could drift from the one proctab.c carries.
 *   SS18        already real (printf(9), exec_kbackend_netbsd.h) -- untouched.
 *
 * CONTRACT RULE 1 (exec_kbackend.h): the pfil hook below MAY ONLY copy the
 * frame into ITS OWN transient stack buffer and call rx_cb once -- rx_cb (the
 * core's own copy-into-pool/enqueue/wake, vms_pe.c) is what actually owns and
 * retains the bytes. The hook never allocates, never sleeps, never touches the
 * fork mutex, and runs no protocol code.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): OVMX glue over PUBLIC, documented NetBSD KPIs
 * (pfil(9), if_mcast_op, if_transmit_lock, kthread(9), callout(9), the mbuf(9)
 * copy routines). No NetBSD or VSI/HPE source is copied; every KPI cited above
 * is read from the public NetBSD kernel headers to confirm its shape, not
 * transcribed from an implementation.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/callout.h>
#include <sys/kthread.h>
#include <sys/time.h>            /* getnanotime / getnanouptime (FC-P0.4) */
#include <sys/mbuf.h>
#include <sys/socket.h>          /* AF_LINK */
#include <sys/sockio.h>          /* SIOCADDMULTI / SIOCDELMULTI */
#include <net/if.h>
#include <net/if_dl.h>           /* struct sockaddr_dl, LLADDR/CLLADDR */
#include <net/if_ether.h>        /* struct ether_header, ETHER_* */
#include <net/pfil.h>

/*
 * "exec_kbackend.h" ONLY -- deliberately NOT "vms_internal.h", following the
 * vms_socket_netbsd.c / vms_blockdev_netbsd.c precedent. vms_internal.h pulls
 * exec_rbtree.h (the lock manager's intrusive tree), whose macro-based
 * rb_left/rb_right collide with <uvm/uvm_extern.h>'s (pulled transitively by
 * <net/if.h> -> <net/pfil.h> here) OWN <sys/rbtree.h> use in the x86 pmap
 * headers -- the exact collision exec_kbackend_netbsd.h's SS10 arena comment
 * documents for vms_lnm_arena_netbsd.c. This TU needs no vms_internal.h
 * symbol: EXEC_SS_NOSUCHDEV is defined in exec_kbackend.h itself, and the
 * EXEC_SS_NOSUCHDEV == SS__NOSUCHDEV single-lineage assertion already lives in
 * the substrate-neutral src/kernel-core/vms_pe.c, which this NetBSD module
 * also builds (src/kernel-netbsd/Makefile SRCS) -- asserting it a second time
 * here would be redundant, not additional protection.
 */
#include "exec_kbackend.h"       /* the SS14..SS18 declarations + shared types */

/*
 * ---------------------------------------------------------------------------
 * SS14 LAN port -- one node, one cluster port (exec_kbackend.h SS14: "these
 * ops name no handle"), so its whole state is these file-scope statics,
 * mutated only from process context (exec_lan_open/close/mc_add/mc_del,
 * always called from the fork thread's setup path per the contract) and READ
 * from the pfil receive hook (Contract Rule 1's context).
 * ---------------------------------------------------------------------------
 */

/* A stack buffer, never heap: Contract Rule 1 forbids allocation in rx
 * context. ETHER_MAX_LEN (1518) is the standard Ethernet frame ceiling; a
 * little headroom above it costs nothing and never binds a real limit (the
 * codec/MTU checks the real ceiling elsewhere). */
#define OVMX_LAN_RXBUF_LEN 1600

static struct ifnet      *g_lan_ifp;         /* NULL: no port open */
static uint16_t            g_lan_ethertype;   /* host order, per the contract */
static exec_lan_rx_cb_t    g_lan_rx_cb;
static void                *g_lan_rx_ctx;
static int                  g_lan_hook_active; /* pfil_add_hook succeeded */

/*
 * scap_pfil_rx - the pfil(9) IN hook bound to ifp->if_pfil in exec_lan_open.
 * Runs at IPL_SOFTNET (FC-P0.3's finding for qe(4)/if_percpuq). Filters on
 * ethertype; on a match it copies the frame into a TRANSIENT stack buffer
 * (never the mbuf itself -- Contract Rule 1 requires the callback's argument
 * be a buffer the substrate owns only for the call's duration) and hands it to
 * the core's rx_cb, then consumes the mbuf so it never reaches IP/ARP
 * dispatch. A non-matching frame is left untouched and returned to normal
 * dispatch, so ETH0: keeps serving TCP/IP unmodified.
 */
static int
scap_pfil_rx(void *arg, struct mbuf **mp, struct ifnet *ifp, int dir)
{
	struct mbuf *m;
	struct ether_header eh;
	uint8_t frame[OVMX_LAN_RXBUF_LEN];
	int len;

	(void)arg;
	(void)ifp;

	if (dir != PFIL_IN || g_lan_rx_cb == NULL)
		return 0;             /* not our direction, or port not armed */

	m = *mp;
	if (m == NULL || (m->m_flags & M_PKTHDR) == 0 ||
	    m->m_pkthdr.len < (int)sizeof(eh))
		return 0;             /* too short to be a claimable frame */

	m_copydata(m, 0, sizeof(eh), &eh);
	if (ntohs(eh.ether_type) != g_lan_ethertype)
		return 0;             /* not our ethertype: pass through */

	len = m->m_pkthdr.len;
	if (len > (int)sizeof(frame))
		len = (int)sizeof(frame);   /* clamp; never over-reads the mbuf */
	m_copydata(m, 0, len, frame);

	g_lan_rx_cb(g_lan_rx_ctx, frame, (uint32_t)len);

	m_freem(m);
	*mp = NULL;                   /* consumed: caller must not touch it again */
	return 1;
}

int
exec_lan_open(const char *ifname, uint16_t ethertype, exec_lan_rx_cb_t rx_cb,
    void *ctx)
{
	struct ifnet *ifp;

	if (g_lan_ifp != NULL)
		exec_lan_close();     /* idempotent re-open: one port, always */

	ifp = ifunit(ifname);
	if (ifp == NULL || ifp->if_pfil == NULL)
		return (int)EXEC_SS_NOSUCHDEV;   /* the honest "no NIC" case */

	/* Set the hook's inputs BEFORE attaching it: scap_pfil_rx can run the
	 * instant pfil_add_hook returns (another CPU may already be draining
	 * this interface's percpuq), so g_lan_rx_cb must never be read stale. */
	g_lan_ethertype = ethertype;
	g_lan_rx_cb = rx_cb;
	g_lan_rx_ctx = ctx;

	if (pfil_add_hook(scap_pfil_rx, NULL, PFIL_IN, ifp->if_pfil) != 0) {
		g_lan_rx_cb = NULL;
		g_lan_rx_ctx = NULL;
		return (int)EXEC_SS_NOSUCHDEV;
	}

	g_lan_ifp = ifp;
	g_lan_hook_active = 1;
	return 0;
}

void
exec_lan_close(void)
{
	if (!g_lan_hook_active)
		return;                /* nothing open: idempotent */

	pfil_remove_hook(scap_pfil_rx, NULL, PFIL_IN, g_lan_ifp->if_pfil);

	g_lan_hook_active = 0;
	g_lan_ifp = NULL;
	g_lan_rx_cb = NULL;
	g_lan_rx_ctx = NULL;
	g_lan_ethertype = 0;
}

int
exec_lan_xmit(const uint8_t *frame, uint32_t len)
{
	struct mbuf *m;

	if (g_lan_ifp == NULL)
		return (int)EXEC_SS_NOSUCHDEV;

	m = m_gethdr(M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return (int)EXEC_SS_NOSUCHDEV;

	/* m_copyback grows the chain (additional mbufs/clusters) as needed and
	 * returns void on this KPI -- it cannot report a mid-copy allocation
	 * failure other than by leaving a short chain, so m_pkthdr.len is set
	 * from the caller's `len`, matching what every in-tree m_copyback
	 * caller building a fresh packet does. */
	m->m_pkthdr.len = 0;
	m_copyback(m, 0, (int)len, frame);
	m->m_pkthdr.len = (int)len;

	/* if_transmit_lock: the uniform if_transmit entry point (bridge(4)'s
	 * shape, docs/research-netbsd-lan-binding.md SS3) -- no ARP/route/
	 * sockaddr work, the frame's own dhost/shost/ethertype (already built
	 * by the codec) go straight to the wire. Consumes `m` either way. */
	if (if_transmit_lock(g_lan_ifp, m) != 0)
		return (int)EXEC_SS_NOSUCHDEV;
	return 0;
}

/*
 * scap_mcast_op - shared body of exec_lan_mc_add/del: build the sockaddr_dl
 * if_mcast_op(9) wants and issue the ioctl-shaped (but sleep-capable, no
 * if_ioctl call site of our own) request.
 */
static int
scap_mcast_op(unsigned long cmd, const uint8_t mac[6])
{
	struct sockaddr_dl sdl;

	if (g_lan_ifp == NULL)
		return (int)EXEC_SS_NOSUCHDEV;

	memset(&sdl, 0, sizeof(sdl));
	sdl.sdl_len = sizeof(sdl);
	sdl.sdl_family = AF_LINK;
	sdl.sdl_alen = ETHER_ADDR_LEN;
	memcpy(LLADDR(&sdl), mac, ETHER_ADDR_LEN);

	if (if_mcast_op(g_lan_ifp, cmd, (const struct sockaddr *)&sdl) != 0)
		return (int)EXEC_SS_NOSUCHDEV;
	return 0;
}

int
exec_lan_mc_add(const uint8_t mac[6])
{
	/* qe(4)'s hardware filter holds at most 12 explicit multicast
	 * addresses; past that (or on any range entry) it falls back to
	 * IFF_ALLMULTI -> IFF_PROMISC, and IFF_PROMISC is exactly the
	 * condition that makes ether_input() set M_PROMISC and SKIP the pfil
	 * hook above for frames not addressed to us (docs/research-netbsd-
	 * lan-binding.md SS4). The cluster join adds exactly one group, well
	 * under the limit; a FUTURE second ETH0: multicast consumer pushing
	 * the combined count over 12 would make cluster rx go silently dark. */
	return scap_mcast_op(SIOCADDMULTI, mac);
}

int
exec_lan_mc_del(const uint8_t mac[6])
{
	return scap_mcast_op(SIOCDELMULTI, mac);
}

int
exec_lan_hwaddr(uint8_t out[6])
{
	if (g_lan_ifp == NULL || g_lan_ifp->if_sadl == NULL)
		return (int)EXEC_SS_NOSUCHDEV;   /* out left untouched (INV-6) */

	memcpy(out, CLLADDR(g_lan_ifp->if_sadl), ETHER_ADDR_LEN);
	return 0;
}

int
exec_lan_mtu(uint32_t *out)
{
	if (g_lan_ifp == NULL)
		return (int)EXEC_SS_NOSUCHDEV;

	*out = (uint32_t)g_lan_ifp->if_mtu;
	return 0;
}

int
exec_lan_link_up(int *out)
{
	if (g_lan_ifp == NULL)
		return (int)EXEC_SS_NOSUCHDEV;   /* "not open", distinct from "down" */

	/* LINK_STATE_UNKNOWN (no media-sense driver support, e.g. many
	 * virtual/QEMU NICs) reads as up, matching netif_carrier_ok's Linux
	 * default of "up unless explicitly told otherwise" -- only an
	 * affirmative LINK_STATE_DOWN reports the link down. */
	*out = (g_lan_ifp->if_link_state != LINK_STATE_DOWN);
	return 0;
}

/*
 * ---------------------------------------------------------------------------
 * SS15 cluster fork context -- ONE kthread per node. exec_kthread_t's shape
 * (exec_kbackend_netbsd.h) is fixed: {lwp, mtx, cv, stop}. The mtx/cv pair
 * exists so exec_kthread_stop's mutation of `stop` and exec_kthread_should_
 * stop's read of it follow the exec_kbackend.h cv-wait contract exactly
 * (lock held across both the mutation-and-signal and the read).
 * ---------------------------------------------------------------------------
 */

/* NetBSD's kthread(9) entry point returns void, not int (contract families
 * pass an `int (*)(void *)` -- the Linux kthread_run() shape). This trampoline
 * bridges the two: it owns a small heap parcel {fn, arg} (freed before fn
 * runs, so nothing outlives the thread's own stack) and calls kthread_exit()
 * on return, which is mandatory -- a kthread(9) entry function must not just
 * `return`. */
struct exec_kthread_trampoline_arg {
	int (*fn)(void *);
	void *arg;
};

static void
exec_kthread_trampoline(void *raw)
{
	struct exec_kthread_trampoline_arg *ta = raw;
	int (*fn)(void *) = ta->fn;
	void *arg = ta->arg;

	exec_free(ta);
	(void)fn(arg);
	kthread_exit(0);
}

int
exec_kthread_create(exec_kthread_t *t, int (*fn)(void *), void *arg,
    const char *name)
{
	struct exec_kthread_trampoline_arg *ta;
	int error;

	memset(t, 0, sizeof(*t));
	mutex_init(&t->mtx, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&t->cv, "vmscfstop");
	t->stop = 0;

	ta = exec_zalloc(sizeof(*ta));
	if (ta == NULL) {
		cv_destroy(&t->cv);
		mutex_destroy(&t->mtx);
		return (int)EXEC_SS_NOSUCHDEV;
	}
	ta->fn = fn;
	ta->arg = arg;

	/* KTHREAD_MUSTJOIN: mandatory for kthread_join(9) to be legal on the
	 * resulting lwp (kern_kthread.c KASSERTs LP_MUSTJOIN); KTHREAD_MPSAFE:
	 * this thread never touches KERNEL_LOCK, matching the Linux twin's
	 * ordinary kthread. */
	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE | KTHREAD_MUSTJOIN,
	    NULL, exec_kthread_trampoline, ta, &t->lwp, "%s", name);
	if (error != 0) {
		exec_free(ta);
		cv_destroy(&t->cv);
		mutex_destroy(&t->mtx);
		t->lwp = NULL;
		return (int)EXEC_SS_NOSUCHDEV;
	}
	return 0;
}

void
exec_kthread_stop(exec_kthread_t *t)
{
	if (t == NULL || t->lwp == NULL)
		return;                /* no thread exists: idempotent no-op */

	mutex_enter(&t->mtx);
	t->stop = 1;
	cv_broadcast(&t->cv);
	mutex_exit(&t->mtx);

	kthread_join(t->lwp);         /* waits for kthread_exit() to run */

	cv_destroy(&t->cv);
	mutex_destroy(&t->mtx);
	t->lwp = NULL;                 /* terminal: a second _stop is a no-op */
}

int
exec_kthread_should_stop(exec_kthread_t *t)
{
	int stop;

	if (t == NULL || t->lwp == NULL)
		return 1;              /* no thread exists: "stop" is the truth */

	mutex_enter(&t->mtx);
	stop = t->stop;
	mutex_exit(&t->mtx);
	return stop;
}

/*
 * ---------------------------------------------------------------------------
 * SS16 timers -- exec_timer_t = {callout, cb, ctx} (exec_kbackend_netbsd.h).
 * ---------------------------------------------------------------------------
 */

static void
exec_timer_trampoline(void *arg)
{
	exec_timer_t *t = arg;

	if (t->cb != NULL)
		t->cb(t->ctx);         /* Contract Rule 2: post-and-wake ONLY */
}

void
exec_timer_init(exec_timer_t *t, void (*cb)(void *), void *ctx)
{
	callout_init(&t->co, CALLOUT_MPSAFE);
	callout_setfunc(&t->co, exec_timer_trampoline, t);
	t->cb = cb;
	t->ctx = ctx;
}

void
exec_timer_arm(exec_timer_t *t, uint32_t ms)
{
	int ticks = mstohz(ms);

	if (ticks < 1)
		ticks = 1;             /* a sub-tick `ms' must still fire, not wait forever */
	callout_schedule(&t->co, ticks);   /* re-arming an armed timer moves it */
}

void
exec_timer_cancel(exec_timer_t *t)
{
	callout_halt(&t->co, NULL);   /* waits out an in-flight callback */
}

void
exec_timer_destroy(exec_timer_t *t)
{
	callout_destroy(&t->co);      /* mandatory on this substrate */
}

/*
 * ---------------------------------------------------------------------------
 * SS17 time. Same public, documented VMS epoch arithmetic
 * src/kernel-core/vms_proctab.c already carries for JPI$_LOGINTIM (CLAUDE.md
 * Rule 8: transcribed from the published epoch, not reverse-engineered, and
 * kept as ONE constant per TU rather than pulled through a shared header
 * proctab.c itself does not expose -- the same "define it locally, same
 * value, same comment" precedent that file follows).
 * ---------------------------------------------------------------------------
 */

/* Seconds between the VMS system-time base (17-NOV-1858 00:00:00, the
 * Smithsonian Modified Julian Date epoch) and the Unix epoch (01-JAN-1970
 * 00:00:00): 40587 days * 86400. */
#define OVMX_VMS_EPOCH_OFFSET_SEC   3506716800ULL
#define OVMX_VMS_TIME_UNITS_PER_SEC 10000000ULL   /* 100ns units */

uint64_t
exec_time_now_vms(void)
{
	struct timespec ts;

	getnanotime(&ts);
	return (uint64_t)ts.tv_sec * OVMX_VMS_TIME_UNITS_PER_SEC
	     + (uint64_t)ts.tv_nsec / 100ULL
	     + OVMX_VMS_EPOCH_OFFSET_SEC * OVMX_VMS_TIME_UNITS_PER_SEC;
}

uint64_t
exec_ticks_ms(void)
{
	struct timespec ts;

	getnanouptime(&ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* SS18 exec_console_printf is a macro over printf(9) in exec_kbackend_netbsd.h
 * -- already the real binding, so there is no body here. */
