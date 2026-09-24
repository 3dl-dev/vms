/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_cnxman_genesis_negctl.c - the HONEST-OMISSION negative control for
 * cluster GENESIS (test-ladder rung R1; docs/design-cluster-genesis.md).
 *
 * THIS IS THE TEETH. Genesis is the one place in the whole cluster stack where
 * the executive asserts a value nobody gave it: its own CSID. The predecessor
 * of this stack defaulted that CSID to 1 from an insmod parameter and "became a
 * phantom cluster of one"; #1052 removed it under INV-6 precisely because an
 * unconditional mint is a fabricated membership. This suite is the control that
 * would have caught that default, and it is what keeps the replacement honest:
 *
 *   A NODE THAT MAY NOT FOUND MINTS NOTHING. Not a CSID, not a membership, not
 *   a transition, not a byte of CLUB state -- `local_csid_valid` stays 0, the
 *   node state stays JOINING, and the CLUB is byte-for-byte what it was.
 *
 * The five ways a node may not found, each proved with the CLUB compared
 * before and after:
 *
 *   1. VOTES = 0 -- the DEFAULT (vms_cluster.h: "0 first, design D-10"), so
 *      this is also the proof that a tree carrying genesis behaves EXACTLY as
 *      the tree without it on every node nobody configured to be a founder.
 *   2. VOTES > 0 but short of quorum -- the two-node protection: a pair each
 *      holding one vote with EXPECTED_VOTES=2 form NOTHING and must find each
 *      other.
 *   3. ANOTHER SYSTEM IS PRESENT -- a real VAX (or another OVMX) this node
 *      holds a CSB for. Even fully quorum-eligible, this node JOINS what is
 *      there rather than founding a cluster beside it. Founding a singleton
 *      next to a live coordinator is the partition that hurts the OTHER side.
 *   4. THE CSID WOULD NOT BE EXPRESSIBLE -- an SCSSYSTEMID whose CSV slot
 *      falls outside the membership-map byte the wire has actually grounded.
 *      The honest answer is to refuse and say so, never to mint an identity
 *      the protocol cannot name.
 *   5. A TRANSITION IS ALREADY IN PROGRESS.
 *
 * And the repetition case: the founding attempt runs on the reconnect beat,
 * once a second, forever. A thousand refusals must still be a thousand
 * refusals -- nothing accumulates toward a mint.
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
#include "vms_cnxman_coord_fsm.h"

#define FOUNDER_SYSID 1025u   /* low ten bits = CSV slot 1 */

struct bed {
	struct vms_cluster   cl;
	struct cnxman_ops    ops;
	struct fake_cnx      fake;
	struct cnxman_coord  c;
	uint32_t             sends;
	struct vms_club      club_before;
};

static struct bed g;

static int bed_send(void *ctx, vms_csid_t dst, const uint8_t *body, uint32_t len)
{
	(void)ctx; (void)dst; (void)body; (void)len;
	g.sends++;
	return 0;
}

static void bed_init(uint16_t votes, uint16_t expected_votes,
		     vms_scs_sysid_t sysid)
{
	memset(&g, 0, sizeof(g));
	fake_ops_init(&g.ops, &g.fake);
	g.ops.send = bed_send;
	g.fake.now_ms = 100000u;

	memcpy(g.cl.params.scsnode, "OVMX01", 6);
	g.cl.params.scsnode_len = 6;
	g.cl.params.scssystemid = sysid;
	g.cl.params.vaxcluster = 2u;   /* "always a member" -- the ONLY setting
					* that may form one at all */
	g.cl.params.votes = votes;
	g.cl.params.expected_votes = expected_votes;

	(void)cnxman_club_init(&g.cl);
	g.cl.state = VMS_CLUSTER_JOINING;

	cnxman_coord_init(&g.c, &g.cl, &g.ops);
}

/* Freeze the whole CLUB, so "nothing was minted" can be asserted as "not one
 * byte moved" rather than as a list of fields somebody remembered to check. */
static void bed_snapshot(void)
{
	g.club_before = g.cl.club;
}

/* The INV-6 teeth in one line: a refusal moved not one byte of the CLUB. */
static void bed_check_unchanged(const char *what)
{
	ct_check(memcmp(&g.club_before, &g.cl.club, sizeof(g.cl.club)) == 0,
		 what);
}

static void check_refused(enum cnxman_coord_refusal want_refusal,
			  const char *what)
{
	int rc = cnxman_coord_found(&g.c, NULL);

	printf("  -- %s\n", what);
	ct_check(rc != 0, "cnxman_coord_found() REFUSED");
	ct_check_eq_u32(g.c.last_refusal, (unsigned long)want_refusal,
			"... for the stated reason");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"NO cluster system id was minted");
	ct_check_eq_u32(g.cl.club.local_csid, 0u,
			"... and local_csid is 0 = none, not node zero (E30)");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER,
		 "the node is NOT a member of anything");
	ct_check_eq_u32((unsigned long)g.cl.state,
			(unsigned long)VMS_CLUSTER_JOINING,
			"... it is still waiting to form or join");
	/* Not "transition_active == 0": one of the cases below refuses BECAUSE
	 * a transition is already running. The honest assertion is that this
	 * attempt opened none -- the flag is exactly what it was. */
	ct_check_eq_u32(g.cl.club.transition_active,
			g.club_before.transition_active,
			"this attempt opened no transition");
	ct_check_eq_u32(g.cl.club.cluster_nodes, 0u, "no member count committed");
	ct_check_eq_u32(g.sends, 0u, "nothing was sent");
	ct_check(memcmp(&g.club_before, &g.cl.club, sizeof(g.cl.club)) == 0,
		 "the CLUB is byte-for-byte what it was before the attempt");
}

/* ==========================================================================
 * 1. VOTES = 0 -- the default, and the removed LARP's own case
 * ========================================================================== */

static void test_votes_zero_never_founds(void)
{
	printf("[negctl] VOTES=0 (the SYSGEN default)\n");
	bed_init(0u, 0u, (vms_scs_sysid_t)FOUNDER_SYSID);

	ct_check(!cnxman_quorum_form_votes_suffice(&g.cl, (const struct cnxman_form_set *)0, (uint16_t *)0),
		 "the predicate says NO before anything is attempted");
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_QUORUM,
		      "a non-voting node asked to found");
	/* negctl: coord-genesis-refusal-uncounted */
	ct_check_eq_u32(g.c.genesis_refused_noquorum, 1u,
			"the refusal is COUNTED (the anti-LARP tripwire)");
	ct_check_eq_u32(g.c.genesis_opens, 0u, "no founding transition opened");
}

/* Even with EXPECTED_VOTES set -- a node that knows it is part of a 4-vote
 * cluster but holds none of them. */
static void test_votes_zero_with_expected(void)
{
	printf("[negctl] VOTES=0, EXPECTED_VOTES=4\n");
	bed_init(0u, 4u, (vms_scs_sysid_t)FOUNDER_SYSID);
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_QUORUM,
		      "a non-voting node in a 4-vote cluster asked to found");
}

/* ==========================================================================
 * 2. VOTES > 0 but short of quorum -- the two-node protection
 * ========================================================================== */

static void test_subquorum_never_founds(void)
{
	uint16_t quorum = 0u;

	printf("[negctl] VOTES=1, EXPECTED_VOTES=2 (each half of a pair)\n");
	bed_init(1u, 2u, (vms_scs_sysid_t)FOUNDER_SYSID);

	ct_check(!cnxman_quorum_form_votes_suffice(&g.cl,
			(const struct cnxman_form_set *)0, &quorum),
		 "one vote, and NOBODY IN SIGHT, does not satisfy a two-vote "
		 "quorum -- the oracle's eighteen silent minutes (rd vms-6d3d)");
	ct_check_eq_u32(quorum, 2u, "... QUORUM = (2+2)/2 = 2 (p. 7-6)");
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_QUORUM,
		      "a sub-quorum voting node asked to found");
	/* negctl: coord-genesis-refusal-uncounted */
	ct_check_eq_u32(g.c.genesis_refused_noquorum, 1u, "counted");
}

/* The same node after a cluster it was really in: Old CEVOTES stands in the
 * max{} (pp. 7-10/7-11), so it may not re-found on its one vote. */
static void test_old_cevotes_blocks_refounding(void)
{
	printf("[negctl] VOTES=1 with the CLUB carrying CEVOTES=5\n");
	bed_init(1u, 1u, (vms_scs_sysid_t)FOUNDER_SYSID);
	g.cl.club.cevotes = 5u;
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_QUORUM,
		      "a node that was in a 5-vote cluster asked to re-found it");
}

/* ==========================================================================
 * 3. ANOTHER SYSTEM IS PRESENT -- join it, never found beside it
 * ========================================================================== */

static void test_peer_present_joins_not_founds(void)
{
	struct vms_csb *peer;

	printf("[negctl] a fully quorum-eligible node WITH a peer present\n");
	bed_init(1u, 1u, (vms_scs_sysid_t)FOUNDER_SYSID);

	/* Discovered, not yet admitted: exactly what a booting OVMX holds a
	 * moment after it hears a real VAX's HELLO and opens a circuit. It has
	 * no CSID -- learning one is what the join is FOR -- so the gate may
	 * NOT be keyed on membership. */
	peer = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)1026u, 1);
	cnxman_csb_set_scsnode(peer, (const uint8_t *)"VAX1", 4u);

	ct_check(cnxman_quorum_form_votes_suffice(&g.cl, (const struct cnxman_form_set *)0, (uint16_t *)0),
		 "this node WOULD satisfy quorum on its own votes");
	bed_snapshot();
	/*
	 * vms-151: the reason is now NAMED rather than lumped into BUSY. That
	 * system has not been asked to admit this node yet -- no admission
	 * round has completed (the NULL evidence check_refused() passes) -- so
	 * asking it comes before forming anything.
	 */
	check_refused(CNXMAN_COORD_REF_PEER_UNASKED,
		      "an eligible node with a discovered, unasked system present");
	ct_check_eq_u32(g.c.genesis_refused_unasked, 1u,
			"the refusal is counted as an UNASKED refusal");
	ct_check_eq_u32(g.c.genesis_opens, 0u, "no founding transition opened");
}

/* And with the peer already a committed member -- a cluster this node is
 * connected to but has not been admitted into yet. */
static void test_member_present_never_founds(void)
{
	struct vms_csb *peer;

	printf("[negctl] an eligible node beside an established member\n");
	bed_init(2u, 2u, (vms_scs_sysid_t)FOUNDER_SYSID);
	peer = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)1026u, 1);
	cnxman_csb_set_csid(peer, 0x00010002u);
	cnxman_csb_set_flags(peer, (uint16_t)(VMS_CSB_F_SELECTED |
					      VMS_CSB_F_MEMBER));
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_PEER_CLUSTER,
		      "an eligible node beside a system already in a cluster");
	ct_check_eq_u32(g.c.genesis_refused_peer, 1u,
			"the refusal is counted as a PEER-IN-CLUSTER refusal");
}

/*
 * ... AND IT STAYS REFUSED HOWEVER MANY ADMISSION ROUNDS HAVE BEEN EXHAUSTED
 * (vms-151, the interop footgun). Clause (2) of the election lets a node form
 * once every visible system declined to admit it -- but a system that HOLDS A
 * CSID is in a cluster whatever it did with the request, and forming a
 * singleton beside it is the partition the design forbids. So clause (1) is
 * tested ahead of the evidence and no amount of evidence overrides it.
 */
static void test_member_present_outranks_any_evidence(void)
{
	struct cnxman_form_evidence ev;
	struct vms_csb *peer;
	int rc;

	printf("[negctl] a peer HOLDING A CSID refuses forming, evidence or not\n");
	bed_init(1u, 1u, (vms_scs_sysid_t)FOUNDER_SYSID);
	/* Lower SCSSYSTEMID than ours, so the election would elect US. */
	peer = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)1020u, 1);
	cnxman_csb_set_scsnode(peer, (const uint8_t *)"VAX9", 4u);
	cnxman_csb_set_csid(peer, 0x00010002u);
	bed_snapshot();

	ev.admission_rounds = 99u;
	rc = cnxman_coord_found(&g.c, &ev);
	ct_check(rc != 0, "cnxman_coord_found() REFUSED");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_PEER_CLUSTER,
			"... because that system holds a cluster system id");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "nothing was minted");
	bed_check_unchanged("the CLUB is byte-for-byte what it was");
}

/*
 * THE ELECTION'S NEGATIVE HALF (vms-151 clause (3)). Two fresh systems, both
 * quorum-eligible, neither in a cluster, each having asked the other for
 * admission and been declined. Exactly one may form, and the one that may NOT
 * is the one with the higher SCSSYSTEMID -- it mints nothing and stays where it
 * was, because in a moment it will be joining the other one.
 */
static void test_election_loser_never_founds(void)
{
	struct cnxman_form_evidence ev;
	struct vms_csb *peer;
	int rc;

	printf("[negctl] the node that loses the founding election forms nothing\n");
	bed_init(1u, 1u, (vms_scs_sysid_t)1990u);
	peer = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)1987u, 1);
	cnxman_csb_set_scsnode(peer, (const uint8_t *)"OVMXA", 5u);
	/* A REAL advert: its own PARAMS record said VOTES=1, so it genuinely
	 * can form and deferring to it is not a deadlock. */
	cnxman_csb_set_params(peer, 1u, 0u, 0u);
	bed_snapshot();

	ev.admission_rounds = 1u;   /* it was asked, and it did not admit us */
	rc = cnxman_coord_found(&g.c, &ev);
	ct_check(rc != 0, "cnxman_coord_found() REFUSED");
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_OUTRANKED,
			"... because another founding candidate ranks ahead");
	ct_check_eq_u32(g.c.genesis_refused_outranked, 1u,
			"the refusal is counted as an OUTRANKED refusal");
	ct_check_eq_u32(g.c.deferred_to_valid, 1u,
			"the system stood down for is RECORDED");
	ct_check_eq_u32((unsigned long)g.c.deferred_to_sysid, 1987u,
			"... read out of that peer's own CSB");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "nothing was minted");
	ct_check_eq_u32(g.c.genesis_opens, 0u, "no founding transition opened");
	bed_check_unchanged("the CLUB is byte-for-byte what it was");
}

/* ==========================================================================
 * 4. An identity the grounded wire cannot name
 * ========================================================================== */

static void test_unexpressible_csid_refused(void)
{
	printf("[negctl] no CSV slot left that the nodemap byte can hold\n");
	/*
	 * vms-151 / vms-3a7c: the founder's slot is the round-robin one
	 * coord_next_slot() hands out, NOT `SCSSYSTEMID & 0x3ff` -- so this
	 * refusal is no longer reachable by choosing an awkward system id (see
	 * test_cnxman_genesis.c's founder-slot case, which proves the two
	 * system ids that used to trip it now found normally). What still
	 * reaches it is the real condition it guards: every slot the grounded
	 * membership bitmap byte can name is already spoken for, so the next
	 * one would be 8 and no transition could ever name this member (spec
	 * sec 4(p): "do not assume 8 slots").
	 */
	bed_init(1u, 1u, (vms_scs_sysid_t)FOUNDER_SYSID);
	g.c.max_slot_seen = 7u;   /* the highest slot the byte can express */
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_SLOT,
		      "an eligible node with no expressible CSV slot left");
}

/* ==========================================================================
 * 5. A transition is already running
 * ========================================================================== */

static void test_transition_active_refused(void)
{
	printf("[negctl] a transition is already in progress\n");
	bed_init(1u, 1u, (vms_scs_sysid_t)FOUNDER_SYSID);
	g.cl.club.transition_active = 1u;   /* granted to another connection
					     * manager (p. 7-30/7-32) */
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_BUSY,
		      "an eligible node while another transition is open");
}

/* ==========================================================================
 * 6. The beat: refusing forever stays refusing
 * ========================================================================== */

static void test_repeated_attempts_never_drift(void)
{
	uint32_t i;

	printf("[negctl] a thousand beats of a VOTES=0 node\n");
	bed_init(0u, 1u, (vms_scs_sysid_t)FOUNDER_SYSID);
	bed_snapshot();

	for (i = 0; i < 1000u; i++) {
		g.fake.now_ms += 1000u;
		(void)cnxman_coord_found(&g.c, NULL);
	}

	/* negctl: coord-genesis-refusal-uncounted */
	ct_check_eq_u32(g.c.genesis_refused_noquorum, 1000u,
			"every attempt was refused, and counted");
	/*
	 * ... AND THE OPERATOR HEARD IT ONCE (rd vms-151). The beat is once a
	 * second; a standing refusal that spoke on every one of them would put
	 * a thousand identical lines on OPA0:, which is exactly what the
	 * two-node rig printed once these lines reached the console at all.
	 * Counted state is not speech: the 1000 above and the 1 here are the
	 * same thousand refusals.
	 */
	ct_check_eq_u32(g.fake.logs, 1u,
			"the standing refusal was SAID once, not a thousand times");
	ct_check_eq_u32(g.c.genesis_opens, 0u, "not one founding transition");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"still NO cluster system id after 1000 attempts");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER, "still not a member");
	ct_check(memcmp(&g.club_before, &g.cl.club, sizeof(g.cl.club)) == 0,
		 "the CLUB never moved");
}

/*
 * ... BUT A REFUSAL THAT CHANGES IS NEWS (rd vms-151). Silence while a
 * situation holds must not become silence about a DIFFERENT situation: a node
 * that was waiting for an answer, then stood down for a rival, then found
 * itself beside a real cluster has three different things to tell the
 * operator, and each one is said exactly once. Every reason here is driven by
 * changing the REAL state the gate reads -- the evidence the join FSM hands
 * over, and the peer's own CSB -- never by poking the latch.
 */
static void test_refusal_speaks_again_when_the_reason_changes(void)
{
	struct cnxman_form_evidence ev;
	struct vms_csb *peer;
	uint32_t i;

	printf("[negctl] a refusal is said once per REASON, not once per beat\n");
	bed_init(1u, 1u, (vms_scs_sysid_t)1990u);
	peer = cnxman_club_alloc_csb(&g.cl.club, (vms_scs_sysid_t)1987u, 1);
	cnxman_csb_set_scsnode(peer, (const uint8_t *)"OVMXA", 5u);
	cnxman_csb_set_params(peer, 1u, 0u, 0u);   /* a real VOTES=1 advert */

	/* (1) the peer has not been asked yet: no admission round has ended. */
	ev.admission_rounds = 0u;
	for (i = 0; i < 6u; i++) {
		g.fake.now_ms += 1000u;
		(void)cnxman_coord_found(&g.c, &ev);
	}
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_PEER_UNASKED,
			"six beats, refused because the peer is unasked");
	ct_check_eq_u32(g.fake.logs, 1u, "... said once");

	/* (2) the rounds ran out and the peer outranks us: a NEW reason. */
	ev.admission_rounds = 1u;
	for (i = 0; i < 6u; i++) {
		g.fake.now_ms += 1000u;
		(void)cnxman_coord_found(&g.c, &ev);
	}
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_OUTRANKED,
			"six more beats, refused because the peer outranks us");
	ct_check_eq_u32(g.fake.logs, 2u, "... said once more, and only once");

	/* (3) that peer is now in a cluster: a third reason, and the one that
	 *     matters most for interop safety. */
	cnxman_csb_set_csid(peer, 0x00010001u);
	for (i = 0; i < 6u; i++) {
		g.fake.now_ms += 1000u;
		(void)cnxman_coord_found(&g.c, &ev);
	}
	ct_check_eq_u32(g.c.last_refusal,
			(unsigned long)CNXMAN_COORD_REF_PEER_CLUSTER,
			"six more, refused because that system is in a cluster");
	ct_check_eq_u32(g.fake.logs, 3u, "... said once more, and only once");
	ct_check(strstr(g.fake.last_log, "already belongs to an OpenVMS "
					 "Cluster") != NULL,
		 "... and what OPA0: last heard is that reason, not the old one");

	/* Through all eighteen beats: every refusal counted, nothing minted. */
	ct_check_eq_u32(g.c.genesis_refused_unasked, 6u,
			"every unasked refusal is counted, said or not");
	ct_check_eq_u32(g.c.genesis_refused_outranked, 6u,
			"every outranked refusal is counted, said or not");
	ct_check_eq_u32(g.c.genesis_refused_peer, 6u,
			"every in-a-cluster refusal is counted, said or not");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u, "nothing was minted");
}

int main(void)
{
	test_votes_zero_never_founds();
	test_votes_zero_with_expected();
	test_subquorum_never_founds();
	test_old_cevotes_blocks_refounding();
	test_peer_present_joins_not_founds();
	test_member_present_never_founds();
	test_member_present_outranks_any_evidence();
	test_election_loser_never_founds();
	test_unexpressible_csid_refused();
	test_transition_active_refused();
	test_repeated_attempts_never_drift();
	test_refusal_speaks_again_when_the_reason_changes();
	return ct_summary("test_cnxman_genesis_negctl");
}
