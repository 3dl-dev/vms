/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_recover_after_giveup.c - rd vms-dfe's rung-R2 leg: the
 * MEASURED in-browser stall, replayed on the virtual clock, and the recovery
 * that has to follow it.
 *
 * WHAT IS BEING REPRODUCED, AND WHERE IT WAS MEASURED.
 * tests/lab/captures/vms-e18e-cn3-browser-20260925/cn3-intermittent/ --
 * OVMXB (OVMX/VAX under pcjs, NetBSD/vax substrate) joining a real OpenVMS VAX
 * V7.3. Its console, verbatim:
 *
 *   [ 88.69] %CNXMAN, the cluster opened the VMS$VAXcluster connection to this node
 *   [ 89.54] %CNXMAN, lost connection to a cluster member, reconnecting
 *   [ 89.55] %CNXMAN, lost the VMS$VAXcluster connection before this node was admitted
 *   [ 90.56] %CNXMAN, adopting the VMS$VAXcluster connection the executive holds
 *   [ 90.91] %CNXMAN, a cluster member refused this node's reconnect
 *   [110.04] %CNXMAN, reconnect interval expired, proposing removal
 *   [111.03] %CNXMAN, the reconnect interval expired with no VMS$VAXcluster
 *            connection to the member: this node is NOT a cluster member
 *
 * ...and then nothing, for the remaining 25 minutes of the window, beside a
 * healthy cluster whose PE circuit this node could still see. Every line is
 * honest; the recovery never came. The cause is the CSB the ladder parked in
 * p. 7-24 DISCONNECT: the table offers it one edge, the peer sweep skips a
 * system that already has a block, and join_askable() refuses to drive through
 * a connection the ladder gave up on. p. 7-25 does not have that resting
 * place -- it DEALLOCATES the block and builds a new one when the system is
 * seen again -- and until rd vms-dfe nothing in the executive did.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1 (the same three things
 * scenarios/cnxman_join.c lists):
 *
 *   1. THE WHOLE 20-SECOND p. 7-30 WINDOW IS REALLY RUN, beat by beat, by the
 *      SHIPPING vms_cnxman_recnx_fsm.c over the SHIPPING vms_cnxman_csb.c
 *      ladder. Nothing here hand-sets a CSB state: the states below are
 *      whatever those two files decided, and the expiry happens because the
 *      virtual clock really passed the deadline they computed from SYSGEN's
 *      RECNXINTERVAL.
 *   2. TIME IS THE SIMULATOR'S VIRTUAL CLOCK, so "it recovered" is a BOUNDED
 *      number of beats rather than a hope, and "it never recovered" is
 *      expressible: the pre-fix behaviour is an unbounded stall, and the
 *      assertion below is a bound.
 *   3. THE ASSERTION IS THE ORDERED SEQUENCE of what the node's own console
 *      said and what its own CLUB looked like, compared element by element.
 *
 * WHAT THE HARNESS OWNS. The PORT (which systems this node has circuits to)
 * and the ORDER OF THE ONCE-A-SECOND BEAT -- both of which live in
 * vms_cnxman.c, the glue TU that names exec_kbackend.h and is therefore not
 * host-linkable (test_cnxman_glue.c's own header note). Those two are the only
 * things modelled here, they are modelled in ten lines, and every decision
 * they feed is the shipping FSMs'.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_cnxman_join_fsm.h"

#define MEMBER_SYSID 0x000004000101ull   /* the real VAX in the capture   */
#define OWN_SYSID    0x000004000103ull
#define MEMBER_CSID  0x00010001u
#define MSCP_CONID   0x4e620008u
#define ACC_CM_CONID 0x4e62000au         /* the connection the MEMBER opened */
#define SIM_NODE     0u
#define RECNXINTERVAL_SECS 20u           /* the OpenVMS default, as measured */

/* ==========================================================================
 * The bed
 * ========================================================================== */
struct bed {
	struct sim_clock       clock;
	struct vms_cluster     cl;
	struct cnxman_ops      ops;
	struct cnxman_join_ops jops;
	struct cnxman_join     j;
	struct cnxman_barrier  b;
	struct cnxman_recnx    recnx;

	/* THE PORT MODEL: the systems this node has an open circuit to. In the
	 * executive this is vms_scs_peer_at()'s answer, read off real received
	 * frames; here it is the scenario's own statement about its LAN. */
	int      circuit_to_member;

	uint32_t beats;
	uint32_t cm_connects_issued;   /* VMS$VAXcluster CONNECT_REQs emitted */
	uint32_t logs;
	char     last_log[160];
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
	g.logs++;
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
	return 0;   /* the poller asks; this member answers below, or not */
}

static int bed_connect(void *ctx, vms_scs_sysid_t dst,
		       const uint8_t *local_name, const uint8_t *remote_name,
		       const uint8_t *conndata, uint16_t credits,
		       vms_conid_t *out_conid)
{
	(void)ctx; (void)dst; (void)local_name; (void)conndata; (void)credits;
	if (memcmp(remote_name, cnxman_join_name_mscp_disk,
		   VMS_SCS_PROCNAME_LEN) == 0) {
		*out_conid = MSCP_CONID;
		return 0;
	}
	/* The capture's shape: this node's own VMS$VAXcluster connect never
	 * completes -- the member is the one that opens the pair's connection.
	 * Counted, so a fix that hammered the peer would be visible. */
	g.cm_connects_issued++;
	return -1;
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

static void bed_init(void)
{
	struct cnxman_join_cfg cfg;

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

	(void)cnxman_club_init(&g.cl);
	cnxman_barrier_init(&g.b, &g.cl, &g.ops);
	cnxman_join_init(&g.j, &g.cl, &g.ops, &g.jops);
	cnxman_join_set_barrier(&g.j, &g.b);
	cnxman_recnx_init(&g.recnx, &g.cl, &g.ops);

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.model, "OVMX simulated node", 19);
	cfg.model_len = 19;
	cfg.model_valid = 1;
	memcpy(cfg.version, "VMX V0.6", 8);
	cfg.version_valid = 1;
	cnxman_join_set_cfg(&g.j, &cfg);

	g.circuit_to_member = 1;   /* the PE circuit is up, and stays up */
}

/* ==========================================================================
 * THE ONCE-A-SECOND BEAT, in vms_cnxman.c's order
 *
 * reclaim -> discover -> drive the join -> run the reconnect ladder. Ten lines
 * of harness; every decision inside them is a shipping FSM's.
 * ========================================================================== */

static struct vms_csb *member_csb(void)
{
	return cnxman_club_find_sysid(&g.cl.club, MEMBER_SYSID);
}

/* cnxman_reclaim_abandoned_csbs() */
static void beat_reclaim(void)
{
	vms_scs_sysid_t released[VMS_CLUB_MAX_CSB];
	uint32_t n, i;

	n = cnxman_club_reclaim_abandoned(&g.cl.club, released,
					  (uint32_t)VMS_CLUB_MAX_CSB);
	for (i = 0; i < n; i++)
		cnxman_join_target_released(&g.j, released[i]);
}

/* cnxman_discover_peers(): a block for every system the port reports a circuit
 * to that the CLUB has none for. */
static void beat_discover(void)
{
	if (!g.circuit_to_member || member_csb() != NULL)
		return;
	(void)cnxman_club_alloc_csb(&g.cl.club, MEMBER_SYSID, 1);
}

/* cnxman_join_drive(): idle join + a system present -> start an attempt. */
static void beat_drive_join(void)
{
	if (g.j.state != (uint8_t)CNXMAN_JOIN_IDLE)
		return;
	if (member_csb() == NULL)
		return;
	(void)cnxman_join_start(&g.j);
}

/* The reconnect ladder's own tick, and the ONE action this scenario's port can
 * satisfy: an attempt that goes out and is never answered. */
static void beat_recnx(void)
{
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];
	uint32_t n = cnxman_recnx_tick(&g.recnx, recs, VMS_CLUB_MAX_CSB);
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (recs[i].action == (uint8_t)CNXMAN_CSB_ACT_RECONNECT)
			g.cm_connects_issued++;
	}
}

static void beat(void)
{
	g.clock.now_ms += CNXMAN_RECNX_ATTEMPT_MS;
	g.beats++;
	beat_reclaim();
	beat_discover();
	beat_drive_join();
	beat_recnx();

	/* ... and whatever join watchdogs the clock now owes. */
	for (;;) {
		struct sim_timer t;
		uint32_t slot;

		if (!sim_clock_next(&g.clock, &slot))
			break;
		if (g.clock.t[slot].due_ms > g.clock.now_ms)
			break;
		if (!sim_clock_fire(&g.clock, slot, &t))
			break;
		if (t.which == (uint8_t)CNXMAN_TIMER_JOIN)
			cnxman_join_timer(&g.j);
	}
}

/* ==========================================================================
 * The replay: reach the capture's [88.69], then lose it at [89.54]
 * ========================================================================== */

static void drive_to_the_members_connection(void)
{
	struct vms_csb *csb;

	bed_init();
	beat();                       /* discovery + the first attempt        */
	ct_check_eq_u32(g.j.state, CNXMAN_JOIN_DIR_ROUND,
			"the attempt starts at p. 2-51's directory round");

	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_mscp_disk,
			       1);
	cnxman_join_dir_result(&g.j, MEMBER_SYSID, cnxman_join_name_vaxcluster,
			       1);
	cnxman_join_opened(&g.j, MSCP_CONID);

	/* [88.69] the MEMBER opens the pair's VMS$VAXcluster connection, and
	 * the glue binds the Con.ID SCS minted into that system's block. */
	csb = member_csb();
	ct_check(csb != NULL, "the member has a block");
	cnxman_csb_set_csid(csb, MEMBER_CSID);
	(void)cnxman_csb_dispatch(&g.cl.club, csb, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g.ops);
	cnxman_csb_bind_connection(csb, ACC_CM_CONID);
	(void)cnxman_recnx_connectivity_gained(&g.recnx, csb);
	cnxman_join_cm_accepted(&g.j, MEMBER_SYSID, ACC_CM_CONID);
	cnxman_join_opened(&g.j, ACC_CM_CONID);

	ct_check_eq_u32(csb->state, (uint8_t)VMS_CNXMAN_CSB_OPEN,
			"[88.69] the cluster's connection to this node is OPEN");
	ct_check_eq_u32(g.j.cm_adopted, 1u, "... and the join adopted it");
}

static void lose_it_before_admission(void)
{
	struct vms_csb *csb = member_csb();

	/* [89.54] SCS closes the CDT. Not a last gasp: p. 7-30's window opens. */
	(void)cnxman_recnx_connectivity_lost(&g.recnx, csb, 0);
	cnxman_join_closed(&g.j, ACC_CM_CONID, 0u);

	ct_check_eq_u32(csb->state, (uint8_t)VMS_CNXMAN_CSB_WAIT,
			"[89.54] the ladder starts the reconnect window");
	ct_check_eq_u32(g.j.state, (uint8_t)CNXMAN_JOIN_VC_CONNECT,
			"[89.55] and the join waits for its connection back");
	ct_check_eq_u32(cnxman_join_handed_off(&g.j), 0,
			"INV-6: this node is NOT a member and says so");
}

/* ==========================================================================
 * The cases
 * ========================================================================== */

static void test_the_window_really_runs_and_really_ends(void)
{
	struct vms_csb *csb;
	uint32_t start_beats;
	int claimed_membership = 0;

	printf("\n-- the p. 7-30 window, run beat by beat on the clock --\n");
	drive_to_the_members_connection();
	lose_it_before_admission();
	start_beats = g.beats;

	/* Nineteen seconds of it: the ladder keeps attempting, and NOTHING
	 * claims a membership while it does. */
	while (g.beats - start_beats < 19u) {
		beat();
		if (cnxman_join_handed_off(&g.j))
			claimed_membership = 1;
	}
	ct_check(!claimed_membership,
		 "INV-6: nineteen seconds of waiting invents no membership");
	csb = member_csb();
	ct_check(csb != NULL && csb->state != (uint8_t)VMS_CNXMAN_CSB_DISCONNECT,
		 "19 s in, the member is STILL being waited for -- p. 7-30's "
		 "'do not presume that the remote system has left'");
	ct_check(csb != NULL && csb->attempts > 0u,
		 "... and the ladder really attempted, once a second");

	/* The beat that steps past the deadline the ladder computed from
	 * SYSGEN's RECNXINTERVAL. Nothing here says 20: the clock does. */
	beat();
	csb = member_csb();
	ct_check(csb != NULL &&
		 csb->state == (uint8_t)VMS_CNXMAN_CSB_DISCONNECT,
		 "[110.04] the window expired and the ladder gave the "
		 "connection up");
	ct_check(csb != NULL && csb->cdt_conid == 0u,
		 "... and the block stopped claiming a connection it does not "
		 "have");

	/* ... and the NEXT beat is where p. 7-25 happens: deallocate, then
	 * rebuild for a system the port still has a circuit to. */
	beat();
	csb = member_csb();
	ct_check(csb != NULL && csb->state == (uint8_t)VMS_CNXMAN_CSB_NEW,
		 "the block is deallocated and built again, in p. 7-23's NEW "
		 "-- which is where the capture's 25 minutes of silence began");
}

static void test_the_node_recovers_and_the_recovery_is_bounded(void)
{
	uint32_t start_beats, recovered_at = 0u;
	struct vms_csb *csb;
	int claimed_membership = 0;

	printf("\n-- vms-dfe: and then it asks again (the 25 minutes of "
	       "silence that did not) --\n");
	drive_to_the_members_connection();
	lose_it_before_admission();
	start_beats = g.beats;

	/* Run well past the window. The node must be ASKING again, through the
	 * same member, over a block the executive rebuilt. */
	while (g.beats - start_beats < 64u) {
		beat();
		if (recovered_at == 0u &&
		    g.j.state == (uint8_t)CNXMAN_JOIN_DIR_ROUND &&
		    g.j.target_valid && g.j.target_sysid == MEMBER_SYSID &&
		    g.j.joins_started >= 2u)
			recovered_at = g.beats - start_beats;
		if (cnxman_join_handed_off(&g.j))
			claimed_membership = 1;
	}
	ct_check(!claimed_membership,
		 "INV-6: not one beat of the recovery claims a membership the "
		 "cluster never granted");

	ct_check(recovered_at != 0u,
		 "the node runs a FRESH admission attempt through the member it "
		 "lost -- the whole of what the capture never did");
	printf("     recovered %u beats (~%u s) after the connection was lost; "
	       "the window is %u s\n",
	       recovered_at,
	       (recovered_at * CNXMAN_RECNX_ATTEMPT_MS) / 1000u,
	       RECNXINTERVAL_SECS);
	ct_check(recovered_at <= 24u,
		 "... within one reconnect window and change, not eventually");

	csb = member_csb();
	ct_check(csb != NULL, "the member's block exists again");
	ct_check(csb != NULL && csb->csid_valid == 0u,
		 "INV-6: the REBUILT block claims no CSID -- p. 7-25 gives a "
		 "returning system a NEW one, never its old one back");
	ct_check(csb != NULL &&
		 (csb->flags & (VMS_CSB_F_MEMBER | VMS_CSB_F_SELECTED)) == 0u,
		 "... and no membership");
	ct_check_eq_u32(g.cl.club.csb_reclaimed, 1u,
			"exactly one block was deallocated (p. 7-25), not a "
			"table swept every beat");
	ct_check_eq_u32(cnxman_club_local(&g.cl.club) != NULL, 1u,
			"and this node's own block was never touched");
}

static void test_the_retry_does_not_hammer_the_peer(void)
{
	uint32_t start_beats, connects_at_loss;

	printf("\n-- and it does not dial the peer once a second to do it --\n");
	drive_to_the_members_connection();
	lose_it_before_admission();
	start_beats = g.beats;
	connects_at_loss = g.cm_connects_issued;

	while (g.beats - start_beats < 64u)
		beat();

	/*
	 * E81 measured a reference VAX bugchecking CNXMGRERR ~470 ms after a
	 * SECOND VMS$VAXcluster CONNECT_REQ, 15 times out of 15. OVMX never
	 * gets to make that decision for a peer (the never-crash-a-peer rule),
	 * so a recovery that retried by dialling every second would be a worse
	 * bug than the stall. The bound here is the reconnect window's own
	 * once-a-second cadence for its 20 s, and after that the directory
	 * round -- which only a member that is really ANSWERING completes --
	 * gates every further connect. This bed's member answers nothing after
	 * the loss, so the count must stop climbing.
	 */
	printf("     %u VMS$VAXcluster connects in %u beats after the loss "
	       "(window = %u s at %u ms)\n",
	       g.cm_connects_issued - connects_at_loss, 64u,
	       RECNXINTERVAL_SECS, (unsigned)CNXMAN_RECNX_ATTEMPT_MS);
	ct_check(g.cm_connects_issued - connects_at_loss <=
		 RECNXINTERVAL_SECS + 2u,
		 "the connects this node puts on the wire are bounded by the "
		 "reconnect window, not by the length of the run");
}

int main(void)
{
	printf("cnxman_recover_after_giveup (rd vms-dfe, rung R2): the measured "
	       "in-browser stall\n");
	test_the_window_really_runs_and_really_ends();
	test_the_node_recovers_and_the_recovery_is_bounded();
	test_the_retry_does_not_hammer_the_peer();
	return ct_summary("cnxman_recover_after_giveup");
}
