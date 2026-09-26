/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/cnxman_never_admitted_not_removed.c - rd vms-b36's rung-R2 leg.
 *
 * WHAT IS BEING REPRODUCED, AND WHERE IT WAS MEASURED.
 * tests/lab/captures/vms-b36-cnxmgrerr-20260925/ -- a three-node bench
 * cluster: a real OpenVMS VAX V7.3 (SIMH) founds, one OVMX node is admitted,
 * a SECOND OVMX node starts its admission and its LAN goes dark inside the
 * transition. The real VAX's own console, verbatim:
 *
 *   %CNXMAN,  received VAXcluster membership request from system OVMXB
 *   %CNXMAN,  proposing addition of system OVMXB
 *   %CNXMAN,  lost connection to system OVMXB
 *   %CNXMAN,  timed-out lost connection to system OVMXB
 *   %CNXMAN,  aborting VAXcluster state transition
 *   **** Fatal BUG CHECK, version = V7.3     CNXMGRERR, ...
 *
 * ...and the ADMITTED OVMX node's console, in the same second:
 *
 *   [ 84.57] %CNXMAN, the cluster abandoned the state transition this node ...
 *   [ 85.17] %CNXMAN, reconnect interval expired, proposing removal
 *   [ 85.18] %CNXMAN, proposing removal of a system from the cluster
 *   [ 85.21] vms: SCS path lost to system 0:1989          <- the VAX is gone
 *
 * That node had never had OVMXB in its membership: its own transcript names
 * only 0x7c3 (itself) and 0x7c5 (the VAX) as added to the cluster. It proposed
 * a cluster reconfiguration REMOVING a system the cluster had never admitted,
 * milliseconds after the coordinator abandoned that very system's admission.
 *
 * THE RULE IT BROKE is p. 7-49's: the cluster's committed membership is the
 * set of CSBs with SELECTED set, written at a transition's Phase 2. A system
 * that is not in the cluster cannot be removed from it.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1 (the same three things
 * scenarios/cnxman_recover_after_giveup.c lists):
 *
 *   1. THE WHOLE p. 7-30 WINDOW IS REALLY RUN, beat by beat, by the SHIPPING
 *      vms_cnxman_recnx_fsm.c over the SHIPPING vms_cnxman_csb.c ladder. No
 *      CSB state is hand-set; the expiry happens because the virtual clock
 *      really passed the deadline those files computed from SYSGEN's
 *      RECNXINTERVAL.
 *   2. TIME IS THE SIMULATOR'S VIRTUAL CLOCK, so "it proposed nothing" is a
 *      statement about a whole bounded window rather than about one call.
 *   3. THE PROPOSAL IS FOLLOWED THROUGH INTO THE SHIPPING COORDINATOR FSM,
 *      which is what would have put the class-0x03 open on the wire, and the
 *      assertion is that NOTHING was emitted.
 *
 * WHAT THE HARNESS OWNS: the port model (which systems have circuits) and the
 * order of the once-a-second beat, both of which live in vms_cnxman.c -- the
 * glue TU that names exec_kbackend.h and is not host-linkable. Every decision
 * they feed is a shipping FSM's.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_recnx_fsm.h"
#include "vms_cnxman_coord_fsm.h"

/* The bench rig's own identities (SCSSYSTEMID), so the transcript above and
 * this scenario name the same three systems. */
#define VAX_SYSID    1989ull            /* VAXC, the real VAX, the coordinator */
#define OWN_SYSID    1987ull            /* OVMXA, admitted                     */
#define JOINER_SYSID 1988ull            /* OVMXB, never admitted               */
#define OWN_CSID     0x00010003u
#define VAX_CSID     0x00010001u
#define PEER_SYSID   1990ull            /* a second admitted member, so the  */
#define PEER_CSID    0x00010002u        /* control's 12 x (M-1) is not zero  */
#define SIM_NODE     0u
#define RECNXINTERVAL_SECS 20u

struct bed {
	struct sim_clock    clock;
	struct vms_cluster  cl;
	struct cnxman_ops   ops;
	struct cnxman_recnx recnx;
	struct cnxman_coord coord;

	int      circuit_to_joiner;   /* the port model */
	uint32_t beats;
	uint32_t frames_sent;         /* anything the coordinator put on a VC */
	uint32_t proposals_driven;    /* CNXMAN_CSB_ACT_PROPOSE_TRANSITION     */
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

/* The ONE thing that matters here: did anything go out? */
static int bed_send(void *ctx, vms_conid_t conid, const uint8_t *body,
		    uint32_t len)
{
	(void)ctx; (void)conid; (void)body; (void)len;
	g.frames_sent++;
	return 0;
}

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx; (void)body; (void)len;
	g.frames_sent++;
	return 0;
}

/* ---- the bed ------------------------------------------------------------- */

/*
 * The members of this scenario's cluster all run THIS implementation, and they
 * say so the only way a system can: the software token their own formation
 * body carried (rd vms-1ee). Without it the CONTROL below is asking a node to
 * open a transition for a connection manager it cannot build one for, which
 * rd vms-0f9 now -- correctly -- refuses.
 */
#define BED_SWVER "OVMXV07\0"

static struct vms_csb *bed_add_member(vms_scs_sysid_t sysid, vms_csid_t csid)
{
	struct vms_csb *csb = cnxman_club_alloc_csb(&g.cl.club, sysid, 1);

	if (csb == NULL)
		return NULL;
	cnxman_csb_set_swver(csb, (const uint8_t *)BED_SWVER,
			     (uint8_t)(sizeof(BED_SWVER) - 1u),
			     (const uint8_t *)BED_SWVER,
			     (uint8_t)(sizeof(BED_SWVER) - 1u));
	cnxman_csb_set_csid(csb, csid);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	return csb;
}

static void bed_init(void)
{
	struct vms_csb *local;

	memset(&g, 0, sizeof(g));
	sim_clock_init(&g.clock, SIM_VMS_ORIGIN);

	g.ops.arm_timer = bed_arm;
	g.ops.cancel_timer = bed_cancel;
	g.ops.now_ms = bed_now_ms;
	g.ops.log = bed_log;
	g.ops.send = bed_send;
	g.ops.respond = bed_respond;

	memcpy(g.cl.params.scsnode, "OVMXA0", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = OWN_SYSID;
	g.cl.params.vaxcluster = 2;
	g.cl.params.votes = 1;
	g.cl.params.recnxinterval = (uint16_t)RECNXINTERVAL_SECS;

	local = cnxman_club_init(&g.cl);
	cnxman_club_learn_local_csid(&g.cl.club, OWN_CSID);
	cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
					       VMS_CSB_F_MEMBER));
	g.cl.state = VMS_CLUSTER_MEMBER;

	/*
	 * The cluster this node really is in: itself, the real VAX, and one
	 * more member. The third member is what makes the CONTROL meaningful --
	 * with only one peer, removing it is the degenerate 12 x (M-1) = 0 case
	 * that completes locally and emits nothing, which would prove nothing
	 * about whether a removal still reaches the wire.
	 */
	(void)bed_add_member(VAX_SYSID, VAX_CSID);
	(void)bed_add_member(PEER_SYSID, PEER_CSID);

	cnxman_recnx_init(&g.recnx, &g.cl, &g.ops);
	cnxman_coord_init(&g.coord, &g.cl, &g.ops);
}

/* ==========================================================================
 * THE ONCE-A-SECOND BEAT, in vms_cnxman.c's order: discover, then run the
 * reconnect ladder, then hand every PROPOSE action to the coordinator exactly
 * as cnxman_recnx_apply() does.
 * ========================================================================== */

static struct vms_csb *joiner_csb(void)
{
	return cnxman_club_find_sysid(&g.cl.club, JOINER_SYSID);
}

static void beat_discover(void)
{
	if (!g.circuit_to_joiner || joiner_csb() != NULL)
		return;
	(void)cnxman_club_alloc_csb(&g.cl.club, JOINER_SYSID, 1);
}

static void beat_recnx(void)
{
	struct cnxman_recnx_rec recs[VMS_CLUB_MAX_CSB];
	uint32_t n = cnxman_recnx_tick(&g.recnx, recs, VMS_CLUB_MAX_CSB);
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (recs[i].action != (uint8_t)CNXMAN_CSB_ACT_PROPOSE_TRANSITION)
			continue;
		g.proposals_driven++;
		(void)cnxman_coord_propose_remove(&g.coord,
						  (int32_t)recs[i].csb_index);
	}
}

static void beat(void)
{
	g.clock.now_ms += CNXMAN_RECNX_ATTEMPT_MS;
	g.beats++;
	beat_discover();
	beat_recnx();
}

/* ==========================================================================
 * The replay
 * ========================================================================== */

/* Discovery gives the joiner a block (p. 7-23 NEW), its connection comes up
 * and then goes -- and at no point does any transition SELECT it. */
static struct vms_csb *bring_the_joiner_up_then_lose_it(void)
{
	struct vms_csb *csb;

	g.circuit_to_joiner = 1;
	beat();
	csb = joiner_csb();
	ct_check(csb != NULL, "discovery allocated a block for the joiner");
	ct_check_eq_u32(csb->flags & VMS_CSB_F_SELECTED, 0,
			"INV-6: discovery does NOT make it a member (p. 7-49)");
	/* ...and a SUBJECT that has proved nothing must not be what blocks the
	 * control below: rd vms-0f9's removal gate excludes the subject. */
	cnxman_csb_set_swver(csb, (const uint8_t *)BED_SWVER,
			     (uint8_t)(sizeof(BED_SWVER) - 1u),
			     (const uint8_t *)BED_SWVER,
			     (uint8_t)(sizeof(BED_SWVER) - 1u));

	(void)cnxman_csb_dispatch(&g.cl.club, csb, CNXMAN_CSB_EV_CONNECT_RCVD,
				  &g.ops);
	cnxman_csb_bind_connection(csb, 0x4e62000au);
	(void)cnxman_recnx_connectivity_gained(&g.recnx, csb);
	ct_check_eq_u32(csb->state, (uint8_t)VMS_CNXMAN_CSB_OPEN,
			"the pair's VMS$VAXcluster connection is OPEN");

	/* The LAN goes dark inside the coordinator's transition. */
	(void)cnxman_recnx_connectivity_lost(&g.recnx, csb, 0);
	ct_check_eq_u32(csb->state, (uint8_t)VMS_CNXMAN_CSB_WAIT,
			"p. 7-30's reconnect window opens");
	return csb;
}

static void test_window_expires_and_nothing_is_proposed(void)
{
	struct vms_csb *csb;
	uint32_t start;

	printf("\n-- rd vms-b36: the window expires on a system the cluster "
	       "never admitted --\n");
	bed_init();
	csb = bring_the_joiner_up_then_lose_it();
	start = g.beats;

	/* Run the whole window and three beats past it. */
	while (g.beats - start < RECNXINTERVAL_SECS + 3u)
		beat();

	csb = joiner_csb();
	ct_check(csb != NULL &&
		 csb->state == (uint8_t)VMS_CNXMAN_CSB_DISCONNECT,
		 "the window really expired and the block gave up");
	ct_check_eq_u32(g.proposals_driven, 0,
			"NOT ONE removal was proposed across the whole window");
	ct_check_eq_u32(csb->transitions_proposed, 0,
			"...and the block counted none");
	ct_check(csb != NULL && csb->removals_withheld >= 1u,
		 "...and the withholding is on the record, not silent");
	ct_check_eq_u32(g.frames_sent, 0,
			"NOTHING reached the wire: no class-0x03 open at the "
			"coordinator that had just abandoned this system");
	ct_check_eq_u32(g.cl.club.transition_active, 0,
			"and no transition was opened");
	ct_check_eq_u32(g.coord.not_admitted + g.coord.refusals, 0,
			"the coordinator was never even asked -- the ladder "
			"settled it first");
}

/*
 * THE POSITIVE CONTROL, on the SAME bed and the SAME beat: when the system
 * that goes IS a committed member, p. 7-30's reconfiguration still happens and
 * still reaches the wire. Without this the test above would pass on an
 * executive that had simply stopped proposing anything.
 */
static void test_a_real_member_is_still_removed(void)
{
	struct vms_csb *vax;
	uint32_t start;

	printf("\n-- CONTROL: a COMMITTED member's window still ends in a "
	       "reconfiguration --\n");
	bed_init();
	vax = cnxman_club_find_sysid(&g.cl.club, VAX_SYSID);
	ct_check(vax != NULL, "the member has a block");
	cnxman_csb_bind_connection(vax, 0x4e62000bu);
	(void)cnxman_recnx_connectivity_lost(&g.recnx, vax, 0);
	start = g.beats;

	while (g.beats - start < RECNXINTERVAL_SECS + 3u)
		beat();

	ct_check_eq_u32(g.proposals_driven, 1,
			"exactly one removal was proposed (p. 7-30)");
	ct_check(g.frames_sent > 0u,
		 "...and the coordinator really put it on the wire");
	ct_check_eq_u32(cnxman_club_find_sysid(&g.cl.club,
					       VAX_SYSID)->removals_withheld, 0,
			"nothing was withheld for a real member");
}

int main(void)
{
	printf("=== cnxman_never_admitted_not_removed: rd vms-b36 on the "
	       "virtual clock ===\n");
	test_window_expires_and_nothing_is_proposed();
	test_a_real_member_is_still_removed();
	return ct_summary("cnxman_never_admitted_not_removed");
}
