/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_giveup_reject_cluexit.c - rd vms-0f9's rung-R2 leg: the
 * whole standoff, on the virtual clock, and the way out of it.
 *
 * WHAT IS BEING REPRODUCED, AND WHERE IT WAS MEASURED.
 * tests/lab/captures/vms-b36-cnxmgrerr-20260925/. A second OVMX node's
 * admission is abandoned when its LAN goes dark inside the transition; when the
 * LAN comes back the real VAX dials it once a second, this executive ACCEPTED
 * every time, and the VAX put its last-gasp datagram on the multicast 0.3 ms
 * after the connection completed -- a fatal CNXMGRERR, reproduced on both sides
 * of an A/B (analysis/crash-window-M1-2.txt, crash-window-F1-2.txt). Where it
 * did not bugcheck, the pair span in a ~2 Hz accept-and-be-hung-up-on loop
 * (rd vms-4c9: 460 cycles in one arm).
 *
 * WHAT A REAL NODE DOES INSTEAD, measured on three real OpenVMS VAX V7.3 nodes
 * on their own bridge (analysis/oracle-accept-vs-reject.txt): it answers
 * REJECT_REQ, once a second, both directions -- and the node the cluster gave
 * up on takes CLUEXIT, reboots, and comes back as a NEW INCARNATION, which is
 * what ends the standoff.
 *
 * THE FOUR STEPS THIS SCENARIO RUNS, in order, on one node:
 *   1. GIVE UP.   The p. 7-30 window really expires on the virtual clock,
 *                 driven by the SHIPPING reconnect FSM over the SHIPPING CSB
 *                 ladder. No state is hand-set.
 *   2. REJECT.    The peer dials back at the SAME incarnation and the SHIPPING
 *                 acceptance policy refuses it -- once a second, for as long
 *                 as it keeps dialling.
 *   3. CLUEXIT.   The node re-incarnates. The glue that does this
 *                 (vms_cnxman.c) is not host-linkable, so the harness performs
 *                 the three things vms_cnxman.h sec 7b says it does -- new
 *                 incarnation, CLUB reset, FSMs re-armed -- and the assertions
 *                 are about what the SHIPPING code then decides.
 *   4. REJOIN.    The same peer's connect is ACCEPTED again, because p. 7-25's
 *                 edge fired on a different incarnation.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1: the window is a real bounded number
 * of beats on a virtual clock rather than a dispatched event, the refusals are
 * counted across the whole standoff rather than at one call, and the assertion
 * is the ORDERED sequence of what this node decided.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cnxman_barrier_fsm.h"

/* The bench rig's own identities, so this scenario and the capture name the
 * same two systems. */
#define PEER_SYSID   1989ull            /* VAXC, the real VAX          */
#define OWN_SYSID    1988ull            /* OVMXB, the node under test  */
#define PEER_CSID    0x00010001u
#define CM_CONID     0x4e62000au
#define SIM_NODE     0u
#define RECNXINTERVAL_SECS 20u

#define INC_OLD 0x0000AAAA00000001ull   /* the peer's incarnation, before */
#define INC_NEW 0x0000BBBB00000002ull   /* ...and after it re-incarnates  */

struct bed {
	struct sim_clock       clock;
	struct vms_cluster     cl;
	struct cnxman_ops      ops;
	struct cnxman_join_ops jops;
	struct cnxman_join     j;
	struct cnxman_barrier  b;
	struct cnxman_recnx    recnx;

	int      circuit_to_peer;      /* the port model */
	uint64_t peer_incarnation;     /* what that circuit advertises */
	uint32_t beats;
	uint32_t accepted;             /* connects this node took      */
	uint32_t refused;              /* ...and refused               */
	uint32_t own_incarnations;     /* how many times WE re-incarnated */
	char     last_log[200];
};

static struct bed g;

/* ---- injected ops -------------------------------------------------------- */

static uint32_t bed_now_ms(void *ctx)
{
	(void)ctx;
	return sim_clock_now_ms(&g.clock);
}

static void bed_arm(void *ctx, enum cnxman_timer which, uint32_t key,
		    uint32_t ms)
{
	(void)ctx;
	sim_clock_arm(&g.clock, (uint8_t)SIM_NODE, (uint8_t)which, key, ms);
}

static void bed_cancel(void *ctx, enum cnxman_timer which, uint32_t key)
{
	(void)ctx;
	sim_clock_cancel(&g.clock, (uint8_t)SIM_NODE, (uint8_t)which, key);
}

static void bed_log(void *ctx, const char *msg)
{
	size_t n;

	(void)ctx;
	if (msg == NULL)
		return;
	n = strlen(msg);
	if (n >= sizeof(g.last_log))
		n = sizeof(g.last_log) - 1;
	memcpy(g.last_log, msg, n);
	g.last_log[n] = '\0';
}

static int bed_dir_inquire(void *ctx, vms_scs_sysid_t dst, const uint8_t *name)
{
	(void)ctx; (void)dst; (void)name;
	return 0;
}

static int bed_connect(void *ctx, vms_scs_sysid_t dst,
		       const uint8_t *local_name, const uint8_t *remote_name,
		       const uint8_t *conndata, uint16_t credits,
		       vms_conid_t *out_conid)
{
	(void)ctx; (void)dst; (void)local_name; (void)remote_name;
	(void)conndata; (void)credits; (void)out_conid;
	return -1;   /* this node's own connects go unanswered in this window */
}

static int bed_send_msg(void *ctx, vms_conid_t conid, const uint8_t *body,
			uint32_t len)
{
	(void)ctx; (void)conid; (void)body; (void)len;
	return 0;
}

static int bed_disconnect(void *ctx, vms_conid_t conid)
{
	(void)ctx; (void)conid;
	return 0;
}

static uint64_t bed_time_now(void *ctx)
{
	(void)ctx;
	return sim_clock_now_vms(&g.clock);
}

/* ---- the bed ------------------------------------------------------------- */

static void bed_arm_fsms(void)
{
	struct cnxman_join_cfg cfg;

	(void)cnxman_club_init(&g.cl);
	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);
	cnxman_recnx_init(&g.recnx, &g.cl, &g.ops);

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.model, "OVMX simulated node", 19);
	cfg.model_len = 19;
	cfg.model_valid = 1;
	cnxman_join_set_cfg(&g.j, &cfg);
}

static void bed_init(void)
{
	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);

	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.log = bed_log;

	g.jops.dir_inquire = bed_dir_inquire;
	g.jops.connect = bed_connect;
	g.jops.send_msg = bed_send_msg;
	g.jops.disconnect = bed_disconnect;
	g.jops.time_now = bed_time_now;

	memcpy(g.cl.params.scsnode, "OVMXB0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	g.cl.params.recnxinterval = (uint16_t)RECNXINTERVAL_SECS;

	bed_arm_fsms();
	g.circuit_to_peer = 1;
	g.peer_incarnation = INC_OLD;
}

/* ==========================================================================
 * THE BEAT, in vms_cnxman.c's order: discover, learn each peer's advertised
 * incarnation off its circuit, run the reconnect ladder.
 * ========================================================================== */

static struct vms_csb *peer_csb(void)
{
	return cnxman_club_find_sysid(&g.cl.club, PEER_SYSID);
}

static void beat_discover(void)
{
	if (!g.circuit_to_peer || peer_csb() != NULL)
		return;
	(void)cnxman_club_alloc_csb(&g.cl.club, PEER_SYSID, 1);
}

/* cnxman_sync_peer_incarnation(): what the circuit really advertises. */
static void beat_sync_incarnation(void)
{
	struct vms_csb *csb = peer_csb();

	if (csb == NULL)
		return;
	if (!g.circuit_to_peer)
		cnxman_csb_set_incarnation(&g.cl.club, csb, 0u, 0);
	else
		cnxman_csb_set_incarnation(&g.cl.club, csb,
					   g.peer_incarnation, 1);
}

static void beat_recnx(void)
{
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];

	(void)cnxman_recnx_tick(&g.recnx, recs, VMS_CLUB_MAX_CSB);
}

static void beat(void)
{
	g.clock.now_ms += CNXMAN_RECNX_ATTEMPT_MS;
	g.beats++;
	beat_discover();
	beat_sync_incarnation();
	beat_recnx();
}

/* The peer dials this node. The SHIPPING acceptance policy decides. */
static int peer_dials(void)
{
	int rc = cnxman_join_connect_req(&g.j, PEER_SYSID, CM_CONID,
					 (const uint8_t *)0, 0u);

	if (rc == 0)
		g.accepted++;
	else
		g.refused++;
	return rc;
}

/*
 * CLUEXIT, as vms_cnxman.h sec 7b defines it. The glue that performs this is
 * not host-linkable; what the harness models is exactly its three effects, and
 * every decision that follows is the shipping code's.
 */
static void cluexit(void)
{
	g.own_incarnations++;
	bed_arm_fsms();              /* the CLUB and the four FSMs, rebuilt */
	g.cl.state = VMS_CLUSTER_JOINING;
}

/* ==========================================================================
 * The run
 * ========================================================================== */

static void test_giveup_reject_cluexit_rejoin(void)
{
	struct vms_csb *csb;
	uint32_t start, refused_before;

	printf("\n-- rd vms-0f9: give up -> refuse -> CLUEXIT -> rejoin --\n");
	bed_init();

	/* --- the pair's connection comes up, and this node's own connect is
	 *     not the one that opened it (the measured shape). --- */
	beat();
	csb = peer_csb();
	ct_check(csb != NULL, "the peer was discovered");
	ct_check(csb->incarnation_valid &&
		 csb->incarnation == INC_OLD,
		 "...and the block carries the incarnation its circuit "
		 "advertises");
	ct_check(peer_dials() == 0,
		 "CONTROL: before anything went wrong the peer's connect is "
		 "ACCEPTED");

	(void)cnxman_csb_dispatch(&g.cl.club, csb, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g.ops);
	cnxman_csb_bind_connection(csb, CM_CONID);
	(void)cnxman_recnx_connectivity_gained(&g.recnx, csb);
	ct_check_eq_u32(csb->state, (uint8_t)VMS_CNXMAN_CSB_OPEN,
			"the pair's VMS$VAXcluster connection is OPEN");

	/* --- STEP 1: the LAN goes dark and the whole p. 7-30 window runs
	 *     out on the clock. --- */
	(void)cnxman_recnx_connectivity_lost(&g.recnx, csb, 0);
	ct_check_eq_u32(csb->state, (uint8_t)VMS_CNXMAN_CSB_WAIT,
			"p. 7-30's reconnect window opens");
	ct_check_eq_u32(cnxman_club_giveup_count(&g.cl.club), 0u,
			"...and nothing is given up on while it runs");
	ct_check(peer_dials() == 0,
		 "p. 7-24 REACCEPT: INSIDE the window the peer's own offer is "
		 "still ACCEPTED -- #1309's recovery path is untouched");

	start = g.beats;
	while (g.beats - start < RECNXINTERVAL_SECS + 2u)
		beat();
	ct_check(cnxman_club_gave_up_on(&g.cl.club, PEER_SYSID, INC_OLD),
		 "STEP 1: the window expired and this node gave up on THAT "
		 "incarnation");

	/* --- STEP 2: the LAN comes back and the peer dials, once a second,
	 *     at the same incarnation. Every one is refused. --- */
	refused_before = g.refused;
	{
		uint32_t i;

		for (i = 0; i < 8u; i++) {
			beat();
			ct_check(peer_dials() != 0,
				 "STEP 2: the same incarnation's connect is "
				 "REFUSED");
		}
	}
	ct_check_eq_u32(g.refused - refused_before, 8u,
			"...every one of them, for as long as it keeps "
			"dialling");
	ct_check_eq_u32(g.j.inbound_refused_giveup, 8u,
			"...and each is counted as its own diagnosis");
	ct_check_eq_u32(g.accepted, 2u,
			"...while the two connects from BEFORE the give-up "
			"still stand as accepts");

	/* --- STEP 3: this node re-incarnates. --- */
	cluexit();
	ct_check_eq_u32(g.own_incarnations, 1u, "STEP 3: CLUEXIT ran");
	ct_check_eq_u32(cnxman_club_giveup_count(&g.cl.club), 0u,
			"...and a new incarnation of THIS node starts with an "
			"empty ledger: it has given up on nobody");

	/* --- STEP 4: and the very next offer is taken. --- */
	beat();
	ct_check(peer_csb() != NULL, "the peer is rediscovered");
	ct_check(peer_dials() == 0,
		 "STEP 4: the peer's connect is ACCEPTED again -- the standoff "
		 "is over");
}

/*
 * THE OTHER WAY OUT, and the one the oracle actually filmed: the PEER
 * re-incarnates. p. 7-25's edge then fires on this node without any CLUEXIT at
 * all, which is what a member does when a removed system reboots.
 */
static void test_a_peer_that_reincarnates_is_taken_back(void)
{
	struct vms_csb *csb;
	uint32_t start;

	printf("\n-- rd vms-0f9: ...or the PEER comes back as a new "
	       "incarnation (p. 7-25) --\n");
	bed_init();
	beat();
	csb = peer_csb();
	(void)cnxman_csb_dispatch(&g.cl.club, csb, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g.ops);
	cnxman_csb_bind_connection(csb, CM_CONID);
	(void)cnxman_recnx_connectivity_gained(&g.recnx, csb);
	(void)cnxman_recnx_connectivity_lost(&g.recnx, csb, 0);

	start = g.beats;
	while (g.beats - start < RECNXINTERVAL_SECS + 2u)
		beat();
	ct_check(cnxman_club_gave_up_on(&g.cl.club, PEER_SYSID, INC_OLD),
		 "given up on the old incarnation");
	ct_check(peer_dials() != 0, "and refusing it");

	/* The peer reboots: its circuit now advertises a different quadword. */
	g.peer_incarnation = INC_NEW;
	beat();
	ct_check_eq_u32(cnxman_club_giveup_count(&g.cl.club), 0u,
			"p. 7-25: a new incarnation of a system has been seen, "
			"and the record is cleared");
	ct_check(peer_dials() == 0,
		 "...and the new incarnation's connect is ACCEPTED, with no "
		 "CLUEXIT needed on this side");
}

int main(void)
{
	printf("=== cnxman_giveup_reject_cluexit: rd vms-0f9 on the virtual "
	       "clock ===\n");
	test_giveup_reject_cluexit_rejoin();
	test_a_peer_that_reincarnates_is_taken_back();
	return ct_summary("cnxman_giveup_reject_cluexit");
}
