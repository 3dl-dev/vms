/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_genesis.c - cluster GENESIS, the POSITIVE half (test-ladder rung
 * R1; docs/design-cluster-genesis.md).
 *
 * WHAT THIS PROVES.
 *
 *   1. THE PREDICATE, as a truth table. `cnxman_quorum_own_votes_suffice()` is
 *      the one gate that permits an executive to mint a CSID nobody gave it,
 *      so it is exercised over (VOTES, EXPECTED_VOTES) pairs directly rather
 *      than inferred from an outcome -- including the pairs whose answer is NO
 *      and the Old-CEVOTES case, where a node that was once in a big cluster
 *      may NOT re-found it on one vote (p. 7-6: CEVOTES cannot decrease by
 *      itself).
 *
 *   2. THE FOUNDING ITSELF, end to end and through the REAL machinery. A
 *      node that satisfies quorum on its own votes reaches VMS_CLUSTER_MEMBER
 *      -- and every step of the way is the same code an admission from a real
 *      VAX runs: cnxman_club_learn_local_csid() (the ONE setter of
 *      local_csid_valid, shared with the op-0x06 learn path),
 *      coord_open_transition/coord_enter_open (the same Phase 1 the joiner's
 *      op-0x02 opens), and cnxman_phase2_commit() (the same commit, setting
 *      MEMBER out of this node's own CSB). There is no founding-only
 *      membership path, and this test is what says so.
 *
 *   3. NOT ONE FRAME GOES ON THE WIRE. With no participants there is nobody to
 *      propose to, so the founding transition is the SAME degenerate
 *      12 x (M-1) = 0 case a two-node cluster takes when it loses its peer.
 *      `ops.send`/`ops.respond` are counted and must stay at zero: a founder
 *      that emitted anything would be talking to a cluster that does not exist.
 *
 *   4. THE MINT IS THE PUBLISHED CONSTRUCTION. generation 1 over this node's
 *      own SCSSYSTEMID, assembled by vms_cm_csid_of() -- the SAME function the
 *      joiner uses on a wire-learned generation, so a founder and a joiner
 *      cannot build a CSID differently.
 *
 * The refusals are the sibling suite (test_cnxman_genesis_negctl.c), which is
 * where the INV-6 teeth live.
 *
 * GROUNDING. *VAXcluster Principles* (Davis 1993) pp. 7-6 (the quorum
 * algorithm), 7-25 (the CSID and its sequence), 7-41/7-42 (Phase 1, the GO and
 * the Phase 2 tasks) -- host-only, page cites only (Rule 8).
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
#include "vms_cnxman_quorum.h"
#include "vms_cnxman_phase2.h"
#include "vms_cnxman_coord_fsm.h"
#include "vms_cluster_codec_cm.h"

/*
 * The lab's own founder: SCSSYSTEMID 1025, whose bottom ten bits are 1 -- the
 * same relationship the real-VAX capture shows (VAX1: sysid 1025, CSID
 * 0x00010001; VAX3: sysid 1027, CSID 0x00010003, codec header sec on the
 * op-0x06 forms). Not a number chosen to make the arithmetic work: it is what
 * a real cluster's first slot looks like.
 */
#define FOUNDER_SYSID 1025u
#define FOUNDER_CSID  0x00010001u

struct bed {
	struct vms_cluster   cl;
	struct cnxman_ops    ops;
	struct fake_cnx      fake;
	struct cnxman_coord  c;

	uint32_t sends;
	uint32_t responds;

	uint32_t dlm_begins;
	uint32_t dlm_ends;
	int      dlm_last_completed;
};

static struct bed g;

static int bed_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx; (void)dst; (void)body; (void)len;
	g.sends++;
	return 0;
}

static int bed_respond(void *ctx, const uint8_t *body, uint32_t len)
{
	(void)ctx; (void)body; (void)len;
	g.responds++;
	return 0;
}

static void bed_dlm_begin(void *ctx, const struct cnxman_transition *tr)
{
	(void)ctx; (void)tr;
	g.dlm_begins++;
}

static void bed_dlm_end(void *ctx, const struct cnxman_transition *tr,
			int completed)
{
	(void)ctx; (void)tr;
	g.dlm_ends++;
	g.dlm_last_completed = completed;
}

static const struct dlm_scs_role_ops bed_dlm_ops = {
	bed_dlm_begin, NULL, bed_dlm_end, NULL, &g
};

/* A node exactly as it stands at CLUSTER_START: SYSGEN parameters loaded, the
 * CLUB initialised with its own local CSB, NO CSID, no peers, not a member. */
static void bed_init(uint16_t votes, uint16_t expected_votes)
{
	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.ops.send = bed_send;
	g.ops.respond = bed_respond;
	g.fake.now_ms = 100000u;

	memcpy(g.cl.params.scsnode, "OVMX01", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = (vms_scs_sysid_t)FOUNDER_SYSID;
	g.cl.params.vaxcluster = 2u;
	g.cl.params.votes = votes;
	g.cl.params.expected_votes = expected_votes;

	(void)cnxman_club_init(&g.cl);
	g.cl.state = VMS_CLUSTER_JOINING;   /* "waiting to form or join" */

	cnxman_coord_init(&g.c, &g.cl, &g.ops);
	cnxman_coord_set_dlm(&g.c, &bed_dlm_ops);
}

/* ==========================================================================
 * 1. The predicate, as a truth table
 * ========================================================================== */

static void check_predicate(uint16_t votes, uint16_t expected_votes,
			    uint16_t old_cevotes, int want_found,
			    uint16_t want_quorum, const char *what)
{
	uint16_t quorum = 0xffffu;
	int got;

	bed_init(votes, expected_votes);
	g.cl.club.cevotes = old_cevotes;

	got = cnxman_quorum_own_votes_suffice(&g.cl, &quorum);
	ct_check(got == want_found, what);
	ct_check_eq_u32(quorum, want_quorum, "  ... judged against quorum");
}

static void test_predicate_truth_table(void)
{
	printf("[genesis] the founding predicate, (VOTES, EXPECTED_VOTES)\n");

	/* VOTES=0: a non-voting node NEVER founds, whatever else is set. It is
	 * the default (vms_cluster.h: "0 first, design D-10"), which is why a
	 * tree that shipped genesis with this case wrong would form phantom
	 * clusters everywhere. The quorum readback stays 0: the predicate
	 * refused before there was anything to judge it against. */
	check_predicate(0u, 0u, 0u, 0, 0u, "VOTES=0, EXPECTED_VOTES=0: NO");
	check_predicate(0u, 1u, 0u, 0, 0u, "VOTES=0, EXPECTED_VOTES=1: NO");
	check_predicate(0u, 4u, 0u, 0, 0u, "VOTES=0, EXPECTED_VOTES=4: NO");

	/* The founding configuration: one vote, one expected. CEVOTES = 1,
	 * QUORUM = (1+2)/2 = 1, and this node has it. */
	check_predicate(1u, 1u, 0u, 1, 1u, "VOTES=1, EXPECTED_VOTES=1: FOUNDS");

	/* EXPECTED_VOTES unset: CEVOTES = max{0, 1} = 1. A node told it has a
	 * vote and told nothing else is a one-node cluster. */
	check_predicate(1u, 0u, 0u, 1, 1u, "VOTES=1, EXPECTED_VOTES=0: FOUNDS");

	/* THE PROTECTION. Two nodes each with one vote and EXPECTED_VOTES=2:
	 * CEVOTES = 2, QUORUM = 2, and neither has 2 -- so NEITHER founds and
	 * they must find each other. This is the configuration that keeps a
	 * pair from forming two clusters of one. */
	check_predicate(1u, 2u, 0u, 0, 2u, "VOTES=1, EXPECTED_VOTES=2: NO");
	check_predicate(2u, 2u, 0u, 1, 2u, "VOTES=2, EXPECTED_VOTES=2: FOUNDS");

	check_predicate(1u, 3u, 0u, 0, 2u, "VOTES=1, EXPECTED_VOTES=3: NO");
	check_predicate(2u, 3u, 0u, 1, 2u, "VOTES=2, EXPECTED_VOTES=3: FOUNDS");
	check_predicate(3u, 3u, 0u, 1, 2u, "VOTES=3, EXPECTED_VOTES=3: FOUNDS");
	check_predicate(2u, 4u, 0u, 0, 3u, "VOTES=2, EXPECTED_VOTES=4: NO");
	check_predicate(3u, 4u, 0u, 1, 3u, "VOTES=3, EXPECTED_VOTES=4: FOUNDS");

	/* OLD CEVOTES IS IN THE max{} (p. 7-6, and pp. 7-10/7-11: it "cannot
	 * decrease by itself"). A node whose CLUB already carries CEVOTES=5 --
	 * a cluster it was really in -- may not re-found on one vote just
	 * because its own EXPECTED_VOTES is small. */
	check_predicate(1u, 1u, 5u, 0, 3u, "old CEVOTES=5 outvotes VOTES=1: NO");
	check_predicate(3u, 1u, 5u, 1, 3u, "old CEVOTES=5, VOTES=3: FOUNDS");
}

/* ==========================================================================
 * 2. The mint: the published construction, shared with the joiner
 * ========================================================================== */

static void test_csid_construction(void)
{
	printf("[genesis] the CSID construction (generation << 16 | SYSID & 0x3ff)\n");

	ct_check_eq_u32(vms_cm_csid_of(1u, FOUNDER_SYSID), FOUNDER_CSID,
			"generation 1 over SCSSYSTEMID 1025 is CSID 0x00010001");
	/* The same function the joiner drives with a WIRE-LEARNED generation:
	 * a founder at generation 1 and a joiner told generation 7 build the
	 * same shape from the same real SCSSYSTEMID. */
	ct_check_eq_u32(vms_cm_csid_of(7u, 1027u), 0x00070003u,
			"generation 7 over SCSSYSTEMID 1027 is CSID 0x00070003");
	ct_check_eq_u32(CNXMAN_COORD_GENESIS_GEN, 1u,
			"a cluster formed from nothing is generation 1 (p. 7-25)");
}

/* ==========================================================================
 * 3. The founding, end to end
 * ========================================================================== */

static void test_founds_and_becomes_member(void)
{
	struct vms_csb *local;
	int rc;

	printf("[genesis] a node with quorum by its own votes forms a cluster\n");
	bed_init(1u, 1u);

	/* Before: no identity, no membership. */
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"before: this node holds NO cluster system id");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER, "before: not a member");

	rc = cnxman_coord_found(&g.c);
	ct_check_eq_u32((unsigned long)rc, 0u, "cnxman_coord_found() founds");

	/* The identity, through the ONE setter a joiner also goes through. */
	ct_check_eq_u32(g.cl.club.local_csid_valid, 1u,
			"local_csid_valid set by cnxman_club_learn_local_csid()");
	ct_check_eq_u32(g.cl.club.local_csid, FOUNDER_CSID,
			"the minted CSID is generation 1 over the real SCSSYSTEMID");

	local = cnxman_club_local(&g.cl.club);
	ct_check(local != NULL, "the local CSB is there");
	ct_check_eq_u32(local->csid_valid, 1u, "the local CSB carries the CSID");
	ct_check_eq_u32(local->csid, FOUNDER_CSID, "... the same one");

	/* THE MEMBERSHIP, and where it came from: phase2 task 1 set MEMBER
	 * from the nodemap bit for this node's own CSID, task 3 counted the
	 * SELECTED CSBs, task 4 set the node state. Nothing assigned it. */
	ct_check(cnxman_csb_is_member(local),
		 "phase2 set MEMBER on this node's own CSB");
	ct_check_eq_u32((local->flags & VMS_CSB_F_SELECTED) != 0u, 1u,
			"... and SELECTED");
	ct_check_eq_u32((unsigned long)g.cl.state, (unsigned long)VMS_CLUSTER_MEMBER,
			"cl->state is VMS_CLUSTER_MEMBER (phase2 task 4)");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 1u,
			"the committed member count is 1 (p. 7-49: SELECTED CSBs)");

	/* A REAL transition ran: epoch advanced, the DLM saw a begin and a
	 * COMPLETED end, the barrier ran its twelve steps against an empty
	 * census, and the coordinator released its lock. */
	ct_check_eq_u32(g.cl.club.epoch, 1u, "the cluster is at generation-1 epoch 1");
	ct_check_eq_u32(g.dlm_begins, 1u, "the DLM was told the transition began");
	ct_check_eq_u32(g.dlm_ends, 1u, "... and that it ended");
	ct_check_eq_u32((unsigned long)g.dlm_last_completed, 1u,
			"... COMPLETED, not abandoned");
	ct_check_eq_u32(g.c.state, (unsigned long)CNXMAN_COORD_COMPLETE,
			"the coordinator finished the transition");
	ct_check_eq_u32(g.cl.club.barrier_step, 12u,
			"all twelve barrier steps ran (12 x (M-1) = 0 frames)");
	ct_check_eq_u32(g.cl.club.reformations, 1u, "one reformation recorded");
	ct_check_eq_u32(g.cl.club.transition_active, 0u,
			"the coordinator lock was released (p. 7-42)");
	ct_check_eq_u32(g.c.genesis_opens, 1u, "one founding transition counted");

	/* AND NOTHING WENT ON THE WIRE. */
	ct_check_eq_u32(g.sends, 0u, "no frame was sent: there is nobody to send to");
	ct_check_eq_u32(g.responds, 0u, "... and nothing was responded to");
	ct_check_eq_u32(g.c.opens_sent + g.c.gos_sent + g.c.releases_sent, 0u,
			"no open, no GO, no release originated");

	/* The founded cluster's quorum, recomputed from the CSBs phase2 just
	 * committed -- the glue does this immediately after founding. */
	cnxman_quorum_recompute(&g.cl.club);
	ct_check_eq_u32(g.cl.club.cevotes, 1u, "CEVOTES from the real local CSB");
	ct_check_eq_u32(g.cl.club.quorum, 1u, "QUORUM = (1+2)/2");
	ct_check_eq_u32(g.cl.club.quorum_lost, 0u,
			"the founder has quorum: 1 present vote >= 1");
}

/* A second call changes nothing: this node already holds a CSID, so it is no
 * longer a candidate to found anything. Idempotence matters because the
 * founding attempt runs on the reconnect beat, once a second, forever. */
static void test_second_call_refuses(void)
{
	vms_csid_t csid_before;
	uint32_t epoch_before;
	int rc;

	printf("[genesis] founding is not repeatable\n");
	bed_init(1u, 1u);
	ct_check_eq_u32((unsigned long)cnxman_coord_found(&g.c), 0u,
			"the first call founds");
	csid_before = g.cl.club.local_csid;
	epoch_before = g.cl.club.epoch;

	rc = cnxman_coord_found(&g.c);
	ct_check(rc != 0, "the second call REFUSES");
	ct_check_eq_u32(g.c.last_refusal, (unsigned long)CNXMAN_COORD_REF_BUSY,
			"... because this node already holds a cluster system id");
	ct_check_eq_u32(g.cl.club.local_csid, csid_before,
			"the CSID was not re-minted");
	ct_check_eq_u32(g.cl.club.epoch, epoch_before,
			"no second transition was opened");
	ct_check_eq_u32(g.c.genesis_opens, 1u, "still exactly one founding");
}

int main(void)
{
	test_predicate_truth_table();
	test_csid_construction();
	test_founds_and_becomes_member();
	test_second_call_refuses();
	return ct_summary("test_cnxman_genesis");
}
