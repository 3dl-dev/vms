/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_barrier_loop.c - the coordinator and the participant, CLOSED ON
 * EACH OTHER (rd vms-c06, test-ladder rung R1/R2 boundary).
 *
 * WHY THIS FILE EXISTS. test_cnxman_coord.c drives the coordinator with step
 * reports the TEST builds (`mk_step(f, g.c.epoch, step)`), and
 * test_cnxman_barrier.c drives the participant with releases the TEST builds.
 * Each half is green, and the two halves never met: neither file can observe
 * what happens when the ONLY thing that advances the barrier is the OTHER
 * FSM's real origination. That gap shipped a two-node cluster whose
 * coordinator sat in CNXMAN_COORD_BARRIER from its first admission to the end
 * of the run (lab capture vms-4838-rejoin-2node-20260913: RIG-A-CLUB
 * transition=1 from t=56 s to t=200 s, on the PASSING control run), because
 * the participant reported step 1 and never heard a release it recognised.
 *
 * SO THIS BED SUPPLIES NO TRANSITION FRAME AT ALL. Node A runs the real
 * coordinator FSM, node B runs the real participant barrier FSM, and every
 * byte either of them receives was ORIGINATED by the other through the
 * shipping codec. The only frame this file builds is the joiner's op-0x02
 * admission request, which is the join FSM's (FC-P3.3) and is what starts an
 * admission; and the only answers it stands in for are the join dialogue's
 * own -- the 0x81/0x03 commit echo and the 0x81/0x05, 0x81/0x06 membership
 * echoes -- built with the same grounded builder the join uses. Both are
 * named at the call site.
 *
 * GROUNDING. Wire: docs/cluster-protocol-spec.md sec 4(o)/(p) -- the join
 * dialogue and the 12 x (M-1) barrier census. Book: *VAXcluster Principles*
 * (Davis 1993) pp. 7-40..7-42 (Phase 1 / Phase 2 / the synchronised rebuild)
 * and p. 7-41 (the coordinator abandons on a lost participant BEFORE the GO;
 * after it, the loss drops the member from the census and the transition
 * still has to finish). Host-only transcript, page cites only (Rule 8).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_coord_fsm.h"
#include "vms_cnxman_join_fsm.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * The two nodes
 *
 * A is the established member and coordinator; B is the joiner. The CSIDs are
 * the lab rig's own (book p. 7-25: CSID = (sequence << 16) | CSV index), and
 * the SCSSYSTEMIDs are the two the 2-node genesis rig really reads back.
 * ========================================================================== */

#define A_SYSID   1025ull
#define B_SYSID   1026ull
#define A_CSID    0x00010001u
#define B_SLOT    2u
#define A_EPOCH   1u

/* Each node's CLUB has its own local block in slot 0 and the peer in slot 1. */
#define SLOT_LOCAL 0
#define SLOT_PEER  1

/* One body in flight between the two connection managers. The transport is a
 * QUEUE, not a call: a real send goes out through SCS and comes back on the
 * fork thread, so an FSM is never re-entered from inside its own send. */
struct wire_msg {
	uint8_t  body[VMS_CM_BODY_LEN];
	uint32_t len;
	int      to_a;          /* 1 = deliver to A, 0 = deliver to B */
};

#define MAX_WIRE 512

/* The software-version token both nodes of this OVMX<->OVMX loop advertise.
 * Its VALUE is irrelevant -- what matters is that it is the SAME on both, so
 * cnxman_csb_set_swver() derives peer_is_ours (rd vms-1ac). */
#define OWN_SWVER "OVMXV07\0"

struct node {
	struct vms_cluster cl;
	struct cnxman_ops  ops;
	struct fake_cnx    fake;
};

struct loop_bed {
	struct node a;
	struct node b;
	struct cnxman_coord   coord;    /* the coordinator, on A            */
	struct cnxman_join    a_join;   /* A's own join -- a FOUNDER's, so it
					 * never reaches CNXMAN_JOIN_MEMBER  */
	struct cnxman_barrier a_barrier;/* A's participant half              */
	struct cnxman_barrier barrier;  /* the participant, on B            */

	struct wire_msg wire[MAX_WIRE];
	uint32_t        head, tail;

	uint32_t delivered_to_a;
	uint32_t delivered_to_b;
	uint32_t unrouted_at_a;
	uint32_t unrouted_at_b;
	uint32_t join_half_echoes;      /* answers the JOIN FSM owns on a real
					 * node and this bed stands in for   */
	uint32_t consumed_by_a_join;    /* frames A's join took off the wire */
	uint32_t consumed_by_a_barrier; /* ... and A's participant half      */
	uint8_t  last_unrouted_cat;
	uint8_t  last_unrouted_op;
	int      b_deaf;                /* B's transport is down (a real
					 * connectivity loss, p. 7-41)       */
	uint32_t kill_b_after_steps;    /* cut the link once B has reported
					 * this many steps (0 = never)       */
};

static struct loop_bed g;

/* ==========================================================================
 * The transport
 * ========================================================================== */

static void wire_post(int to_a, const uint8_t *body, uint32_t len)
{
	struct wire_msg *m;

	if (g.tail >= MAX_WIRE || len > VMS_CM_BODY_LEN)
		return;
	if (!to_a && g.b_deaf)
		return;   /* the link to B is down: the bytes go nowhere */
	m = &g.wire[g.tail++];
	memcpy(m->body, body, len);
	m->len = len;
	m->to_a = to_a;
}

static int a_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	(void)dst;
	wire_post(0, body, len);
	return 0;
}

static int a_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	wire_post(0, body, len);
	return 0;
}

static int b_send_csb(void *ctx, int32_t csb_index, const uint8_t *body,
		      uint32_t len)
{
	(void)ctx;
	(void)csb_index;
	wire_post(1, body, len);
	return 0;
}

static int b_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx;
	wire_post(1, body, len);
	return 0;
}

/* ==========================================================================
 * The join FSM's half, stood in for
 *
 * On a real node these three answers come from vms_cnxman_join_fsm.c, which is
 * not what this file is exercising. The echo is built with the SAME grounded
 * builder the join uses (vms_cm_echo_response_build), so the bytes the
 * coordinator sees are the ones a real joiner sends.
 * ========================================================================== */
static void join_half_echo(const uint8_t *body, uint32_t len, uint8_t tr_class)
{
	uint8_t out[VMS_CM_BODY_LEN];
	uint32_t written = 0;

	if (vms_cm_echo_response_build(body, len, tr_class, out,
				       (uint32_t)sizeof(out),
				       &written) != VMS_CODEC_OK)
		return;
	g.join_half_echoes++;
	wire_post(1, out, written);
}

/*
 * A's join FSM is in the ROUTE, not under test: it is never started, so it
 * never reaches for any of these. They are here because cnxman_join_init()
 * takes them, and every one is deliberately NULL -- if the founder's idle join
 * ever transmits, this bed crashes instead of quietly passing.
 */
static const struct cnxman_join_ops a_join_ops;

/* ==========================================================================
 * Delivery
 * ========================================================================== */

static void note_unrouted(uint32_t *counter, const uint8_t *body, uint32_t len)
{
	struct vms_cm_envelope env;

	(*counter)++;
	if (vms_cm_envelope_parse(body, len, &env) == VMS_CODEC_OK) {
		g.last_unrouted_cat = env.category;
		g.last_unrouted_op = env.opcode;
	}
}

static void deliver_to_b(const uint8_t *body, uint32_t len)
{
	struct vms_cm_envelope env;

	g.delivered_to_b++;
	if (cnxman_barrier_rx_body(&g.barrier, body, len, A_CSID, 1,
				   SLOT_PEER) == CNXMAN_BARRIER_RX_CONSUMED)
		return;
	if (vms_cm_envelope_parse(body, len, &env) != VMS_CODEC_OK) {
		note_unrouted(&g.unrouted_at_b, body, len);
		return;
	}
	/* Exactly the three the join FSM answers during an admission. */
	if (env.category == VMS_CM_CAT_CONFIG &&
	    (env.opcode == VMS_CM_OP_COMMIT ||
	     env.opcode == VMS_CM_OP_MEMBREC ||
	     env.opcode == VMS_CM_OP_MEMBERSHIP)) {
		join_half_echo(body, len, VMS_CM_CLASS_ADD);
		return;
	}
	note_unrouted(&g.unrouted_at_b, body, len);
}

/*
 * THE GLUE'S OWN ROUTING ORDER, on the node that is coordinating
 * (vms_cnxman.c cnxman_vc_route): join first, then the participant barrier,
 * then the coordinator; unclaimed is counted and logged, never dropped in
 * silence.
 *
 * The join FSM is in the bed because it is WHERE THE BARRIER STALLED. A
 * founder's join never leaves the states whose table has no barrier cell, and
 * an empty cell used to answer CONSUMED -- so the joiner's step reports died
 * one FSM short of the coordinator that owed the release. A bed that skipped
 * the join could not see that, which is exactly why the shipped stack could
 * not either.
 */
static void deliver_to_a(const uint8_t *body, uint32_t len)
{
	g.delivered_to_a++;

	if (cnxman_join_rx_body(&g.a_join, body, len, 0u, 0, SLOT_PEER) ==
	    CNXMAN_JOIN_RX_CONSUMED) {
		g.consumed_by_a_join++;
		return;
	}
	if (cnxman_barrier_rx_body(&g.a_barrier, body, len, 0u, 0,
				   SLOT_PEER) == CNXMAN_BARRIER_RX_CONSUMED) {
		g.consumed_by_a_barrier++;
		return;
	}
	if (cnxman_coord_rx_body(&g.coord, body, len, SLOT_PEER) ==
	    CNXMAN_COORD_RX_CONSUMED)
		return;
	note_unrouted(&g.unrouted_at_a, body, len);
}

/* Drain the wire until both connection managers have nothing left to say. */
static void pump(void)
{
	while (g.head < g.tail) {
		struct wire_msg *m = &g.wire[g.head++];

		if (m->to_a)
			deliver_to_a(m->body, m->len);
		else
			deliver_to_b(m->body, m->len);

		/* A circuit that dies in the middle of the barrier, at the
		 * step this scenario chose. */
		if (g.kill_b_after_steps != 0u &&
		    g.barrier.steps_sent >= g.kill_b_after_steps)
			g.b_deaf = 1;
	}
}

/* ==========================================================================
 * The bed
 * ========================================================================== */

static void seed_dialogue(struct vms_csb *csb)
{
	/* Where this bed's simulated connection starts -- real, advancing
	 * per-CSB dialogue state (design sec 3.2.4 ruling E1), not a
	 * placeholder. */
	csb->cm_send_msg = 0x0140u;
	csb->cm_ack_msg = 0x0100u;
	csb->cm_txn = 0x0009u;
	csb->cm_token = 0x07f5u;
}

static void node_init(struct node *n, const char *name,
		      vms_scs_sysid_t sysid)
{
	fake_ops_init(&n->ops, &n->fake);
	n->fake.now_ms = 100000u;
	memcpy(n->cl.params.scsnode, name, strlen(name));
	n->cl.params.scsnode_len = (uint8_t)strlen(name);
	/*
	 * BOTH NODES RUN THIS IMPLEMENTATION, AND SAY SO (rd vms-1ac). This
	 * loop is OVMX<->OVMX, and on a live node the port's formation body
	 * carries each peer's software-version token, which
	 * cnxman_sync_peer_swver() copies onto the CSB before any CM frame is
	 * routed. Without it the bed models a pair that has advertised NOTHING
	 * -- which is exactly what the coordinator's grounded-open gate
	 * refuses, and rightly: an op-0x09 this executive cannot build
	 * byte-faithfully must not be sent to a connection manager that is not
	 * this one.
	 */
	memcpy(n->cl.params.sw_version, OWN_SWVER, sizeof(OWN_SWVER) - 1u);
	n->cl.params.sw_version_len = (uint8_t)(sizeof(OWN_SWVER) - 1u);
	n->cl.params.scssystemid = sysid;
	n->cl.params.vaxcluster = 2;
	n->cl.params.votes = 1;
	n->cl.params.expected_votes = 1;
	(void)cnxman_club_init(&n->cl);
}

/* A: an established single-member cluster that has just founded (epoch 1), with
 * a real CSB for the joiner carrying NO CSID -- assigning one is the
 * coordinator's job. */
/* What cnxman_sync_peer_swver() does on a live node once the port has the
 * peer's formation body: copy the advertised token onto the CSB and let the
 * setter derive `peer_is_ours` from it. */
static void bed_prove_ours(struct node *n, struct vms_csb *csb)
{
	cnxman_csb_set_swver(csb, n->cl.params.sw_version,
			     n->cl.params.sw_version_len,
			     n->cl.params.sw_version,
			     n->cl.params.sw_version_len);
}

static void bed_init_a(void)
{
	struct vms_csb *local, *joiner;

	node_init(&g.a, "OVMXA", A_SYSID);
	g.a.ops.send = a_send;
	g.a.ops.respond = a_respond;

	local = &g.a.cl.club.csb[SLOT_LOCAL];
	cnxman_club_learn_local_csid(&g.a.cl.club, A_CSID);
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
					       VMS_CSB_F_MEMBER));
	g.a.cl.state = VMS_CLUSTER_MEMBER;
	g.a.cl.club.epoch = A_EPOCH;

	joiner = cnxman_club_alloc_csb(&g.a.cl.club, B_SYSID, 1);
	cnxman_csb_set_scsnode(joiner, (const uint8_t *)"OVMXB", 5);
	bed_prove_ours(&g.a, joiner);
	seed_dialogue(joiner);

	cnxman_coord_init(&g.coord, &g.a.cl, &g.a.ops);
	cnxman_barrier_init(&g.a_barrier, &g.a.cl, &g.a.ops);
	/*
	 * A FOUNDER'S JOIN, LEFT WHERE FOUNDING LEAVES IT. cnxman_coord_found()
	 * makes this node a member without a join drive, so the join FSM stays
	 * in CNXMAN_JOIN_IDLE -- the state the live rig's own join ring reads
	 * back for the founder (RIG-A-JOINREC state=0). It is in the route
	 * because the glue puts it there, not because it has anything to do.
	 */
	cnxman_join_init(&g.a_join, &g.a.cl, &g.a.ops, &a_join_ops);
}

/* B: a joining node that holds a real connection to A and knows A's CSID (the
 * join dialogue learned it), but has no CSID of its own yet. */
static void bed_init_b(void)
{
	struct vms_csb *coord_csb;

	node_init(&g.b, "OVMXB", B_SYSID);
	g.b.ops.send_csb = b_send_csb;
	g.b.ops.respond = b_respond;

	coord_csb = cnxman_club_alloc_csb(&g.b.cl.club, A_SYSID, 1);
	bed_prove_ours(&g.b, coord_csb);
	cnxman_csb_set_scsnode(coord_csb, (const uint8_t *)"OVMXA", 5);
	cnxman_csb_set_csid(coord_csb, A_CSID);
	cnxman_csb_set_flags(coord_csb, (uint16_t)(VMS_CSB_F_SELECTED |
						   VMS_CSB_F_MEMBER));
	seed_dialogue(coord_csb);

	cnxman_barrier_init(&g.barrier, &g.b.cl, &g.b.ops);
}

static void bed_init(void)
{
	memset(&g, 0, sizeof(g));
	bed_init_a();
	bed_init_b();
}

/* ==========================================================================
 * The one frame this bed builds: the joiner's op-0x02 admission request
 * (spec sec 4(o) step 4, "this starts admission")
 * ========================================================================== */
static uint32_t mk_join_request(uint8_t *body)
{
	vms_wire_buf_t w;

	memset(body, 0, VMS_CM_BODY_LEN);
	vms_wire_buf_init(&w, body, VMS_CM_BODY_LEN);
	vms_wire_put_le16(&w, VMS_OFB_CM_SEND_MSG, 0x0220);
	vms_wire_put_le16(&w, VMS_OFB_CM_ACK_MSG, 0x0140);
	vms_wire_put_le16(&w, VMS_OFB_CM_TXN, 0x0031);
	vms_wire_put_le16(&w, VMS_OFB_CM_TOKEN, 0x0abc);
	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY, VMS_CM_CAT_CONFIG);
	vms_wire_put_u8(&w, VMS_OFB_CM_OPCODE, VMS_CM_OP_CONFIG);
	vms_wire_put_u8(&w, VMS_OFB_CM_ROLE, VMS_CM_ROLE_COMMIT);
	return VMS_CM_BODY_LEN;
}

/* B asks A for admission, and then both connection managers run. */
static void drive_admission(void)
{
	uint8_t body[VMS_CM_BODY_LEN];
	uint32_t n = mk_join_request(body);

	wire_post(1, body, n);
	pump();
}

/* ==========================================================================
 * 1. THE WHOLE TRANSITION, END TO END
 * ========================================================================== */

static void test_two_node_add_runs_the_barrier_to_completion(void)
{
	printf("\n-- a two-node admission completes the barrier on both sides --\n");
	bed_init();
	drive_admission();

	/* The coordinator's side of the 12 x (M-1) law: M-1 = 1 participant. */
	ct_check_eq_u32(g.coord.transitions_driven, 1, "one transition driven");
	ct_check_eq_u32(g.coord.n_participants, 1, "M-1 = 1 participant frozen");
	ct_check_eq_u32(g.coord.steps_received, CNXMAN_COORD_BARRIER_STEPS,
			"twelve step reports received");
	ct_check_eq_u32(g.coord.releases_sent, CNXMAN_COORD_BARRIER_STEPS,
			"twelve releases sent");
	ct_check_eq_u32(g.coord.transitions_completed, 1,
			"the coordinator COMPLETED the transition");
	ct_check_eq_u32(g.a.cl.club.transition_active, 0,
			"A's CLUB no longer holds a transition open");
	ct_check_eq_u32(g.coord.transitions_abandoned, 0, "not abandoned");

	/* The participant's side. */
	ct_check_eq_u32(g.barrier.steps_sent, CNXMAN_BARRIER_STEPS,
			"the participant sent twelve steps");
	ct_check_eq_u32(g.barrier.transitions_completed, 1,
			"the participant COMPLETED the transition");
	ct_check_eq_u32(g.b.cl.club.transition_active, 0,
			"B's CLUB no longer holds a transition open");

	/*
	 * AND THE ROUTE ITSELF, which is where this stalled. Every one of the
	 * twelve steps passed THROUGH A's idle join FSM on its way to the
	 * coordinator; each is counted there as a frame that table declined.
	 * If the join ever eats one again, this number falls and the barrier
	 * stops -- which is exactly what the live rig did.
	 */
	ct_check_eq_u32(g.a_join.foreign_transition_frames,
			CNXMAN_COORD_BARRIER_STEPS,
			"all twelve steps were ROUTED ON by A's idle join FSM");
	ct_check_eq_u32(g.consumed_by_a_join, 0,
			"A's idle join took nothing off the wire");
	ct_check_eq_u32(g.consumed_by_a_barrier, 0,
			"A's participant half claimed no step meant for the "
			"coordinator");
	ct_check_eq_u32(g.coord.ignored_events, 0,
			"no coordinator event fell into an empty cell");
	ct_check_eq_u32(g.coord.send_failures, 0, "nothing failed to send");
	ct_check_eq_u32(g.unrouted_at_b, 0, "B routed every frame A sent");

	/*
	 * HONEST, AND A SEPARATE GAP. Two frames A cannot place: B's 0x81/0x05
	 * acknowledgements of the two MEMBERSHIP RECORDS the coordinator
	 * ORIGINATED -- no FSM has a cell for them, so the executive logs
	 * "an unroutable VMS$VAXcluster frame was received" twice per
	 * admission. The live rig logs exactly the same two
	 * (vms-4838-rejoin-2node-20260913 control-nodeA.console.log
	 * t=36.126/36.132), which is one of the three facts that identified
	 * this bed as faithful. Nothing gates on them, so they are asserted as
	 * the count they really are rather than wished to zero.
	 */
	ct_check_eq_u32(g.unrouted_at_a, 2,
			"exactly the two membership-record acks are unplaced");
	ct_check_eq_u32(g.last_unrouted_op, VMS_CM_OP_MEMBREC,
			"... and they are op-0x05, as on the live rig");
}

/* The order law, observed on a closed loop rather than asserted on a bed's own
 * synthetic reports: release N never precedes the participant's step N. */
static void test_no_release_precedes_its_step(void)
{
	printf("\n-- 0x0c#N never precedes 0x0b#N, over a real round trip --\n");
	bed_init();
	drive_admission();

	ct_check(g.coord.step_out_of_order == 0,
		 "no step arrived ahead of the one in progress");
	ct_check(g.barrier.step_mismatch == 0,
		 "no release arrived that did not match the step in flight");
	ct_check(g.coord.step_duplicates == 0, "no step was reported twice");
}

/* ==========================================================================
 * 2. A PARTICIPANT LOST (book p. 7-41 before the GO, p. 7-42 after it)
 *
 * The loss is reported through cnxman_coord_participant_lost() -- the entry
 * point vms_cnxman.c's cnxman_transition_peer_lost() now calls from the CSB
 * connectivity-loss ladder, and which until rd vms-c06 had no production
 * caller at all.
 * ========================================================================== */

/* Cut the link to B the moment it has reported `after_steps` steps, the way a
 * circuit really dies in the middle of a barrier. */
static void kill_b_after(uint32_t after_steps)
{
	g.kill_b_after_steps = after_steps;
}

static void test_a_silent_participant_is_never_timed_out(void)
{
	printf("\n-- a silent participant stalls the barrier, and is NEVER "
	       "timed out --\n");
	bed_init();
	g.b_deaf = 1;          /* B never hears the open: it can never answer */
	drive_admission();

	ct_check_eq_u32(g.coord.transitions_completed, 0,
			"the transition is NOT complete");
	ct_check_eq_u32(g.a.cl.club.transition_active, 1,
			"A's CLUB still holds the transition open");

	cnxman_coord_timer(&g.coord);
	cnxman_coord_timer(&g.coord);
	ct_check(g.coord.slow_phase1 + g.coord.slow_steps > 0,
		 "the watchdog counted the stall");
	ct_check_eq_u32(g.coord.transitions_abandoned, 0,
			"a slow participant is never abandoned by a timer "
			"(spec sec 4(p))");
}

static void test_loss_before_the_go_abandons_the_transition(void)
{
	printf("\n-- a participant lost BEFORE the GO abandons the transition "
	       "(p. 7-41) --\n");
	bed_init();
	g.b_deaf = 1;
	drive_admission();
	ct_check(g.coord.state != (uint8_t)CNXMAN_COORD_BARRIER,
		 "(pre-condition) the coordinator has not committed Phase 2");

	cnxman_coord_participant_lost(&g.coord, SLOT_PEER);

	ct_check_eq_u32(g.coord.transitions_abandoned, 1,
			"the transition was ABANDONED, not left open");
	ct_check_eq_u32(g.coord.transitions_completed, 0,
			"and it certainly did not complete");
	ct_check_eq_u32(g.a.cl.club.transition_active, 0,
			"A's CLUB no longer claims a transition is running");
	ct_check_eq_u32(g.coord.releases_sent, 0,
			"nothing was released: there was no barrier to release");
}

static void test_loss_inside_the_barrier_drops_it_and_finishes(void)
{
	printf("\n-- a participant lost INSIDE the barrier is dropped and the "
	       "barrier FINISHES (p. 7-42) --\n");
	bed_init();
	kill_b_after(3u);
	drive_admission();

	/* The state the live rig was stuck in: committed, mid-barrier, and
	 * waiting on a system that will never answer. */
	ct_check(g.coord.state == (uint8_t)CNXMAN_COORD_BARRIER,
		 "(pre-condition) Phase 2 committed and the barrier is running");
	ct_check_eq_u32(g.coord.transitions_completed, 0,
			"(pre-condition) it has NOT finished on its own");
	ct_check_eq_u32(g.a.cl.club.transition_active, 1,
			"(pre-condition) the CLUB still holds it open");
	ct_check_eq_u32(g.coord.n_participants, 1,
			"(pre-condition) the census still counts the lost node");

	cnxman_coord_participant_lost(&g.coord, SLOT_PEER);
	pump();

	ct_check_eq_u32(g.coord.n_participants, 0,
			"the lost system is DROPPED from the frozen census");
	ct_check_eq_u32(g.coord.transitions_completed, 1,
			"the barrier finished instead of standing open");
	ct_check_eq_u32(g.coord.transitions_abandoned, 0,
			"past the GO it is NOT abandoned (p. 7-42)");
	ct_check_eq_u32(g.a.cl.club.transition_active, 0,
			"A's CLUB no longer claims a transition is running");
	/*
	 * And not one release went to the system that is gone: the three it
	 * really reported are all it was ever sent. A release fanned out to a
	 * dead block is a body stamped on a connection that does not exist.
	 */
	ct_check_eq_u32(g.coord.releases_sent, 3,
			"only the steps it really reported were released to it");
}

/* The same loss reported twice -- the close path and then the reconnect beat,
 * which is exactly what vms_cnxman.c does -- changes nothing the second time. */
static void test_reporting_the_same_loss_twice_is_harmless(void)
{
	printf("\n-- the close path and the beat may both report one loss --\n");
	bed_init();
	kill_b_after(3u);
	drive_admission();

	cnxman_coord_participant_lost(&g.coord, SLOT_PEER);
	pump();
	cnxman_coord_participant_lost(&g.coord, SLOT_PEER);
	pump();

	ct_check_eq_u32(g.coord.n_participants, 0, "the census stays at zero");
	ct_check_eq_u32(g.coord.transitions_completed, 1,
			"the transition completed exactly once");
	ct_check_eq_u32(g.coord.transitions_abandoned, 0,
			"and was not then abandoned on top of completing");
}

int main(void)
{
	test_two_node_add_runs_the_barrier_to_completion();
	test_no_release_precedes_its_step();
	test_a_silent_participant_is_never_timed_out();
	test_loss_before_the_go_abandons_the_transition();
	test_loss_inside_the_barrier_drops_it_and_finishes();
	test_reporting_the_same_loss_twice_is_harmless();
	return ct_summary("test_cnxman_barrier_loop");
}
