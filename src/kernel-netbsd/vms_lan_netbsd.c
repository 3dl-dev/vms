/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_lan_netbsd.c - the NetBSD realization of the cluster seam, families
 * SS14..SS18 of exec_kbackend.h (FC-P0.1; design
 * docs/design-faithful-cluster-executive.md SS3.2.1/SS3.2.2).
 *
 * This is the NetBSD TWIN of the Linux binding that lives static-inline in
 * exec_kbackend_linux.h. It follows the vms_socket_netbsd.c / vms_blockdev_
 * netbsd.c pattern: exec_kbackend_netbsd.h carries only the declarations (the
 * substrate-neutrality anchor -- every signature is expressible in NetBSD terms)
 * and the bodies live here, where the link-layer, kthread(9) and callout(9) KPIs
 * belong.
 *
 * SCOPE (FC-P0.1 = CONTRACT-ONLY STUBS, compiled, never run). FC-P0.1 freezes
 * the contract and proves BOTH kmods build against it. The real binding is two
 * items away and deliberately so:
 *
 *   FC-P0.3  a recorded spike on the rail's own NetBSD tree: which link-layer
 *            receive hook exists there (pfil(9) on ifp->if_pfil, which
 *            ether_input() runs before ethertype dispatch on NetBSD >= 8, vs
 *            interposing ifp->if_input the way bridge(4)/agr(4) do), at which
 *            IPL the VAX qe/xq drivers deliver input, and that if_transmit
 *            accepts a pre-built Ethernet frame.
 *   FC-P0.4  the real binding + the rung-3 substrate contract test on the
 *            NetBSD-VAX rail (SIMH tap): rx loopback, multicast join, timer
 *            post-and-wake, kthread start/stop, exec_time monotonicity.
 *
 * Guessing the receive hook before FC-P0.3 measures it is exactly the class of
 * mistake this project's rules exist to prevent, so these bodies FAIL HONESTLY
 * with SS$_NOSUCHDEV (EXEC_SS_NOSUCHDEV): they open nothing, register nothing,
 * transmit nothing and fabricate nothing (INV-6 / CLAUDE.md Rule 9 -- an
 * executive that cannot do it says so; it never simulates a port). Because
 * exec_lan_open() never succeeds, vms_pe.c never brings PEA0: up, so no timer is
 * ever armed and no fork thread is ever created on this substrate today.
 *
 * CLEAN-ROOM (CLAUDE.md Rule 8): OVMX glue over PUBLIC, documented NetBSD KPIs.
 * No NetBSD or VSI/HPE source is copied.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/callout.h>
#include <sys/kthread.h>
#include <sys/time.h>            /* getnanotime / getnanouptime (FC-P0.4) */

#include "vms_internal.h"        /* fixed-width types + the SS$_ vocabulary */
#include "exec_kbackend.h"       /* the SS14..SS18 declarations + shared types */

/* One value, two spellings, asserted -- never two constants that happen to
 * agree today (the same single-lineage discipline vms_pe.c applies in the
 * core). */
__CTASSERT(EXEC_SS_NOSUCHDEV == SS__NOSUCHDEV);

/* ---- SS14 LAN port -------------------------------------------------------
 * FC-P0.4: dev-hook per the FC-P0.3 spike for open/close, if_transmit for
 * xmit, if_mcast_op(SIOCADDMULTI/SIOCDELMULTI) for multicast, and
 * CLLADDR(ifp->if_sadl) / ifp->if_mtu / ifp->if_link_state for the readouts. */

int
exec_lan_open(const char *ifname, uint16_t ethertype, exec_lan_rx_cb_t rx_cb,
    void *ctx)
{
	(void)ifname;
	(void)ethertype;
	(void)rx_cb;
	(void)ctx;
	return (int)EXEC_SS_NOSUCHDEV;
}

void
exec_lan_close(void)
{
	/* Nothing is open, so there is nothing to unhook. */
}

int
exec_lan_xmit(const uint8_t *frame, uint32_t len)
{
	(void)frame;
	(void)len;
	return (int)EXEC_SS_NOSUCHDEV;
}

int
exec_lan_mc_add(const uint8_t mac[6])
{
	(void)mac;
	return (int)EXEC_SS_NOSUCHDEV;
}

int
exec_lan_mc_del(const uint8_t mac[6])
{
	(void)mac;
	return (int)EXEC_SS_NOSUCHDEV;
}

int
exec_lan_hwaddr(uint8_t out[6])
{
	(void)out;   /* left untouched: never a fabricated source MAC */
	return (int)EXEC_SS_NOSUCHDEV;
}

int
exec_lan_mtu(uint32_t *out)
{
	(void)out;
	return (int)EXEC_SS_NOSUCHDEV;
}

int
exec_lan_link_up(int *out)
{
	(void)out;
	return (int)EXEC_SS_NOSUCHDEV;
}

/* ---- SS15 fork context ---------------------------------------------------
 * FC-P0.4: kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, fn, arg, &t->lwp,
 * "%s", name) after mutex_init/cv_init on the handle; _stop sets t->stop under
 * t->mtx, cv_broadcasts and kthread_join(t->lwp). */

int
exec_kthread_create(exec_kthread_t *t, int (*fn)(void *), void *arg,
    const char *name)
{
	(void)fn;
	(void)arg;
	(void)name;
	memset(t, 0, sizeof(*t));
	return (int)EXEC_SS_NOSUCHDEV;
}

void
exec_kthread_stop(exec_kthread_t *t)
{
	(void)t;   /* no thread was ever created */
}

int
exec_kthread_should_stop(exec_kthread_t *t)
{
	(void)t;
	return 1;  /* no thread exists: "stop" is the truthful answer */
}

/* ---- SS16 timers ---------------------------------------------------------
 * FC-P0.4: callout_init(&t->co, CALLOUT_MPSAFE) + callout_setfunc,
 * callout_schedule(&t->co, mstohz(ms)), callout_halt, callout_destroy. */

void
exec_timer_init(exec_timer_t *t, void (*cb)(void *), void *ctx)
{
	memset(t, 0, sizeof(*t));
	t->cb = cb;
	t->ctx = ctx;
}

void
exec_timer_arm(exec_timer_t *t, uint32_t ms)
{
	(void)t;
	(void)ms;
}

void
exec_timer_cancel(exec_timer_t *t)
{
	(void)t;
}

void
exec_timer_destroy(exec_timer_t *t)
{
	(void)t;
}

/* ---- SS17 time -----------------------------------------------------------
 * FC-P0.4: getnanotime() + the 17-NOV-1858 epoch offset / 100 for VMS absolute
 * time; getnanouptime() / 1000000 for the monotonic millisecond counter. Zero is
 * not a plausible VMS time, so a caller shipping this value would be visibly
 * wrong rather than subtly wrong. */

uint64_t
exec_time_now_vms(void)
{
	return 0;
}

uint64_t
exec_ticks_ms(void)
{
	return 0;
}

/* SS18 exec_console_printf is a macro over printf(9) in exec_kbackend_netbsd.h
 * -- already the real binding, so there is no body here. */
