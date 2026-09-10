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

static void check_refused(enum cnxman_coord_refusal want_refusal,
			  const char *what)
{
	int rc = cnxman_coord_found(&g.c);

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

	ct_check(!cnxman_quorum_own_votes_suffice(&g.cl, (uint16_t *)0),
		 "the predicate says NO before anything is attempted");
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_QUORUM,
		      "a non-voting node asked to found");
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

	ct_check(!cnxman_quorum_own_votes_suffice(&g.cl, &quorum),
		 "one vote does not satisfy a two-vote quorum");
	ct_check_eq_u32(quorum, 2u, "... QUORUM = (2+2)/2 = 2 (p. 7-6)");
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_QUORUM,
		      "a sub-quorum voting node asked to found");
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

	ct_check(cnxman_quorum_own_votes_suffice(&g.cl, (uint16_t *)0),
		 "this node WOULD satisfy quorum on its own votes");
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_BUSY,
		      "an eligible node with a discovered system present");
	ct_check_eq_u32(g.c.genesis_refused_peer, 1u,
			"the refusal is counted as a PEER refusal");
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
	check_refused(CNXMAN_COORD_REF_BUSY,
		      "an eligible node beside a system already in a cluster");
}

/* ==========================================================================
 * 4. An identity the grounded wire cannot name
 * ========================================================================== */

static void test_unexpressible_csid_refused(void)
{
	printf("[negctl] SCSSYSTEMID whose CSV slot the nodemap byte cannot hold\n");
	/* 1032 & 0x3ff = 8, one past the last slot the membership bitmap byte
	 * has been grounded to carry (spec sec 4(p): "do not assume 8 slots").
	 * Minting it would produce a member no transition could ever name. */
	bed_init(1u, 1u, (vms_scs_sysid_t)1032u);
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_SLOT,
		      "an eligible node whose CSID would be unnameable");

	/* ... and slot 0, which p. 7-25 says is never used. */
	bed_init(1u, 1u, (vms_scs_sysid_t)1024u);
	bed_snapshot();
	check_refused(CNXMAN_COORD_REF_NO_SLOT,
		      "an eligible node whose CSV slot would be 0");
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
		(void)cnxman_coord_found(&g.c);
	}

	ct_check_eq_u32(g.c.genesis_refused_noquorum, 1000u,
			"every attempt was refused, and counted");
	ct_check_eq_u32(g.c.genesis_opens, 0u, "not one founding transition");
	ct_check_eq_u32(g.cl.club.local_csid_valid, 0u,
			"still NO cluster system id after 1000 attempts");
	ct_check(g.cl.state != VMS_CLUSTER_MEMBER, "still not a member");
	ct_check(memcmp(&g.club_before, &g.cl.club, sizeof(g.cl.club)) == 0,
		 "the CLUB never moved");
}

int main(void)
{
	test_votes_zero_never_founds();
	test_votes_zero_with_expected();
	test_subquorum_never_founds();
	test_old_cevotes_blocks_refounding();
	test_peer_present_joins_not_founds();
	test_member_present_never_founds();
	test_unexpressible_csid_refused();
	test_transition_active_refused();
	test_repeated_attempts_never_drift();
	return ct_summary("test_cnxman_genesis_negctl");
}
