/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_recnx_fsm.c - the RECNXINTERVAL/TIMVCFAIL reconnect loop and the
 * last-gasp emission (FC-P3.6).
 *
 * The contract, the grounding and the three rules this file must get right are
 * in vms_cnxman_recnx_fsm.h. The CSB ladder it drives is vms_cnxman_csb.c.
 *
 * PURE TU (CI gate rule 4): no seam primitive, no allocation, no clock but
 * ops->now_ms. Every deadline is injected, so a unit test drives a whole
 * 20-second reconnect window in microseconds.
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"

/* ==========================================================================
 * Time: wrap-safe, injected
 * ========================================================================== */

/*
 * The injected clock is a 32-bit millisecond counter (struct cnxman_ops), so it
 * rolls over about every 49.7 days -- well inside a VMS uptime. Comparing with
 * `now >= deadline` would, at a rollover, make every deadline look unreachable
 * and freeze the reconnect apparatus for weeks. The signed-difference form is
 * correct across the wrap and is also what makes a clock that appears to run
 * BACKWARDS fire nothing at all, which is the honest response to a bad sample.
 */
static int recnx_reached(uint32_t now, uint32_t deadline)
{
	return (int32_t)(now - deadline) >= 0;
}

static uint32_t recnx_now(const struct cnxman_recnx *r)
{
	if (r->ops != NULL && r->ops->now_ms != NULL)
		return r->ops->now_ms(r->ops->ctx);
	return 0u;
}

static void recnx_arm(struct cnxman_recnx *r)
{
	if (r->ops != NULL && r->ops->arm_timer != NULL)
		r->ops->arm_timer(r->ops->ctx, CNXMAN_TIMER_RECNX, 0u,
				  CNXMAN_RECNX_ATTEMPT_MS);
}

/* ==========================================================================
 * The p. 7-30 arithmetic
 * ========================================================================== */

uint32_t cnxman_recnx_lan_remote_secs(int v55_or_later,
				      uint32_t remote_timvcfail)
{
	/* p. 7-30: "fixed at 16 prior to Version 5.5. But starting with
	 * Version 5.5, the remote system supplies the value of its TIMVCFAIL
	 * parameter." */
	return v55_or_later ? remote_timvcfail : CNXMAN_RECNX_LAN_PRE_V55_SECS;
}

uint32_t cnxman_recnx_period_secs(uint32_t local_recnxinterval,
				  uint32_t remote_port_secs)
{
	/* p. 7-30: "the maximum of the local value for RECNXINTERVAL and a port
	 * dependent number supplied by the remote Connection Manager." */
	return (local_recnxinterval >= remote_port_secs) ? local_recnxinterval
							: remote_port_secs;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void cnxman_recnx_init(struct cnxman_recnx *r, struct vms_cluster *cl,
		       const struct cnxman_ops *ops)
{
	if (r == NULL)
		return;
	r->cl = cl;
	r->ops = ops;
	r->running = 0u;
	r->departing = 0u;
	r->pad[0] = 0u;
	r->pad[1] = 0u;
	r->ticks = 0u;
	r->attempts_issued = 0u;
	r->proposals = 0u;
	r->last_gasps = 0u;
}

void cnxman_recnx_start(struct cnxman_recnx *r)
{
	if (r == NULL || r->running)
		return;
	r->running = 1u;
	recnx_arm(r);
}

void cnxman_recnx_stop(struct cnxman_recnx *r)
{
	if (r == NULL || !r->running)
		return;
	r->running = 0u;
	if (r->ops != NULL && r->ops->cancel_timer != NULL)
		r->ops->cancel_timer(r->ops->ctx, CNXMAN_TIMER_RECNX, 0u);
}

/* ==========================================================================
 * Connectivity events
 * ========================================================================== */

enum cnxman_csb_action cnxman_recnx_connectivity_gained(struct cnxman_recnx *r,
							struct vms_csb *csb)
{
	if (r == NULL || r->cl == NULL)
		return CNXMAN_CSB_ACT_NONE;
	return cnxman_csb_dispatch(&r->cl->club, csb,
				   CNXMAN_CSB_EV_CONN_OPEN, r->ops);
}

enum cnxman_csb_action cnxman_recnx_connectivity_lost(struct cnxman_recnx *r,
						      struct vms_csb *csb,
						      int announced_departure)
{
	enum cnxman_csb_action act;

	if (r == NULL || r->cl == NULL)
		return CNXMAN_CSB_ACT_NONE;

	/* p. 7-29 vs p. 7-30 -- the whole difference between "it told us it was
	 * going" and "we cannot see it at the moment". */
	act = cnxman_csb_dispatch(&r->cl->club, csb,
				  announced_departure
					  ? CNXMAN_CSB_EV_LAST_GASP
					  : CNXMAN_CSB_EV_CONN_LOST,
				  r->ops);

	if (act == CNXMAN_CSB_ACT_PROPOSE_TRANSITION)
		r->proposals++;
	return act;
}

/* ==========================================================================
 * The once-a-second beat
 * ========================================================================== */

/* Is this CSB inside a reconnect window? Only these three states have a live
 * deadline (pp. 7-24: WAIT is the timeout, RECONNECT our attempt, REACCEPT the
 * peer's). */
static int recnx_in_window(const struct vms_csb *csb)
{
	return csb->state == (uint8_t)VMS_CNXMAN_CSB_WAIT ||
	       csb->state == (uint8_t)VMS_CNXMAN_CSB_RECONNECT ||
	       csb->state == (uint8_t)VMS_CNXMAN_CSB_REACCEPT;
}

static void recnx_emit(struct cnxman_recnx_rec *out, uint32_t max, uint32_t *n,
		       uint32_t index, enum cnxman_csb_action act)
{
	if (out == NULL || *n >= max)
		return;
	out[*n].csb_index = index;
	out[*n].action = (uint8_t)act;
	out[*n].pad[0] = 0u;
	out[*n].pad[1] = 0u;
	out[*n].pad[2] = 0u;
	(*n)++;
}

/*
 * One CSB's second. Expiry is tested BEFORE the beat: once the period is over,
 * p. 7-30 is done attempting and the question becomes whether to propose.
 */
static enum cnxman_csb_action recnx_tick_one(struct cnxman_recnx *r,
					     struct vms_club *club,
					     struct vms_csb *csb, uint32_t now)
{
	if (recnx_reached(now, csb->deadline_ms))
		return cnxman_csb_dispatch(club, csb,
					   CNXMAN_CSB_EV_RECNX_EXPIRED, r->ops);

	/* In REACCEPT the PEER is driving the reconnect and we are the
	 * accepting side (p. 7-24), so we do not also attempt our own; the
	 * deadline set at loss time keeps running underneath. */
	if (csb->state == (uint8_t)VMS_CNXMAN_CSB_REACCEPT)
		return CNXMAN_CSB_ACT_NONE;

	if (recnx_reached(now, csb->next_attempt_ms))
		return cnxman_csb_dispatch(club, csb,
					   CNXMAN_CSB_EV_RECNX_ATTEMPT, r->ops);

	return CNXMAN_CSB_ACT_NONE;
}

uint32_t cnxman_recnx_tick(struct cnxman_recnx *r,
			   struct cnxman_recnx_rec *out, uint32_t max)
{
	struct vms_club *club;
	uint32_t i, n = 0u, now;

	if (r == NULL || r->cl == NULL)
		return 0u;

	/* A node that has emitted its last gasp is on its way out: it runs no
	 * reconnect apparatus and proposes no transition for the cluster it is
	 * leaving (p. 7-49 -- from the SHUTDOWN flag on, it services others and
	 * initiates nothing of its own). */
	if (r->departing)
		return 0u;

	club = &r->cl->club;
	now = recnx_now(r);
	r->ticks++;

	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];
		enum cnxman_csb_action act;

		if (!csb->in_use || !recnx_in_window(csb))
			continue;

		act = recnx_tick_one(r, club, csb, now);
		if (act == CNXMAN_CSB_ACT_NONE)
			continue;
		if (act == CNXMAN_CSB_ACT_RECONNECT)
			r->attempts_issued++;
		else if (act == CNXMAN_CSB_ACT_PROPOSE_TRANSITION)
			r->proposals++;
		recnx_emit(out, max, &n, i, act);
	}

	if (r->running)
		recnx_arm(r);
	return n;
}

/* ==========================================================================
 * Leaving: the last gasp
 * ========================================================================== */

/* Is there anybody out there to tell? p. 7-29 sends the datagram "to each of
 * the other VAX systems in the cluster"; with no other member there is nobody,
 * and a node that announces a departure from a cluster it never joined is
 * asserting a membership it does not have. */
static int recnx_have_peers(const struct vms_club *club)
{
	uint32_t i;

	for (i = 0; i < club->n_csb; i++) {
		const struct vms_csb *csb = &club->csb[i];

		if (!csb->in_use || (csb->flags & VMS_CSB_F_LOCAL) != 0u)
			continue;
		if ((csb->flags & (VMS_CSB_F_MEMBER | VMS_CSB_F_SELECTED)) != 0u)
			return 1;
	}
	return 0;
}

uint32_t cnxman_recnx_shutdown(struct cnxman_recnx *r,
			       struct cnxman_recnx_rec *out, uint32_t max)
{
	struct vms_club *club;
	struct vms_csb *local;
	uint32_t n = 0u;

	if (r == NULL || r->cl == NULL)
		return 0u;

	club = &r->cl->club;

	/* p. 7-49: the SHUTDOWN flag goes in the CLUB and in this system's own
	 * CSB, in that order, before anything is told to anybody. */
	club->shutdown = 1u;
	local = cnxman_club_local(club);
	if (local != NULL)
		cnxman_csb_set_flags(local, VMS_CSB_F_SHUTDOWN);

	/* A departing node reconfigures nothing: from here the once-a-second
	 * beat may still count seconds, but it proposes no transition. */
	r->departing = 1u;

	if (!recnx_have_peers(club))
		return 0u;

	r->last_gasps++;
	if (r->ops != NULL && r->ops->log != NULL)
		r->ops->log(r->ops->ctx,
			    "%CNXMAN, leaving the cluster, sending last gasp");
	recnx_emit(out, max, &n,
		   (local != NULL) ? cnxman_club_csb_index(club, local)
				   : (uint32_t)VMS_CLUB_MAX_CSB,
		   CNXMAN_CSB_ACT_LAST_GASP);
	return n;
}
