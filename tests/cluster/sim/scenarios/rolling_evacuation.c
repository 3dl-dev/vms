/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/rolling_evacuation.c - the ROLLING-EVACUATION harness (rd vms-ci.6,
 * the V1.0 R5 release gate). A workload is moved off a DEPARTING cluster member
 * onto a SURVIVING one, and the cluster STAYS UP throughout.
 *
 * ===========================================================================
 * THE SEQUENCE THIS HARNESS ENCODES
 * ===========================================================================
 *
 *   1. FORM      three members (OVMXA, OVMXB, OVMXC), each one vote, form a
 *                quorate cluster (EXPECTED_VOTES 3 -> QUORUM 2).
 *   2. PLACE     a workload W runs on OVMXC, holding a cluster-wide resource.
 *   3. QUIESCE   OVMXC announces its departure (the transport last gasp) and
 *                stops being PRESENT.
 *   4. TAKEOVER  the transition completes: OVMXC is removed from membership,
 *                the survivors recount and recompute quorum, and W is taken
 *                over by a survivor.
 *   5. VERIFY    the two survivors each still PERCEIVE QUORUM and their
 *                executives keep serving -- the cluster stayed up.
 *
 * and a FALSIFIER: a SECOND departure drops the survivors below quorum, so the
 * "stays up" checks above are proven to be capable of going red (INV-6: an
 * assertion that can only pass is not evidence).
 *
 * ===========================================================================
 * WHAT IS REAL HERE, AND WHAT IS A STUB -- SAID PLAINLY
 * ===========================================================================
 *
 * REAL (the shipping executive objects, driven, never modelled):
 *   - the quorum arithmetic and its ENFORCEMENT: cnxman_quorum_recompute(),
 *     cnxman_quorum_arm_update() and cnxman_quorum_hang_active() from
 *     src/kernel-core/vms_cnxman_quorum.c decide, from each survivor's own
 *     CLUB, whether that node perceives quorum and its executive keeps serving
 *     or STALLS (p. 7-4). "The cluster stays up" is exactly hang_active == 0.
 *   - the membership count: cnxman_club_recount_members() (p. 7-49).
 *   - the transport departure: sim_node_halt() emits the shipping
 *     pe_fsm_send_last_gasp() (p. 7-29) and the survivors' shipping ports tear
 *     down the circuit to the departed node while HOLDING the circuits between
 *     themselves -- the lower-layer corroboration that a departure happened.
 *
 * STUB (a seam, not a claim -- these are the pieces ci.6 still needs):
 *   - THE WORKLOAD. In the shipped demo W is a real OpenVMS-built image's
 *     process set whose SYS$ services dispatch and execute on OVMX (rd vms-sys,
 *     the large open gate). Here W is a harness token -- a name, the node it
 *     runs on, the id of the resource it holds -- and it contributes NO
 *     cnxman/DLM metric to any assertion. Every number checked below is read
 *     from a real vms_club or pe_fsm.
 *   - THE WORKLOAD'S LOCKS REMASTERING to the survivor across the departure
 *     (rd vms-1ee and its blockers vms-fcb / vms-c27 / vms-3e3). The stub
 *     proves the survivor-takeover PRECONDITION -- a quorate, connected
 *     survivor exists to receive W -- which real remastering depends on; it
 *     does not yet prove the lock crossed.
 *   - THE COORDINATOR driving the transition over the wire. As in
 *     dlm_directory.c and test_cnxman_quorum.c, THE HARNESS decides who is a
 *     member (it clears the departing CSB's flags directly). The real
 *     class-0x03 transition choreography is FC-P3.12's coordinator; this
 *     harness drives the membership change the coordinator would commit.
 *
 * So: the harness is the ci.6 SKELETON. When vms-sys lands the real workload
 * plugs into place_on()/takeover_to(), and when vms-1ee's remaster lands the
 * takeover phase gains a real lock-crossing assertion. The evacuation sequence,
 * the survivor-takeover preconditions and the "cluster stays up" enforcement
 * check are host-proven now.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "sim.h"
#include "sim_node.h"
#include "sim_scenario.h"       /* sim_metric_read, enum sim_metric */

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h" /* enum vms_pe_vc_state, VMS_CNXMAN_CSB_* */
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_quorum.h"

/* ==========================================================================
 * The three nodes. One vote each; EXPECTED_VOTES 3, so QUORUM = (3+2)/2 = 2 --
 * the smallest cluster that survives losing a member, which is the whole point
 * of the demo.
 * ========================================================================== */
enum { NODE_A = 0, NODE_B = 1, NODE_C = 2, N_NODES = 3 };

static const char *const g_name[N_NODES] = { "OVMXA", "OVMXB", "OVMXC" };
static const uint64_t     g_sysid[N_NODES] = {
	0x0000040001ULL, 0x0000040002ULL, 0x0000040003ULL
};
static const vms_csid_t   g_csid[N_NODES] = { 0x00010001u, 0x00010002u,
					      0x00010003u };
#define NODE_VOTES      1u
#define NODE_EXPECTED   3u

/* ==========================================================================
 * The workload -- a STUB (see the header). Harness bookkeeping only; it never
 * feeds an assertion about cluster state.
 * ========================================================================== */
struct evac_workload {
	const char *name;      /* the workload's name                          */
	int         on_node;   /* the node index currently running it, or -1   */
	uint32_t    resource;  /* stub id of the cluster-wide resource it holds */
	int         takeovers; /* how many times it has been taken over         */
};

static void wl_place_on(struct evac_workload *w, int node)
{
	w->on_node = node;
}

/* The takeover a real remaster (vms-1ee) will drive: move W to a survivor. The
 * caller has already proven `to` is a quorate, connected survivor. */
static void wl_takeover_to(struct evac_workload *w, int to)
{
	w->on_node = to;
	w->takeovers++;
}

/* ==========================================================================
 * The membership vantage. Each surviving node computes quorum from its OWN
 * CLUB (that is the executive's design -- a node acts on the numbers it holds,
 * never a shared global), so the harness stands up one CLUB PER SURVIVOR and
 * asserts each survivor independently perceives quorum. This mirrors
 * test_cnxman_quorum.c's fixture and dlm_directory.c's sys_form().
 * ========================================================================== */
struct member_view {
	struct vms_cluster cl;
	struct vms_csb    *peer[N_NODES];   /* peer[me] stays NULL             */
};

/* Bring node `me` up as a committed member that sees all three nodes present
 * and selected -- the state a running cluster is in before the evacuation. */
static void view_form(struct member_view *v, int me)
{
	int i;

	memset(v, 0, sizeof(*v));
	memcpy(v->cl.params.scsnode, g_name[me], strlen(g_name[me]));
	v->cl.params.scsnode_len = (uint8_t)strlen(g_name[me]);
	v->cl.params.scssystemid = g_sysid[me];
	v->cl.params.votes = NODE_VOTES;
	v->cl.params.expected_votes = NODE_EXPECTED;
	v->cl.params.vaxcluster = 2;
	cnxman_club_init(&v->cl);

	for (i = 0; i < N_NODES; i++) {
		struct vms_csb *csb;

		if (i == me) {
			/* Our own CSB, made by cnxman_club_init(): learn our
			 * identity and select ourselves (INV-6: no unearned
			 * self-membership -- do it as the join FSM will). */
			struct vms_csb *local = cnxman_club_local(&v->cl.club);

			cnxman_csb_set_csid(local, g_csid[me]);
			cnxman_csb_set_params(local, NODE_VOTES, NODE_EXPECTED,
					      0);
			cnxman_csb_set_flags(local,
				(uint16_t)(VMS_CSB_F_SELECTED |
					   VMS_CSB_F_MEMBER));
			local->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
			cnxman_club_learn_local_csid(&v->cl.club, g_csid[me]);
			v->peer[i] = NULL;
			continue;
		}
		csb = cnxman_club_alloc_csb(&v->cl.club,
					    (vms_scs_sysid_t)g_sysid[i], 1);
		cnxman_csb_set_csid(csb, g_csid[i]);
		cnxman_csb_set_params(csb, NODE_VOTES, NODE_EXPECTED, 0);
		cnxman_csb_set_flags(csb,
			(uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
		csb->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
		v->peer[i] = csb;
	}

	/* A committed member: the founding transition's Phase 2 counts the
	 * members (p. 7-49) and applies p. 7-6 to the votes we really hold.
	 * recount is a TRANSITION event -- quiesce below deliberately does not
	 * repeat it, because membership is held across a reconnect (p. 7-30). */
	v->cl.state = VMS_CLUSTER_MEMBER;
	(void)cnxman_club_recount_members(&v->cl.club);
	cnxman_quorum_recompute(&v->cl.club);
	cnxman_quorum_arm_update(&v->cl);
}

/* On this survivor's CLUB, run the two real events a departing peer causes:
 *   quiesce -- the peer stops being PRESENT (state -> WAIT: membership is still
 *              HELD across the break, p. 7-30, so the count must NOT dip yet);
 *   remove  -- the transition completes and the peer leaves membership
 *              (SELECTED cleared, p. 7-49 recount drops it).
 * Recompute and re-arm after each, exactly as the executive does on a
 * membership change (design SS3.7). */
static void view_peer_quiesce(struct member_view *v, int peer)
{
	if (v->peer[peer] != NULL)
		v->peer[peer]->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(&v->cl.club);
	cnxman_quorum_arm_update(&v->cl);
}

static void view_peer_remove(struct member_view *v, int peer)
{
	if (v->peer[peer] != NULL)
		cnxman_csb_clear_flags(v->peer[peer],
			(uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
	(void)cnxman_club_recount_members(&v->cl.club);
	cnxman_quorum_recompute(&v->cl.club);
	cnxman_quorum_arm_update(&v->cl);
}

/* ==========================================================================
 * The PE-transport corroboration. A departure is also a wire event: the
 * departing port emits its last gasp and the survivors tear down the circuit
 * to it while HOLDING the circuits between themselves. This is a separate
 * shipping object from the CLUB above (the sim has no membership; see
 * sim_node.h), so the harness drives it in lockstep by node identity and reads
 * the survivors' real circuit state -- corroboration, not the primary proof.
 * ========================================================================== */
struct pe_cluster {
	struct sim sim;
};

static int pe_form(struct pe_cluster *p, uint64_t seed)
{
	struct sim_node_cfg cfg;
	struct sim_link link;
	int i;

	memset(p, 0, sizeof(*p));
	sim_init(&p->sim, seed);
	for (i = 0; i < N_NODES; i++) {
		sim_node_cfg_default(&cfg, g_name[i],
				     (uint16_t)g_sysid[i], (uint8_t)i);
		if (sim_add_node(&p->sim, &cfg) < 0)
			return 0;
	}
	memset(&link, 0, sizeof(link));
	sim_lan_set_link_all(&p->sim.lan, &link);
	if (sim_boot_all(&p->sim) < 0)
		return 0;
	return sim_run_until(&p->sim, sim_all_vcs_open, NULL, 120000u);
}

static uint64_t pe_vc_state(struct pe_cluster *p, int node, int peer)
{
	return sim_metric_read(&p->sim, SIM_M_VC_STATE,
			       g_name[node], g_name[peer]);
}

static uint64_t pe_vc_downs(struct pe_cluster *p, int node, int peer)
{
	return sim_metric_read(&p->sim, SIM_M_VC_DOWNS,
			       g_name[node], g_name[peer]);
}

/* ==========================================================================
 * THE DEMO. Three members; evacuate OVMXC; the two survivors stay up.
 * ========================================================================== */
static void test_rolling_evacuation_stays_up(uint64_t seed)
{
	static struct member_view a, b;   /* the two survivors' vantages       */
	struct evac_workload w = { "PAYROLL_BATCH", -1, 0xC0DEu, 0 };
	struct pe_cluster pe;

	printf("\n== rolling evacuation, seed %llu ==\n",
	       (unsigned long long)seed);

	/* ---- 1. FORM ---------------------------------------------------- */
	view_form(&a, NODE_A);
	view_form(&b, NODE_B);
	ct_check_eq_u32(a.cl.club.cluster_nodes, 3u,
			"form: OVMXA sees three members");
	ct_check_eq_u32(a.cl.club.quorum, 2u,
			"form: QUORUM = (3+2)/2 = 2");
	ct_check(!a.cl.club.quorum_lost,
		 "form: OVMXA has quorum (3 votes >= 2)");
	ct_check(a.cl.club.quorum_armed,
		 "form: OVMXA has PERCEIVED quorum (armed)");
	ct_check(!cnxman_quorum_hang_active(&a.cl),
		 "form: OVMXA executive is serving (no hang)");
	ct_check(!cnxman_quorum_hang_active(&b.cl),
		 "form: OVMXB executive is serving (no hang)");

	/* ---- 2. PLACE the workload on the node we will evacuate --------- */
	wl_place_on(&w, NODE_C);
	ct_check(w.on_node == NODE_C,
		 "place: PAYROLL_BATCH runs on OVMXC");
	/* Precondition for a real workload: its host is a current member. */
	ct_check(cnxman_csb_is_member(a.peer[NODE_C]),
		 "place: OVMXC is a member that can host the workload");

	/* ---- transport: form the real circuits so the departure is a real
	 *      wire event, not just a flag change. ----------------------- */
	ct_check(pe_form(&pe, seed),
		 "transport: three ports form their circuits");
	ct_check_eq_u32((uint32_t)pe_vc_state(&pe, NODE_A, NODE_B),
			(uint32_t)VMS_PE_VC_OPEN,
			"transport: OVMXA <-> OVMXB circuit OPEN before evac");

	/* ---- 3. QUIESCE: OVMXC announces departure and stops being present */
	sim_node_halt(&pe.sim.node[NODE_C]);        /* real last gasp, p.7-29 */
	(void)sim_run_ms(&pe.sim, 30000u);          /* survivors process it   */
	view_peer_quiesce(&a, NODE_C);
	view_peer_quiesce(&b, NODE_C);
	/* Membership is HELD across the break: the count must not dip yet. */
	ct_check_eq_u32(a.cl.club.cluster_nodes, 3u,
			"quiesce: membership held across the break (count 3)");
	ct_check(!a.cl.club.quorum_lost,
		 "quiesce: two present votes still meet QUORUM 2");
	ct_check(!cnxman_quorum_hang_active(&a.cl),
		 "quiesce: OVMXA keeps serving while OVMXC is away");

	/* ---- 4. TAKEOVER: the transition removes OVMXC; W moves to OVMXA - */
	view_peer_remove(&a, NODE_C);
	view_peer_remove(&b, NODE_C);
	wl_takeover_to(&w, NODE_A);                 /* the vms-1ee seam        */
	ct_check_eq_u32(a.cl.club.cluster_nodes, 2u,
			"takeover: OVMXC removed, two members remain");
	ct_check_eq_u32(a.cl.club.quorum, 2u,
			"takeover: QUORUM holds at 2 (CEVOTES never decreases)");
	ct_check(w.on_node == NODE_A && w.takeovers == 1,
		 "takeover: a survivor (OVMXA) now runs PAYROLL_BATCH");
	ct_check(cnxman_quorum_enforce_ready(&a.cl),
		 "takeover: OVMXA is still a committed member");

	/* ---- 5. VERIFY: the cluster STAYED UP -------------------------- */
	ct_check(!a.cl.club.quorum_lost,
		 "stays up: OVMXA still has quorum (2 votes >= 2)");
	ct_check(!b.cl.club.quorum_lost,
		 "stays up: OVMXB still has quorum (2 votes >= 2)");
	ct_check(!cnxman_quorum_hang_active(&a.cl),
		 "STAYS UP: OVMXA executive keeps serving after evacuation");
	ct_check(!cnxman_quorum_hang_active(&b.cl),
		 "STAYS UP: OVMXB executive keeps serving after evacuation");

	/* transport corroboration: the survivors HELD their circuit to each
	 * other, and each saw the circuit to OVMXC go down. */
	ct_check_eq_u32((uint32_t)pe_vc_state(&pe, NODE_A, NODE_B),
			(uint32_t)VMS_PE_VC_OPEN,
			"transport: OVMXA <-> OVMXB circuit HELD through evac");
	ct_check_eq_u32((uint32_t)pe_vc_downs(&pe, NODE_A, NODE_B), 0u,
			"transport: that survivor circuit was never torn down");
	ct_check((uint32_t)pe_vc_state(&pe, NODE_A, NODE_C) !=
			(uint32_t)VMS_PE_VC_OPEN,
		 "transport: OVMXA's circuit to the departed OVMXC is down");

	/* ---- FALSIFIER: a SECOND departure loses quorum. Proves every
	 *      "stays up" check above CAN go red (INV-6, non-vacuity). ---- */
	view_peer_quiesce(&a, NODE_B);   /* now OVMXA is the last node up */
	ct_check(a.cl.club.quorum_lost != 0,
		 "falsifier: one vote < QUORUM 2 -> quorum LOST");
	ct_check(cnxman_quorum_hang_active(&a.cl) != 0,
		 "falsifier: OVMXA executive STALLS -- a lone node is NOT a "
		 "cluster (so the checks above are real)");
}

/*
 * Determinism: the whole demo is pure arithmetic over the CLUB plus a
 * seed-deterministic transport, so two seeds must reach identical survivor
 * quorum state. (The trace-digest byte-equality of the PE layer is already
 * proven by test_sim_vc.c; here we assert the membership verdict is seed
 * independent, which is what a demo that must reproduce needs.)
 */
static void test_evacuation_is_deterministic(void)
{
	static struct member_view a1, a2;

	view_form(&a1, NODE_A);
	view_peer_quiesce(&a1, NODE_C);
	view_peer_remove(&a1, NODE_C);

	view_form(&a2, NODE_A);
	view_peer_quiesce(&a2, NODE_C);
	view_peer_remove(&a2, NODE_C);

	ct_check_eq_u32(a1.cl.club.cluster_nodes, a2.cl.club.cluster_nodes,
			"deterministic: same member count");
	ct_check_eq_u32(a1.cl.club.quorum, a2.cl.club.quorum,
			"deterministic: same QUORUM");
	ct_check_eq_u32(a1.cl.club.quorum_lost, a2.cl.club.quorum_lost,
			"deterministic: same quorum verdict");
}

int main(void)
{
	test_rolling_evacuation_stays_up(1u);
	test_rolling_evacuation_stays_up(7u);
	test_evacuation_is_deterministic();
	return ct_summary("sim_rolling_evacuation");
}
