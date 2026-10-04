/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_genesis.c - cluster GENESIS, the POSITIVE half (test-ladder rung
 * R1; docs/design-cluster-genesis.md).
 *
 * WHAT THIS PROVES.
 *
 *   1. THE PREDICATE, as a truth table. `cnxman_quorum_form_votes_suffice()` is
 *      the one gate that permits an executive to mint a CSID nobody gave it,
 *      so it is exercised over (VOTES, EXPECTED_VOTES) pairs directly rather
 *      than inferred from an outcome -- including the pairs whose answer is NO
 *      and the Old-CEVOTES case, where a node that was once in a big cluster
 *      may NOT re-found it on one vote (p. 7-6: CEVOTES cannot decrease by
 *      itself). The set it is judged over is the COLD FORMATION's proposed set
 *      (p. 7-6 step 1), so the table is run twice: once for a node that can see
 *      nobody, and once for the documented two-node VMScluster where the answer
 *      flips because the peer's vote is really there (rd vms-6d3d).
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

/* Designated initialisers: the role ops grow (quorum_changed, FC-P8.1), and a
 * positional list silently shifts every field when they do. */
static const struct dlm_scs_role_ops bed_dlm_ops = {
	.transition_begin = bed_dlm_begin,
	.transition_end   = bed_dlm_end,
	.ctx              = &g,
};

/* A node exactly as it stands at CLUSTER_START: SYSGEN parameters loaded, the
 * CLUB initialised with its own local CSB, NO CSID, no peers, not a member. */
static void bed_init_sysid(uint16_t votes, uint16_t expected_votes,
			   vms_scs_sysid_t sysid)
{
	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.ops.send = bed_send;
	g.ops.respond = bed_respond;
	g.fake.now_ms = 100000u;

	memcpy(g.cl.params.scsnode, "OVMX01", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = sysid;
	g.cl.params.vaxcluster = 2u;
	g.cl.params.votes = votes;
	g.cl.params.expected_votes = expected_votes;

	(void)cnxman_club_init(&g.cl);
	g.cl.state = VMS_CLUSTER_JOINING;   /* "waiting to form or join" */

	cnxman_coord_init(&g.c, &g.cl, &g.ops);
	cnxman_coord_set_dlm(&g.c, &bed_dlm_ops);
}

static void bed_init(uint16_t votes, uint16_t expected_votes)
{
	bed_init_sysid(votes, expected_votes, (vms_scs_sysid_t)FOUNDER_SYSID);
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

	got = cnxman_quorum_form_votes_suffice(&g.cl, (const struct cnxman_form_set *)0, &quorum);
	ct_check(got == want_found, what);
	ct_check_eq_u32(quorum, want_quorum, "  ... judged against quorum");
}

/*
 * The SAME table, but with one peer this node can really see: a CSB whose
 * PARAMS record really arrived and whose circuit is really OPEN. That is
 * p. 7-6 step 1's proposed set with two systems in it, and the votes weighed
 * against quorum are their COMBINED votes (rd vms-6d3d).
 */
static void check_predicate_with_peer(uint16_t votes, uint16_t expected_votes,
				      uint16_t peer_votes,
				      uint16_t peer_expected_votes,
				      int want_found, uint16_t want_quorum,
				      const char *what)
{
	struct vms_csb *peer;
	uint16_t quorum = 0xffffu;
	int got;

	bed_init(votes, expected_votes);
	peer = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)1990u, 1);
	ct_check(peer != NULL, "the peer CSB exists");
	cnxman_csb_set_params(peer, peer_votes, peer_expected_votes, 0u);
	peer->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;

	got = cnxman_quorum_form_votes_suffice(&g.cl,
					       (const struct cnxman_form_set *)0,
					       &quorum);
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

	/* ------------------------------------------------------------------
	 * AND THE SAME TABLE WITH A SYSTEM THIS NODE CAN SEE (rd vms-6d3d).
	 *
	 * THE DEFECT THIS ROW IS: with VOTES=1 / EXPECTED_VOTES=2 on both --
	 * the documented two-node VMScluster -- NEITHER node's own vote meets
	 * quorum 2, so the predicate above says NO to both and an all-OVMX
	 * pair configured the documented way could never form a cluster at
	 * all. The ORACLE (two real OpenVMS VAX V7.3 systems, capture
	 * tests/lab/captures/vms-6d3d-coldform-ev2-20260924/) forms exactly
	 * this cluster: the first node waited alone for eighteen minutes with
	 * no %CNXMAN line at all, and proposed the formation 3.2 s after the
	 * second one's circuit came up.
	 * ------------------------------------------------------------------ */
	printf("[genesis] ... and over the systems this node can SEE\n");

	/* The textbook pair: 1 + 1 = 2 votes against quorum (2+2)/2 = 2. */
	check_predicate_with_peer(1u, 2u, 1u, 2u, 1, 2u,
		"VOTES=1/EV=2 + a seen VOTES=1/EV=2 peer: FOUNDS on 2 votes");

	/* ... and the SAME node with nobody in sight still refuses, which is
	 * what makes the row above a combined-votes answer and not a weakened
	 * gate. (check_predicate() above already asserts it; repeated here
	 * because the pair is the whole point.) */
	check_predicate(1u, 2u, 0u, 0, 2u,
		"VOTES=1/EV=2 alone: STILL NO -- one vote is not quorum 2");

	/* A peer that advertises ZERO votes adds nothing to the sum: seeing a
	 * non-voting system does not manufacture quorum (INV-6). */
	check_predicate_with_peer(1u, 2u, 0u, 2u, 0, 2u,
		"a seen VOTES=0 peer does not make quorum");

	/* p. 7-6 step 2's EXPECTED_VOTES term is the LARGEST in the set, so a
	 * peer that expects a bigger cluster RAISES the bar for everybody --
	 * three expected votes need quorum 2, which two still meet. */
	check_predicate_with_peer(1u, 2u, 1u, 3u, 1, 2u,
		"a peer expecting 3 votes: quorum 2, and 2 votes meet it");

	/* ... but a peer expecting five does not: quorum 3, and there are 2. */
	check_predicate_with_peer(1u, 2u, 1u, 5u, 0, 3u,
		"a peer expecting 5 votes: quorum 3, and 2 votes do NOT");

	/* Three votes between two systems DO meet that bar. */
	check_predicate_with_peer(2u, 2u, 1u, 5u, 1, 3u,
		"VOTES=2 + a seen VOTES=1/EV=5 peer: 3 votes meet quorum 3");
}

/* ==========================================================================
 * 2. The mint: the published construction, shared with the joiner
 * ========================================================================== */

static void test_csid_construction(void)
{
	printf("[genesis] the CSID construction (generation << 16 | SYSID & 0x3ff)\n");

	ct_check_eq_u32(vms_cm_csid_of(1u, FOUNDER_SYSID), FOUNDER_CSID,
			"generation 1 over SCSSYSTEMID 1025 is CSID 0x00010001");
	/* ... and that coincidence is exactly why the ASSIGNMENT rule may not
	 * be read off it: 1025's bottom ten bits and its CSV slot are both 1.
	 * test_founder_slot_is_the_round_robin_one() below separates them. */
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

	printf("[genesis] a node whose visible systems have quorum forms a cluster\n");
	bed_init(1u, 1u);

	/* Before: no identity, no membership. */
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"before: this node holds NO cluster system id");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER, "before: not a member");

	rc = cnxman_coord_found(&g.c, NULL);
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

	/*
	 * AND THE FOUNDER SAID SO (rd vms-151). A joiner announces its
	 * membership from the join FSM; a founder never runs that FSM, so until
	 * this the one node that formed the cluster was the one node whose
	 * console never said it was in one -- and the in-browser proof reads
	 * exactly that line off both consoles.
	 */
	ct_check(strcmp(g.fake.last_log,
			"%CNXMAN, this node is now a VAXcluster member") == 0,
		 "the founder announces its membership in the same words a "
		 "joiner does, AFTER phase2 committed it");

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
	ct_check_eq_u32((unsigned long)cnxman_coord_found(&g.c, NULL), 0u,
			"the first call founds");
	csid_before = g.cl.club.local_csid;
	epoch_before = g.cl.club.epoch;

	rc = cnxman_coord_found(&g.c, NULL);
	ct_check(rc != 0, "the second call REFUSES");
	ct_check_eq_u32(g.c.last_refusal, (unsigned long)CNXMAN_COORD_REF_BUSY,
			"... because this node already holds a cluster system id");
	ct_check_eq_u32(g.cl.club.local_csid, csid_before,
			"the CSID was not re-minted");
	ct_check_eq_u32(g.cl.club.epoch, epoch_before,
			"no second transition was opened");
	ct_check_eq_u32(g.c.genesis_opens, 1u, "still exactly one founding");
}

/* ==========================================================================
 * 4. THE FOUNDER'S CSV SLOT -- round-robin, not `SCSSYSTEMID & 0x3ff`
 *    (rd vms-3a7c settled the rule; rd vms-151 moved the founding path onto it)
 * ==========================================================================
 *
 * The falsified rule was invisible in this suite because its founder's system
 * id (1025) has bottom ten bits equal to its slot. Three system ids that do NOT
 * are what separates the two readings -- and all three are system ids a real
 * deployment uses: the in-browser demo's own nodes run 1987 and 1990, and under
 * the old rule NEITHER of them could form a cluster at all.
 */
static void check_founds_with_slot_1(vms_scs_sysid_t sysid, const char *what)
{
	struct vms_csb *local;

	bed_init_sysid(1u, 1u, sysid);
	printf("  -- %s\n", what);
	ct_check_eq_u32((unsigned long)cnxman_coord_found(&g.c, NULL), 0u,
			"it founds");
	ct_check_eq_u32(g.cl.club.local_csid, FOUNDER_CSID,
			"... taking CSV slot 1 at generation 1, whatever its "
			"SCSSYSTEMID's bottom ten bits are");
	local = cnxman_club_local(&g.cl.club);
	ct_check(local != NULL && cnxman_csb_is_member(local),
		 "... and phase2 committed it a member");
	ct_check_eq_u32((unsigned long)g.cl.state,
			(unsigned long)VMS_CLUSTER_MEMBER, "... cl->state agrees");
}

static void test_founder_slot_is_the_round_robin_one(void)
{
	printf("[genesis] the founder takes CSV slot 1, not SCSSYSTEMID & 0x3ff\n");
	check_founds_with_slot_1((vms_scs_sysid_t)1987u,
				 "SCSSYSTEMID 1987 (& 0x3ff = 963: slot 963 is "
				 "unnameable, and the old rule refused it)");
	check_founds_with_slot_1((vms_scs_sysid_t)1024u,
				 "SCSSYSTEMID 1024 (& 0x3ff = 0: slot 0 is never "
				 "used, and the old rule refused it)");
	check_founds_with_slot_1((vms_scs_sysid_t)1032u,
				 "SCSSYSTEMID 1032 (& 0x3ff = 8: one past the "
				 "grounded bitmap byte)");
}

/* ==========================================================================
 * 5. THE ELECTION -- two fresh systems that can see each other (vms-151)
 * ==========================================================================
 *
 * The measured deadlock: two fresh OVMX nodes open their circuits, each holds a
 * CSB for the other, neither is in a cluster, and neither could form one. Both
 * asked the other for admission first and were declined (the evidence below is
 * the join FSM's own count of those completed rounds), so what has to break the
 * symmetry is the election -- and it must break it the SAME WAY on both nodes,
 * from numbers both of them hold.
 */
static int form_verdict(vms_scs_sysid_t own, vms_scs_sysid_t peer_sysid,
			uint16_t peer_votes, uint8_t peer_params_known,
			uint32_t rounds)
{
	struct cnxman_form_evidence ev;
	struct vms_csb *peer;

	bed_init_sysid(1u, 1u, own);
	peer = cnxman_club_alloc_csb(&g.cl.club, peer_sysid, 1);
	ct_check(peer != NULL, "the peer CSB exists");
	if (peer_params_known)
		cnxman_csb_set_params(peer, peer_votes, 0u, 0u);
	ev.admission_rounds = rounds;
	return cnxman_coord_found(&g.c, &ev);
}

/*
 * The same decision for the CONFIGURATION THE ORACLE RUNS (rd vms-6d3d): both
 * systems VOTES=1 / EXPECTED_VOTES=2, each with the other's PARAMS really
 * learned over a circuit that is really OPEN, so the peer is in p. 7-6 step 1's
 * proposed set and its vote is really in the sum.
 */
static int form_verdict_seen(vms_scs_sysid_t own, uint16_t own_votes,
			     uint16_t own_expected, vms_scs_sysid_t peer_sysid,
			     uint16_t peer_votes, uint16_t peer_expected,
			     uint32_t rounds)
{
	struct cnxman_form_evidence ev;
	struct vms_csb *peer;

	bed_init_sysid(own_votes, own_expected, own);
	peer = cnxman_club_alloc_csb(&g.cl.club, peer_sysid, 1);
	ct_check(peer != NULL, "the peer CSB exists");
	cnxman_csb_set_params(peer, peer_votes, peer_expected, 0u);
	peer->state = (uint8_t)VMS_CNXMAN_CSB_OPEN;
	ev.admission_rounds = rounds;
	return cnxman_coord_found(&g.c, &ev);
}

/*
 * THE DEFECT, END TO END (rd vms-6d3d). Two systems configured the documented
 * way -- VOTES=1, EXPECTED_VOTES=2 -- that can see each other. Before this
 * change NEITHER could found (each own vote is short of quorum 2) and the pair
 * hung forever; the oracle forms this cluster in 3.2 s.
 */
static void test_two_node_cold_formation(void)
{
	int a, b;

	printf("[genesis] the documented 2-node VMScluster forms from cold\n");

	a = form_verdict_seen(1987u, 1u, 2u, 1990u, 1u, 2u, 1u);
	ct_check_eq_u32((unsigned long)a, 0u,
			"VOTES=1/EV=2 founds on the COMBINED two votes");
	ct_check_eq_u32(g.cl.club.local_csid, FOUNDER_CSID,
			"... at generation 1, CSV slot 1");
	ct_check_eq_u32((unsigned long)g.cl.state,
			(unsigned long)VMS_CLUSTER_MEMBER,
			"... and phase2 committed it a member");
	ct_check_eq_u32(g.sends + g.responds, 0u,
			"still not one frame: the peer joins on the ordinary "
			"op-0x02 path, it is not a participant of this "
			"transition");

	/* ... and the OTHER node, deciding from the same two real numbers,
	 * stands down. Exactly one founder, which is what stops a partition. */
	b = form_verdict_seen(1990u, 1u, 2u, 1987u, 1u, 2u, 1u);
	ct_check(b != 0, "the HIGHER SCSSYSTEMID does NOT also form one");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_OUTRANKED,
			"... it stands down for the other candidate");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "... minting nothing");
	ct_check((a == 0) != (b == 0), "EXACTLY ONE of the two founds");

	/* AND A NODE ALONE IN THAT CONFIGURATION STILL DOES NOT FOUND -- the
	 * oracle's eighteen silent minutes. This is the INV-6 half: the fix is
	 * "count the votes that are really there", never "lower the bar". */
	bed_init_sysid(1u, 2u, 1987u);
	ct_check(cnxman_coord_found(&g.c, NULL) != 0,
		 "alone, VOTES=1/EV=2 REFUSES to form");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_NO_QUORUM,
			"... naming the honest reason: no quorum");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "... and mints nothing");
	ct_check(strcmp(g.fake.last_log,
			"%CNXMAN, the systems this node can see do not have "
			"quorum; waiting to form or join an OpenVMS Cluster") == 0,
		 "... and SAYS so on OPA0:, rather than hanging silently");
}

/*
 * ... AND THE ELECTION IS OVER SYSTEMS THAT COULD ACTUALLY FORM. A candidate is
 * ranked only if the SAME p. 7-6 predicate this node's own gate applies says
 * that system could have formed this cluster. So a node never reports "another
 * system takes precedence" about a system which could not have taken it: the
 * refusal it gives is the true one.
 */
static void test_ineligible_candidate_never_wins(void)
{
	int rc;

	printf("[genesis] an ineligible candidate is not deferred to\n");

	/* A LOWER-numbered peer whose own EXPECTED_VOTES is 5: the set's two
	 * votes cannot reach quorum 3, so NEITHER system can form -- and the
	 * refusal must name that, not the peer. */
	rc = form_verdict_seen(1990u, 1u, 2u, 1900u, 1u, 5u, 1u);
	ct_check(rc != 0, "it does not form");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_NO_QUORUM,
			"... for want of QUORUM, not for the lower-numbered "
			"system that could not have formed it either");
	ct_check_eq_u32(g.c.genesis_refused_outranked, 0u,
			"... and no deferral was recorded");
	ct_check_eq_u32(g.c.deferred_to_valid, 0u,
			"... nor any system stood down for");

	/* The SAME peer, one SYSGEN digit apart: EXPECTED_VOTES=2 makes it a
	 * system that really could form this cluster, and now it IS deferred
	 * to. One digit is the whole difference between the two refusals. */
	rc = form_verdict_seen(1990u, 1u, 2u, 1900u, 1u, 2u, 1u);
	ct_check(rc != 0, "it still does not form");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_OUTRANKED,
			"... but now because that system takes precedence");
	ct_check_eq_u32(g.c.deferred_to_valid, 1u, "... and it is named");
	ct_check_eq_u32(g.c.deferred_to_sysid, 1900u, "... by its real sysid");
}

static void test_election_elects_exactly_one(void)
{
	int a, b;

	printf("[genesis] two fresh systems elect exactly ONE founder\n");

	/* Run BOTH sides of the same pair, with the inputs each of them really
	 * holds. Not "assert A founds": assert that of the two symmetric
	 * decisions exactly one is a founding. */
	a = form_verdict(1987u, 1990u, 1u, 1u, 1u);
	ct_check_eq_u32((unsigned long)a, 0u,
			"the LOWER SCSSYSTEMID (1987) forms the cluster");
	ct_check_eq_u32(g.cl.club.local_csid, FOUNDER_CSID,
			"... at generation 1, CSV slot 1");
	ct_check_eq_u32((unsigned long)g.cl.state,
			(unsigned long)VMS_CLUSTER_MEMBER,
			"... and phase2 committed it a member");
	ct_check_eq_u32(g.c.genesis_opens, 1u, "one founding transition");
	ct_check_eq_u32(g.sends + g.responds, 0u,
			"still not one frame: the peer is not a participant of "
			"a transition it was never proposed");

	b = form_verdict(1990u, 1987u, 1u, 1u, 1u);
	ct_check(b != 0, "the HIGHER SCSSYSTEMID (1990) does NOT form one");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_OUTRANKED,
			"... it stands down for the other candidate");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"... minting nothing while it waits to be admitted");

	ct_check((a == 0) != (b == 0),
		 "EXACTLY ONE of the two symmetric decisions founds -- the "
		 "property that makes this an election and not a race");
}

/*
 * ... AND A PEER THAT CAN NEVER FOUND IS NOT DEFERRED TO. FORM requires the
 * coordinator to have VOTES > 0 (pp. 7-28, 7-33), so standing down for a
 * zero-vote system is the same deadlock in a new shape: it would wait forever
 * for a node that is structurally incapable of forming anything.
 */
static void test_zero_vote_peer_is_not_a_rival(void)
{
	int rc;

	printf("[genesis] a VOTES=0 peer is not a founding rival\n");

	/* Lower SCSSYSTEMID than ours -- it would win the election if it were a
	 * candidate at all -- but its own PARAMS record says zero votes. */
	rc = form_verdict(1990u, 1987u, 0u, 1u, 1u);
	ct_check_eq_u32((unsigned long)rc, 0u,
			"this node founds despite the lower-numbered peer");
	ct_check_eq_u32((unsigned long)g.cl.state,
			(unsigned long)VMS_CLUSTER_MEMBER, "... and is a member");

	/* But a peer whose PARAMS have NOT arrived is UNKNOWN, and unknown is
	 * treated as a rival: standing down costs a beat, forming beside a
	 * system this node has not finished listening to costs a partition. */
	rc = form_verdict(1990u, 1987u, 0u, 0u, 1u);
	ct_check(rc != 0, "a peer whose votes are not yet known DOES block it");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_OUTRANKED,
			"... as an OUTRANKED refusal");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "nothing was minted");
}

/*
 * AND NOBODY FORMS BEFORE ASKING. Clause (2): a system that is present has to
 * be asked to admit this node before this node concludes there is no cluster
 * here -- a member answers such a request in milliseconds, and a system that is
 * not in a cluster cannot answer it at all. Evidence of zero completed rounds
 * refuses even the election's winner.
 */
static void test_unasked_peer_blocks_the_winner(void)
{
	int rc;

	printf("[genesis] even the election winner asks before it forms\n");
	rc = form_verdict(1987u, 1990u, 1u, 1u, 0u);
	ct_check(rc != 0, "with no completed admission round it REFUSES");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_PEER_UNASKED,
			"... naming the unasked system as the reason");
	ct_check_eq_u32(g.c.genesis_refused_unasked, 1u, "and counting it");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "nothing was minted");

	/* ... while a node that can see NOBODY needs no round: there was
	 * nobody to ask. This is the shipped single-node founding, unchanged. */
	bed_init(1u, 1u);
	ct_check_eq_u32((unsigned long)cnxman_coord_found(&g.c, NULL), 0u,
			"a node alone still founds with no evidence at all");
}

int main(void)
{
	test_predicate_truth_table();
	test_csid_construction();
	test_founds_and_becomes_member();
	test_second_call_refuses();
	test_founder_slot_is_the_round_robin_one();
	test_election_elects_exactly_one();
	test_two_node_cold_formation();
	test_ineligible_candidate_never_wins();
	test_zero_vote_peer_is_not_a_rival();
	test_unasked_peer_blocks_the_winner();
	return ct_summary("test_cnxman_genesis");
}
