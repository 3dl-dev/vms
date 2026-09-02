/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_recnx.c - the RECNXINTERVAL/TIMVCFAIL reconnect loop and the
 * last-gasp emission (FC-P3.6, test-ladder rung R1).
 *
 * PORTED FROM tests/vmsscs/test_scs_recnx.c -- the CASES, not the code. The
 * strawman daemon's version of this proof drove a userspace struct with a
 * uint64 millisecond argument threaded through every call; this one drives the
 * REAL executive CLUB/CSB model through struct cnxman_ops, whose clock is
 * injected (design SS3.9 rule 6), so the same twenty-second window is exercised
 * without a wire, a daemon or a boot.
 *
 * ONE CASE WAS DELIBERATELY NOT PORTED. The strawman's "FAIL-pre" arm flipped
 * behaviour on the environment variable OVMX_NO_RECNX_RECONNECT, which is a
 * Linux env gate deciding a VMS behaviour -- exactly the anti-pattern the
 * campaign banned after a shipped build ran the imitation path while only the
 * test flag ran the real one. The executive has no such switch and this file
 * asserts none. The fail-pre it was proving (a break dropping membership at
 * once) is instead proven positively: test_membership_held_across_the_window
 * shows the CSB stays a member for the whole period, which is false of any
 * implementation that drops on the first lost frame.
 *
 * GROUNDING. *VAXcluster Principles* (Davis 1993): the ten connectivity states
 * pp. 7-23/7-24; the once-a-second reconnect, the max(RECNXINTERVAL, remote
 * port value) period, the LAN remote value (16 before V5.5, the remote's
 * TIMVCFAIL from V5.5), the "do not presume the remote has left" rule and the
 * "if no other Connection Manager has already instituted a transition"
 * condition all p. 7-30; the last-gasp departure p. 7-29; the CLUSTER_SHUTDOWN
 * flags p. 7-49. Host-only transcript, page cites only (clean-room rule 8).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"

static struct vms_cluster    g_cl;
static struct cnxman_ops     g_ops;
static struct fake_cnx       g_fake;
static struct cnxman_recnx   g_recnx;

/*
 * A two-node cluster in the executive's own structures: this node plus one
 * remote connection manager whose CSB is OPEN and a committed member. Nothing
 * below hand-sets a connectivity state after this point -- every state the
 * tests observe is reached by dispatching a real event.
 */
static struct vms_csb *bed(uint16_t recnxinterval, uint32_t remote_port_secs,
			   int remote_port_valid)
{
	struct vms_csb *local, *peer;

	memset(&g_cl, 0, sizeof(g_cl));
	fake_ops_init(&g_ops, &g_fake);

	memcpy(g_cl.params.scsnode, "OVMX01", 6);
	g_cl.params.scsnode_len = 6;
	g_cl.params.scssystemid = 0x000004000103ull;
	g_cl.params.expected_votes = 2;
	g_cl.params.recnxinterval = recnxinterval;

	local = cnxman_club_init(&g_cl);
	cnxman_csb_set_flags(local, VMS_CSB_F_MEMBER | VMS_CSB_F_SELECTED);

	peer = cnxman_club_alloc_csb(&g_cl.club, 0x000004000101ull, 1);
	cnxman_csb_set_scsnode(peer, (const uint8_t *)"VAX1", 4);
	cnxman_csb_set_csid(peer, 0x00010001u);
	if (remote_port_valid)
		cnxman_csb_set_remote_port_secs(peer, remote_port_secs);

	cnxman_recnx_init(&g_recnx, &g_cl, &g_ops);
	cnxman_recnx_start(&g_recnx);

	/* Drive it to OPEN through the ladder, then let the cluster admit it --
	 * membership is a transition's decision, not a side effect of a
	 * connection (pp. 7-28/7-49). */
	(void)cnxman_csb_dispatch(&g_cl.club, peer, CNXMAN_CSB_EV_CONNECT_SENT,
				  &g_ops);
	(void)cnxman_recnx_connectivity_gained(&g_recnx, peer);
	cnxman_csb_set_flags(peer, VMS_CSB_F_MEMBER | VMS_CSB_F_SELECTED);
	(void)cnxman_club_recount_members(&g_cl.club);
	return peer;
}

/* ==========================================================================
 * 1. The p. 7-30 arithmetic, including the book's own worked example
 * ========================================================================== */
static void test_period_arithmetic(void)
{
	printf("[recnx] the period is max(local RECNXINTERVAL, remote value) (p. 7-30)\n");
	ct_check(cnxman_recnx_period_secs(20, 10) == 20,
		 "max(20,10) == 20 -- the p. 7-30 worked example");
	ct_check(cnxman_recnx_period_secs(10, 20) == 20, "max(10,20) == 20");
	ct_check(cnxman_recnx_period_secs(16, 10) == 16, "max(16,10) == 16");
	ct_check(cnxman_recnx_period_secs(5, 5) == 5, "max(5,5) == 5");
	ct_check(cnxman_recnx_period_secs(0, 0) == 0, "max(0,0) == 0");

	ct_check(cnxman_recnx_lan_remote_secs(0, 10) ==
		 CNXMAN_RECNX_LAN_PRE_V55_SECS,
		 "a pre-V5.5 LAN remote contributes the fixed value, not TIMVCFAIL");
	ct_check_eq_u32(CNXMAN_RECNX_LAN_PRE_V55_SECS, 16,
			"and that fixed value is 16 (p. 7-30)");
	ct_check(cnxman_recnx_lan_remote_secs(1, 10) == 10,
		 "from V5.5 the remote contributes its own TIMVCFAIL");
	ct_check(cnxman_recnx_period_secs(20, cnxman_recnx_lan_remote_secs(1, 10))
			 == 20,
		 "the full example (V5.5-1, local 20, remote TIMVCFAIL 10) -> 20 s");
	ct_check(cnxman_recnx_period_secs(10, cnxman_recnx_lan_remote_secs(0, 10))
			 == 16,
		 "and pre-V5.5 the remote's 16 can DOMINATE a smaller local value");
	ct_check_eq_u32(CNXMAN_RECNX_ATTEMPT_MS, 1000,
			"the cadence is once a second (p. 7-30)");
}

/* ==========================================================================
 * 2. THE CORE PROOF: a real breakage holds membership, beats once a second for
 *    the whole period, then proposes exactly one transition.
 * ========================================================================== */
static void test_cadence_and_membership_held(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[8];
	uint32_t t, n;
	uint32_t reconnects = 0, reconnect_at[8];
	uint32_t proposals = 0, proposed_at = 0;
	int membership_ever_dropped = 0;

	printf("[recnx] a breakage: 1/sec for max(RECNX,remote)=3 s, membership HELD\n");
	/* local RECNXINTERVAL 3, remote-supplied 2 -> period max(3,2) = 3 s. */
	peer = bed(3, 2, 1);
	ct_check(peer->state == (uint8_t)VMS_CNXMAN_CSB_OPEN &&
		 cnxman_csb_is_member(peer),
		 "precondition: the circuit is OPEN and the node is a member");
	ct_check_eq_u32(g_cl.club.cluster_nodes, 2,
			"and CLUSTER_NODES is 2 (this node + VAX1)");

	/* INJECT the breakage: not a last gasp. */
	g_fake.now_ms = 0;
	ct_check(cnxman_recnx_connectivity_lost(&g_recnx, peer, 0) ==
			 CNXMAN_CSB_ACT_NONE,
		 "a plain breakage proposes NOTHING -- it starts a wait");
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_WAIT,
			"the CSB enters WAIT: a timeout is in progress (p. 7-24)");
	ct_check(cnxman_csb_is_member(peer),
		 "MEMBERSHIP IS HELD across the breakage (p. 7-30)");
	ct_check_eq_u32(peer->deadline_ms, 3000,
			"the deadline is the max()-sized period, not RECNXINTERVAL alone");

	/* Drive the injected clock at 250 ms across the whole window. */
	for (t = 250; t <= 3000; t += 250) {
		g_fake.now_ms = t;
		n = cnxman_recnx_tick(&g_recnx, rec, 8);
		for (uint32_t i = 0; i < n; i++) {
			if (rec[i].action == CNXMAN_CSB_ACT_RECONNECT) {
				if (reconnects < 8)
					reconnect_at[reconnects] = t;
				reconnects++;
			} else if (rec[i].action ==
				   CNXMAN_CSB_ACT_PROPOSE_TRANSITION) {
				proposals++;
				proposed_at = t;
			}
		}
		if (proposals == 0 && !cnxman_csb_is_member(peer))
			membership_ever_dropped = 1;
	}

	ct_check_eq_u32(reconnects, 2,
			"exactly 2 attempts fired inside a 3 s window");
	ct_check(reconnects == 2 && reconnect_at[0] == 1000 &&
			 reconnect_at[1] == 2000,
		 "they landed at t=1000 and t=2000 -- once a second, never faster");
	ct_check_eq_u32(peer->attempts, 2, "the CSB counted both");
	ct_check_eq_u32(g_recnx.attempts_issued, 2, "so did the FSM");
	ct_check(!membership_ever_dropped,
		 "membership was HELD for the entire reconnect window");
	ct_check(proposals == 1 && proposed_at == 3000,
		 "at expiry (t=3000) the local CM proposes ONE transition (p. 7-30)");
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_DISCONNECT,
			"and the CSB leaves the reconnect states");
	ct_check_eq_u32(peer->transitions_proposed, 1,
			"exactly one transition is charged to this CSB");
	ct_check(cnxman_csb_is_member(peer) == 0,
		 "only NOW is our MEMBER view of the peer cleared");
	ct_check(g_fake.timers_armed >= 12,
		 "the once-a-second timer re-armed itself on every tick");
	ct_check_eq_u32(g_fake.last_arm_ms, CNXMAN_RECNX_ATTEMPT_MS,
			"and always for one second");
	ct_check_eq_u32(g_fake.last_arm_which, CNXMAN_TIMER_RECNX,
			"on the connection manager's RECNX timer");
}

/* The state the tick observes, for a reader who wants the ladder walked. */
static void test_window_walks_wait_and_reconnect(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] the window alternates WAIT <-> RECONNECT (pp. 7-24)\n");
	peer = bed(5, 0, 0);
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_WAIT, "lost -> WAIT");

	g_fake.now_ms = 1000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1, "one attempt");
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_RECONNECT,
			"the attempt is in progress -> RECONNECT");

	(void)cnxman_csb_dispatch(&g_cl.club, peer, CNXMAN_CSB_EV_RECNX_FAILED,
				  &g_ops);
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_WAIT,
			"it failed -> back to WAIT, and it will be repeated");
	ct_check(cnxman_csb_is_member(peer), "still a held member");

	g_fake.now_ms = 2000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1,
			"the next second fires the next attempt");
	ct_check_eq_u32(peer->attempts, 2, "two attempts so far");
}

/* p. 7-24 REACCEPT: the peer's reconnect arrives while ours is outstanding. */
static void test_peer_driven_reaccept(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] the peer's reconnect request -> REACCEPT (p. 7-24)\n");
	peer = bed(5, 0, 0);
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	g_fake.now_ms = 1000;
	(void)cnxman_recnx_tick(&g_recnx, rec, 4);

	(void)cnxman_csb_dispatch(&g_cl.club, peer, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g_ops);
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_REACCEPT,
			"we are accepting THEIR reconnect");

	/* While they drive it we do not also attempt -- but the p. 7-30
	 * deadline keeps running underneath. */
	g_fake.now_ms = 2000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"no attempt of our own while REACCEPT is in progress");
	ct_check_eq_u32(peer->attempts, 1, "so the attempt count does not move");

	g_fake.now_ms = 5000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1,
			"but the deadline still expires underneath it");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_PROPOSE_TRANSITION,
			"and expiry proposes the transition");
}

/* ==========================================================================
 * 3. Recovery: the circuit comes back before the period runs out
 * ========================================================================== */
static void test_recovers_without_membership_loss(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] restored mid-window -> OPEN, membership never lost\n");
	peer = bed(3, 0, 0);   /* period = max(3, nothing supplied) = 3 s */
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	ct_check_eq_u32(peer->deadline_ms, 3000,
			"an absent remote value leaves the local one standing alone");

	g_fake.now_ms = 1000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1, "one attempt at t=1000");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_RECONNECT, "a reconnect");
	ct_check(cnxman_csb_is_member(peer), "still a held member during the attempt");

	g_fake.now_ms = 1500;
	(void)cnxman_recnx_connectivity_gained(&g_recnx, peer);
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_OPEN, "restored -> OPEN");
	ct_check(cnxman_csb_is_member(peer), "membership was HELD throughout");
	ct_check_eq_u32(peer->reconnects, 1, "the recovery was counted");
	ct_check_eq_u32(cnxman_club_recount_members(&g_cl.club), 2,
			"and CLUSTER_NODES never dipped");

	g_fake.now_ms = 100000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"an OPEN circuit proposes nothing, however long the clock runs");
	ct_check_eq_u32(peer->transitions_proposed, 0, "no transition was ever proposed");
	ct_check_eq_u32(g_recnx.proposals, 0, "and the FSM proposed none");
}

/* ==========================================================================
 * 4. "if no other Connection Manager has already instituted a cluster state
 *    transition" (p. 7-30)
 * ========================================================================== */
static void test_peer_transition_suppresses_our_own(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] a transition already running suppresses ours (p. 7-30)\n");
	peer = bed(2, 0, 0);   /* period = 2 s */
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);

	/*
	 * The predicate reads the executive's REAL transition state -- what
	 * this node has actually observed on the wire -- not a parameter the
	 * caller passes in. That is the difference between an implementation
	 * that can be lied to and one that cannot.
	 */
	g_cl.club.transition_active = 1;
	g_fake.now_ms = 2000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"expiry with a transition already running proposes NOTHING");
	ct_check_eq_u32(peer->transitions_proposed, 0, "nothing is charged to us");
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_WAIT,
			"and the CSB stays in the window so the next tick can ask again");

	g_cl.club.transition_active = 0;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1,
			"with no transition running, the same expiry proposes one");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_PROPOSE_TRANSITION,
			"the action is a state-transition proposal");
	ct_check_eq_u32(peer->transitions_proposed, 1, "and it is charged to us");
}

/* ==========================================================================
 * 5. The last gasp, both directions
 * ========================================================================== */
static void test_inbound_last_gasp_skips_the_window(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] an inbound last gasp skips the reconnect window (p. 7-29)\n");
	peer = bed(20, 0, 0);
	ct_check(cnxman_csb_is_member(peer), "member before the last gasp");

	g_fake.now_ms = 100;
	ct_check_eq_u32(cnxman_recnx_connectivity_lost(&g_recnx, peer, 1),
			CNXMAN_CSB_ACT_PROPOSE_TRANSITION,
			"an announced departure proposes a transition immediately");
	ct_check_eq_u32(peer->state, VMS_CNXMAN_CSB_DISCONNECT,
			"last gasp -> DISCONNECT, never WAIT");
	ct_check(cnxman_csb_is_member(peer) == 0,
		 "and our MEMBER view is dropped at once -- it announced it was leaving");

	g_fake.now_ms = 5000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"no reconnect beat ever fires after a last gasp");
	ct_check_eq_u32(peer->attempts, 0, "and no attempt was made");
}

static void test_outbound_last_gasp_on_shutdown(void)
{
	struct vms_csb *peer, *local;
	struct cnxman_recnx_rec rec[4];
	uint32_t n;

	printf("[recnx] leaving: the CLUB/CSB SHUTDOWN flags + one last gasp (7-29/7-49)\n");
	peer = bed(20, 0, 0);
	local = cnxman_club_local(&g_cl.club);

	n = cnxman_recnx_shutdown(&g_recnx, rec, 4);
	ct_check_eq_u32(n, 1, "one last gasp is emitted for the cluster");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_LAST_GASP,
			"and the action says so");
	ct_check_eq_u32(rec[0].csb_index,
			cnxman_club_csb_index(&g_cl.club, local),
			"attributed to this system's own CSB");
	ct_check(g_cl.club.shutdown == 1,
		 "the SHUTDOWN flag is set in the CLUB (p. 7-49)");
	ct_check((local->flags & VMS_CSB_F_SHUTDOWN) != 0,
		 "and in the CSB this system associates with itself (p. 7-49)");
	ct_check_eq_u32(g_recnx.last_gasps, 1, "counted once");

	/* A departing node does not reconfigure the cluster it is leaving. */
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	g_fake.now_ms = 60000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"once departing, the tick proposes nothing");
	ct_check_eq_u32(g_recnx.proposals, 0, "and no proposal is charged to it");
}

static void test_standalone_node_announces_nothing(void)
{
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] a node with no cluster members announces no departure\n");
	memset(&g_cl, 0, sizeof(g_cl));
	fake_ops_init(&g_ops, &g_fake);
	g_cl.params.scssystemid = 0x000004000103ull;
	g_cl.params.recnxinterval = 20;
	(void)cnxman_club_init(&g_cl);
	cnxman_recnx_init(&g_recnx, &g_cl, &g_ops);

	ct_check_eq_u32(cnxman_recnx_shutdown(&g_recnx, rec, 4), 0,
			"no members to tell -> no last gasp (INV-6: claim nothing)");
	ct_check(g_cl.club.shutdown == 1,
		 "the CLUB's own SHUTDOWN flag is still set");
	ct_check_eq_u32(g_recnx.last_gasps, 0, "and none was counted");
}

/* ==========================================================================
 * 6. Clock hazards -- the ones a real 49.7-day uptime produces
 * ========================================================================== */
static void test_clock_hazards(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] backwards and wrapping clocks\n");
	peer = bed(3, 0, 0);
	g_fake.now_ms = 10000;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	g_fake.now_ms = 5000;   /* apparently backwards */
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"a clock that appears to run backwards fires nothing");
	ct_check_eq_u32(peer->attempts, 0, "no attempt");
	ct_check_eq_u32(peer->transitions_proposed, 0, "and no premature proposal");

	/*
	 * The rollover. The ops clock is 32-bit milliseconds, so it wraps about
	 * every 49.7 days. A naive `now >= deadline` would freeze the whole
	 * reconnect apparatus for weeks at the wrap; the signed-difference form
	 * keeps working across it.
	 */
	peer = bed(3, 0, 0);
	g_fake.now_ms = 0xfffff000u;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	ct_check_eq_u32(peer->deadline_ms, 0xfffff000u + 3000u,
			"the deadline wrapped past 2^32 as unsigned arithmetic does");

	g_fake.now_ms = 0xfffff000u + 1000u;             /* first beat */
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1, "the beat fires across the wrap");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_RECONNECT, "as a reconnect");
	g_fake.now_ms = (uint32_t)(0xfffff000u + 3000u); /* expiry, now past zero */
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1, "and expiry fires too");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_PROPOSE_TRANSITION,
			"proposing the transition on the far side of the rollover");
}

/* ==========================================================================
 * 7. The local CSB is never a reconnect subject
 * ========================================================================== */
static void test_local_csb_never_reconnects(void)
{
	struct vms_csb *local;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] the LOCAL CSB is never a reconnect subject (p. 7-24)\n");
	(void)bed(3, 0, 0);
	local = cnxman_club_local(&g_cl.club);

	g_fake.now_ms = 0;
	ct_check_eq_u32(cnxman_recnx_connectivity_lost(&g_recnx, local, 0),
			CNXMAN_CSB_ACT_NONE,
			"connectivity-lost at the local CSB does nothing");
	ct_check_eq_u32(local->state, VMS_CNXMAN_CSB_LOCAL, "it stays LOCAL");
	ct_check(cnxman_club_ignored_events(&g_cl.club) > 0,
		 "and the event is COUNTED as ignored, not silently swallowed");

	g_fake.now_ms = 100000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 0,
			"no tick can make this node propose its own removal");
}

/* ==========================================================================
 * 8. Lifecycle and NULL safety
 * ========================================================================== */
static void test_lifecycle_and_null_safety(void)
{
	struct cnxman_recnx_rec rec[4];
	struct cnxman_recnx r;

	printf("[recnx] start/stop and NULL safety\n");
	(void)bed(3, 0, 0);
	ct_check(g_fake.timers_armed >= 1, "start armed the once-a-second timer");
	cnxman_recnx_start(&g_recnx);
	cnxman_recnx_stop(&g_recnx);
	ct_check_eq_u32(g_fake.timers_cancelled, 1,
			"stop cancels it exactly once, and is idempotent");
	cnxman_recnx_stop(&g_recnx);
	ct_check_eq_u32(g_fake.timers_cancelled, 1, "a second stop cancels nothing");

	cnxman_recnx_init(&r, NULL, NULL);
	ct_check_eq_u32(cnxman_recnx_tick(&r, rec, 4), 0, "a tick with no node is 0");
	ct_check_eq_u32(cnxman_recnx_shutdown(&r, rec, 4), 0,
			"a shutdown with no node emits nothing");
	ct_check(cnxman_recnx_connectivity_lost(&r, NULL, 0) ==
			 CNXMAN_CSB_ACT_NONE,
		 "connectivity-lost with no node is safe");
	cnxman_recnx_init(NULL, NULL, NULL);
	cnxman_recnx_start(NULL);
	cnxman_recnx_stop(NULL);
	ct_check_eq_u32(cnxman_recnx_tick(NULL, rec, 4), 0, "tick(NULL) is 0");

	/* A zero-length record array must not be written through. */
	(void)bed(5, 0, 0);
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, cnxman_club_csb_at(&g_cl.club, 1), 0);
	g_fake.now_ms = 1000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, NULL, 0), 0,
			"a tick with nowhere to write reports 0 records");
	ct_check_eq_u32(g_recnx.attempts_issued, 1,
			"but the attempt still happened -- the FSM is not gated on its output");
}

/*
 * The degenerate window: RECNXINTERVAL = 1 with no remote contribution puts the
 * first beat and the deadline on the same millisecond. Expiry is evaluated
 * first, so the window closes instead of firing an attempt it has no time to
 * hear back from. Asserted rather than left to be discovered: a one-second
 * reconnect interval is a configuration a site can actually set, and the
 * ordering here is what decides whether it reconnects at all.
 */
static void test_degenerate_one_second_window(void)
{
	struct vms_csb *peer;
	struct cnxman_recnx_rec rec[4];

	printf("[recnx] RECNXINTERVAL=1: the window closes on the first beat\n");
	peer = bed(1, 0, 0);
	g_fake.now_ms = 0;
	(void)cnxman_recnx_connectivity_lost(&g_recnx, peer, 0);
	ct_check(peer->deadline_ms == peer->next_attempt_ms,
		 "deadline and first beat coincide at RECNXINTERVAL=1");

	g_fake.now_ms = 1000;
	ct_check_eq_u32(cnxman_recnx_tick(&g_recnx, rec, 4), 1, "one record");
	ct_check_eq_u32(rec[0].action, CNXMAN_CSB_ACT_PROPOSE_TRANSITION,
			"expiry wins: the period is over, so no attempt is started");
	ct_check_eq_u32(peer->attempts, 0, "and none was counted");
}

int main(void)
{
	printf("=== test_cnxman_recnx: RECNXINTERVAL/TIMVCFAIL + last gasp ===\n");
	test_period_arithmetic();
	test_cadence_and_membership_held();
	test_window_walks_wait_and_reconnect();
	test_peer_driven_reaccept();
	test_recovers_without_membership_loss();
	test_peer_transition_suppresses_our_own();
	test_inbound_last_gasp_skips_the_window();
	test_outbound_last_gasp_on_shutdown();
	test_standalone_node_announces_nothing();
	test_clock_hazards();
	test_local_csb_never_reconnects();
	test_lifecycle_and_null_safety();
	test_degenerate_one_second_window();
	return ct_summary("test_cnxman_recnx");
}
