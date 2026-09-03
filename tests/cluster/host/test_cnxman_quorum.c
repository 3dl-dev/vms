/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_quorum.c - the Quorum Algorithm: CEVOTES/QUORUM computation
 * against the CLUB's own CSB table (FC-P3.7, test-ladder rung R1).
 *
 * WHAT THIS PORTS. `tests/vmsscs/test_scs_quorum.c` proved the same published
 * algorithm against the strawman's standalone `scs_quorum` model (a
 * caller-fed member list, no CSB, no wire). Every ARITHMETIC case there is
 * reproduced here against the real executive-resident model instead --
 * `cnxman_quorum_recompute()` walking `struct vms_club`'s own SELECTED CSBs
 * (vms_cnxman_quorum.c) -- so the same worked examples now prove the object
 * this item actually ships. The strawman's `test_votes_wire_roundtrip` is NOT
 * ported: it round-trips the RETIRED `src/vmsscs/scs_member.c` codec, which
 * this branch does not build against (Rule 9 -- reference its wire shape
 * only, never lift its code); the wire-grounded VOTES byte itself is FC-P3.1's
 * codec, already covered by test_codec_cm.c.
 *
 * ORACLE (documented, clean-room rule 8): *VAXcluster Principles* (Davis
 * 1993) ch. 7 pp. 7-5/7-6 (the max{} formula, "cannot decrease by itself"
 * pp. 7-10/7-11), the 5-node worked example pp. 7-6/7-7, the quorum-disk
 * trustworthiness/Watcher concept pp. 7-15/7-16 (why this module tracks but
 * does not fold in QDSKVOTES). Cross-checked against the public *VMScluster
 * Systems for OpenVMS* sec. 2.3.5/2.3.6/2.3.8 -- the identical algorithm and
 * worked examples, cited the same way in vms_cnxman_quorum.h.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"

#include "vms_cluster.h"
#include "vms_cluster_snapshot.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_quorum.h"

static struct vms_cluster g_cl;

/* A fresh CLUB: the local system's own SYSGEN VOTES/EXPECTED_VOTES seed the
 * local CSB (cnxman_club_init(), FC-P3.6), exactly as STARTUP.EXE's
 * VMS_IOCTL_SYSGEN_LOAD would before VMS_IOCTL_CLUSTER_START. */
static struct vms_club *reset_cluster(uint16_t local_votes,
				      uint16_t local_expected)
{
	memset(&g_cl, 0, sizeof(g_cl));
	g_cl.params.scssystemid = 0x0000040001FFull;
	memcpy(g_cl.params.scsnode, "LOCAL", 5);
	g_cl.params.scsnode_len = 5;
	g_cl.params.votes = local_votes;
	g_cl.params.expected_votes = local_expected;
	cnxman_club_init(&g_cl);
	return &g_cl.club;
}

/*
 * Add one member CSB: SELECTED (p. 7-6's "selected set of proposed members",
 * p. 7-49's membership count) with a learned PARAMS record. `reachable`
 * decides whether it also counts toward PRESENT votes (state OPEN) or is
 * held as a member but not currently contributing (any other state --
 * p. 7-30's reconnect hold).
 */
static struct vms_csb *add_member(struct vms_club *club, uint64_t sysid,
				  uint16_t votes, uint16_t expected,
				  int reachable)
{
	struct vms_csb *csb = cnxman_club_alloc_csb(club, sysid, 1);

	if (csb == NULL)
		return NULL;
	cnxman_csb_set_params(csb, votes, expected, 0);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
	csb->state = reachable ? (uint8_t)VMS_CNXMAN_CSB_OPEN
			       : (uint8_t)VMS_CNXMAN_CSB_WAIT;
	return csb;
}

/* Select (and, if asked, mark reachable) the LOCAL CSB itself -- p. 7-6's
 * proposed set always includes the system doing the computing once it has
 * actually decided it belongs to the set. Nothing in FC-P3.6 does this for
 * free (INV-6: no unearned self-membership), so the fixture does it exactly
 * as the not-yet-built join FSM (FC-P3.3) eventually will. */
static void select_local(struct vms_club *club)
{
	struct vms_csb *local = cnxman_club_local(club);

	if (local != NULL)
		cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
}

/*
 * ==========================================================================
 * 1. The five-node worked example, Davis pp. 7-6/7-7 -- CEVOTES/QUORUM as
 *    members depart, present votes falling until quorum is lost.
 *
 *   transition            up  SUM  maxEXP  Old   New CEVOTES  QUORUM  present
 *   ------------------------------------------------------------------------
 *   1. all five formed    5    5     5      0       5           3       5
 *   2. one departs         4    5     5      5       5           3       4
 *   3. two departed        3    5     5      5       5           3       3
 *   4. three departed      2    5     5      5       5           3       2
 * ==========================================================================
 */
static void test_five_node_worked_example(void)
{
	struct vms_club *club = reset_cluster(0, 0);   /* local: a bystander, VOTES=0 */
	struct vms_csb *n[5];
	int i;

	printf("[quorum] the five-node worked example, pp. 7-6/7-7\n");

	for (i = 0; i < 5; i++) {
		n[i] = add_member(club, 0x1000ull + (uint64_t)i, 1, 5, 1);
		ct_check(n[i] != NULL, "5-node: member allocated");
	}

	/* Transition 1: all five formed. */
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 5, "5-node t1: CEVOTES = max{5,5,0} = 5");
	ct_check_eq_u32(club->quorum, 3, "5-node t1: QUORUM = (5+2)/2 = 3");
	ct_check(!club->quorum_lost, "5-node t1: quorum held (5 >= 3)");

	/* Transition 2: node 4 departs (held as WAIT, membership stays). */
	n[4]->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 5, "5-node t2: CEVOTES holds at 5 (never decreases)");
	ct_check_eq_u32(club->quorum, 3, "5-node t2: QUORUM holds at 3");
	ct_check(!club->quorum_lost, "5-node t2: quorum held (4 >= 3)");

	/* Transition 3: node 3 also down. present == quorum -> still held. */
	n[3]->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->quorum, 3, "5-node t3: QUORUM = 3");
	ct_check(!club->quorum_lost, "5-node t3: held at the boundary (present == quorum)");

	/* Transition 4: node 2 also down. present < quorum -> LOST. */
	n[2]->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 5, "5-node t4: CEVOTES still 5 (never decreased)");
	ct_check_eq_u32(club->quorum, 3, "5-node t4: QUORUM still 3");
	ct_check(club->quorum_lost != 0, "5-node t4: quorum LOST (2 < 3)");
}

/*
 * ==========================================================================
 * 2. The three-node documented example (VMScluster Systems sec. 2.3.6): any
 *    two of three VOTES=1/EXPECTED_VOTES=3 nodes constitute a quorum, no
 *    single node can.
 * ==========================================================================
 */
static void test_three_node_documented_example(void)
{
	struct vms_club *club = reset_cluster(0, 0);
	struct vms_csb *a = add_member(club, 1, 1, 3, 1);
	struct vms_csb *b = add_member(club, 2, 1, 3, 1);
	struct vms_csb *c = add_member(club, 3, 1, 3, 1);

	printf("[quorum] the three-node documented example, sec. 2.3.6\n");
	ct_check(a != NULL && b != NULL && c != NULL, "3-node: three members allocated");

	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->quorum, 2, "3-node: QUORUM = (3+2)/2 = 2");
	ct_check(!club->quorum_lost, "3-node: three up -> quorum held");

	c->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	ct_check(!club->quorum_lost, "3-node: any two constitute a quorum");

	b->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	ct_check(club->quorum_lost != 0, "3-node: no single node constitutes a quorum");
}

/*
 * ==========================================================================
 * 3. The EXPECTED_VOTES term must WIN the max even when it exceeds the votes
 *    actually present -- the "estimated quorum" step-1 case.
 * ==========================================================================
 */
static void test_expected_votes_dominates(void)
{
	struct vms_club *club = reset_cluster(0, 0);

	printf("[quorum] EXPECTED_VOTES dominates the max\n");
	add_member(club, 1, 1, 5, 1);
	add_member(club, 2, 1, 5, 1);
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 5, "expected term: CEVOTES = max{5, 2, 0} = 5");
	ct_check_eq_u32(club->quorum, 3, "expected term: QUORUM = 3");
	ct_check(club->quorum_lost != 0, "expected term: 2 < 3 -> lost until more join");
}

/*
 * ==========================================================================
 * 4. The SUM-VOTES term must win once the running cluster's SUM exceeds each
 *    member's own configured EXPECTED_VOTES.
 * ==========================================================================
 */
static void test_sum_votes_dominates(void)
{
	struct vms_club *club = reset_cluster(0, 0);

	printf("[quorum] SUM VOTES dominates the max\n");
	add_member(club, 1, 1, 1, 1);
	add_member(club, 2, 1, 1, 1);
	add_member(club, 3, 1, 1, 1);
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 3, "sum term: CEVOTES = max{1, 3, 0} = 3");
	ct_check_eq_u32(club->quorum, 2, "sum term: QUORUM = (3+2)/2 = 2");
	ct_check(!club->quorum_lost, "sum term: 3 >= 2 -> quorum held");
}

/*
 * ==========================================================================
 * 5. CEVOTES/QUORUM never decrease on their own, even across repeated
 *    recomputes after votes fall (pp. 7-10/7-11).
 * ==========================================================================
 */
static void test_cevotes_never_decreases(void)
{
	struct vms_club *club = reset_cluster(0, 0);
	struct vms_csb *a = add_member(club, 1, 2, 2, 1);
	struct vms_csb *b = add_member(club, 2, 2, 2, 1);

	printf("[quorum] CEVOTES/QUORUM never decrease on their own\n");
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 4, "monotonic: CEVOTES 4");
	ct_check_eq_u32(club->quorum, 3, "monotonic: QUORUM 3");

	/* Both members' votes fall to zero (a re-advertised PARAMS record, not
	 * a departure) and recompute runs twice: CEVOTES must hold. */
	cnxman_csb_set_params(a, 0, 0, 0);
	a->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_csb_set_params(b, 0, 0, 0);
	b->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 4, "monotonic: CEVOTES holds at 4 after votes fall");
	ct_check_eq_u32(club->quorum, 3, "monotonic: QUORUM holds at 3");
	ct_check(club->quorum_lost != 0, "monotonic: 0 < 3 -> quorum lost");
}

/*
 * ==========================================================================
 * 6. QDSKVOTES is TRACKED but NOT folded into CEVOTES/QUORUM/PRESENT in this
 *    slice (P8 owns the Watcher/trustworthiness liveness, pp. 7-15/7-16) --
 *    the honest divergence from the strawman's test_quorum_disk, which
 *    modelled disk presence as a caller-supplied boolean this module does not
 *    yet have from the executive.
 * ==========================================================================
 */
static void test_quorum_disk_tracked_not_folded_in(void)
{
	struct vms_club *club;

	printf("[quorum] QDSKVOTES tracked, not folded in (P8 scope)\n");
	/* The local system's own SYSGEN QDSKVOTES, learned at club_init(). */
	memset(&g_cl, 0, sizeof(g_cl));
	g_cl.params.scssystemid = 0x0000040001FFull;
	memcpy(g_cl.params.scsnode, "LOCAL", 5);
	g_cl.params.scsnode_len = 5;
	g_cl.params.qdskvotes = 1;
	cnxman_club_init(&g_cl);
	club = &g_cl.club;

	add_member(club, 1, 1, 3, 1);
	add_member(club, 2, 1, 3, 1);
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 3, "qdisk: CEVOTES = max{3, 2, 0} = 3 (disk not summed)");
	ct_check_eq_u32(club->quorum, 2, "qdisk: QUORUM = 2 (disk not summed)");
	ct_check_eq_u32(cnxman_quorum_qdskvotes(club), 1,
			"qdisk: the configured QDSKVOTES is still readable");
}

/*
 * ==========================================================================
 * 7. OVMX's own non-voting contribution (design D-10, VOTES=0 first): two
 *    voting VAXes plus a non-voting OVMX must behave exactly as if OVMX were
 *    not there -- its zero votes never prop up quorum.
 * ==========================================================================
 */
static void test_ovmx_nonvoting_contribution(void)
{
	struct vms_club *club = reset_cluster(0, 1);   /* OVMX: VOTES=0 (design D-10) */
	struct vms_csb *vax2;

	printf("[quorum] OVMX's non-voting contribution never props up quorum\n");
	select_local(club);   /* OVMX joins the selected set with VOTES=0 */
	add_member(club, 0x1025, 1, 1, 1);   /* VAX1, voting */
	vax2 = add_member(club, 0x1026, 1, 1, 1);   /* VAX2, voting */

	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 2, "ovmx: CEVOTES 2 (VAX1 + VAX2; OVMX contributes 0)");
	ct_check_eq_u32(club->quorum, 2, "ovmx: QUORUM 2");
	ct_check(!club->quorum_lost, "ovmx: two voting VAXes present -> quorum held");

	/* Kill VAX2: VAX1 (1) + OVMX (0) = 1 < quorum 2 -> lost. OVMX being up
	 * with zero votes must not keep the cluster running. */
	vax2->state = (uint8_t)VMS_CNXMAN_CSB_WAIT;
	cnxman_quorum_recompute(club);
	ct_check(club->quorum_lost != 0,
		 "ovmx: quorum lost; OVMX's 0 votes do not prop it up");
}

/*
 * ==========================================================================
 * 8. Edge / INV-6 handling: an empty CLUB, and a CSB that never learned its
 *    PARAMS contributes nothing (not an advertised 0).
 * ==========================================================================
 */
static void test_edge(void)
{
	struct vms_club *club = reset_cluster(0, 0);
	struct vms_csb *unlearned;

	printf("[quorum] edge cases: empty CLUB, an un-advertised CSB\n");

	cnxman_quorum_recompute(club);   /* nothing SELECTED yet: must not crash */
	ct_check_eq_u32(club->cevotes, 0, "empty: CEVOTES 0 (nothing selected)");
	ct_check_eq_u32(club->quorum, 1, "empty: QUORUM (0+2)/2 = 1");
	ct_check(club->quorum_lost != 0, "empty: 0 < 1 -> quorum lost");

	cnxman_quorum_recompute(NULL);   /* must not crash */

	/* A CSB that is SELECTED but never received a PARAMS record
	 * (params_valid == 0) must contribute nothing -- INV-6: unknown, not
	 * zero. */
	unlearned = cnxman_club_alloc_csb(club, 999, 1);
	cnxman_csb_set_flags(unlearned, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
	unlearned->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	add_member(club, 1, 3, 3, 1);
	cnxman_quorum_recompute(club);
	ct_check_eq_u32(club->cevotes, 3,
			"edge: the un-advertised CSB is skipped, not counted as 0");
}

int main(void)
{
	test_five_node_worked_example();
	test_three_node_documented_example();
	test_expected_votes_dominates();
	test_sum_votes_dominates();
	test_cevotes_never_decreases();
	test_quorum_disk_tracked_not_folded_in();
	test_ovmx_nonvoting_contribution();
	test_edge();

	return ct_summary("test_cnxman_quorum");
}
