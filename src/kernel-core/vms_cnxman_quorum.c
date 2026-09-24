/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_quorum.c - the Quorum Algorithm: CEVOTES/QUORUM computation,
 * tracking only (FC-P3.7).
 *
 * The contract, the grounding and the INV-6 rules are in
 * vms_cnxman_quorum.h. This file is the arithmetic: walk the CLUB's own
 * SELECTED, params_valid CSBs (never a caller-supplied membership list),
 * apply the published max{} formula, and store the result back into the CLUB
 * fields FC-P3.6 declared but deliberately left unwritten.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_quorum.h"

/*
 * p. 7-6: "the selected set of proposed members" is the CSB table's own
 * SELECTED flag (p. 7-49) -- never a caller's opinion. A CSB that never
 * learned its PARAMS record (params_valid == 0) contributes nothing to any
 * sum below: an un-advertised VOTES is unknown, not zero (INV-6).
 */
static int quorum_csb_counts(const struct vms_csb *csb)
{
	if (csb == NULL || !csb->in_use)
		return 0;
	if ((csb->flags & VMS_CSB_F_SELECTED) == 0u)
		return 0;
	return csb->params_valid != 0u;
}

/*
 * p. 7-4/7-5: a member's votes are AVAILABLE (count toward PRESENT) only
 * while it is reachable. The LOCAL CSB always is -- it is this system, not a
 * connection that can be down. A remote CSB is reachable only in state OPEN:
 * p. 7-30 holds membership (SELECTED stays set) across a reconnect window,
 * but that is precisely the case where the system is NOT currently
 * contributing votes.
 */
static int quorum_csb_present(const struct vms_csb *csb)
{
	if (!quorum_csb_counts(csb))
		return 0;
	if ((csb->flags & VMS_CSB_F_LOCAL) != 0u)
		return 1;
	return csb->state == (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

/*
 * p. 7-6 step 2 as ARITHMETIC, with nothing read from a CLUB: New CEVOTES =
 * max{EXPECTED_VOTES; SUM VOTES; Old CEVOTES}. It lives here, as one function
 * two callers share, because the founding predicate below has to apply exactly
 * the formula a running cluster's recompute applies -- a second copy of it is
 * how a node ends up founding on a quorum the rest of the executive would not
 * have agreed to.
 */
uint16_t cnxman_quorum_cevotes(uint16_t old_cevotes, uint32_t expected_votes,
			       uint32_t sum_votes)
{
	uint32_t cevotes = (uint32_t)old_cevotes;

	if (expected_votes > cevotes)
		cevotes = expected_votes;
	if (sum_votes > cevotes)
		cevotes = sum_votes;
	if (cevotes > 0xffffu)
		cevotes = 0xffffu;  /* the CLUB field is 16 bits; VMS's own
				     * VOTES/EXPECTED_VOTES SYSGEN params are
				     * themselves 16-bit, so this clamp never
				     * actually triggers */
	return (uint16_t)cevotes;
}

/* p. 7-6 step 3: QUORUM = (New CEVOTES + 2) / 2. The ONE spelling of it. */
uint16_t cnxman_quorum_of_cevotes(uint16_t cevotes)
{
	return (uint16_t)(((uint32_t)cevotes + 2u) / 2u);
}

/* ==========================================================================
 * FORMING FROM NOTHING -- p. 7-6 over a COLD formation's proposed set
 * (vms_cnxman_quorum.h SS "FORMING FROM NOTHING"; rd vms-6d3d's oracle)
 * ========================================================================== */

/* Defined with the enforcement predicates below; declared here because the
 * founding gate needs the same ONE spelling of "this node's own block". */
static const struct vms_csb *quorum_local_csb(const struct vms_club *club);

/*
 * IS THIS CSB IN A COLD FORMATION'S PROPOSED SET? Deliberately NOT
 * quorum_csb_counts() above: that one requires SELECTED, which is the
 * membership a formation does not have yet. What it shares is the fact that
 * matters -- a CSB that never received a PARAMS record contributes nothing,
 * because an un-advertised VOTES is unknown and not zero (INV-6).
 *
 * Reachability is the same p. 7-4/7-5 rule quorum_csb_present() applies: the
 * LOCAL CSB always counts (it is this system, not a connection that can be
 * down), a remote one only while its circuit is OPEN. The oracle is explicit
 * about the ordering -- the real VAX logged `established connection to node
 * VAX2` BEFORE it proposed the formation their combined votes carried.
 */
int cnxman_quorum_form_set_contains(const struct vms_csb *csb)
{
	if (csb == NULL || !csb->in_use || !csb->params_valid)
		return 0;
	if ((csb->flags & VMS_CSB_F_LOCAL) != 0u)
		return 1;
	return csb->state == (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

void cnxman_quorum_form_set(const struct vms_club *club,
			    struct cnxman_form_set *out)
{
	uint32_t i;

	if (out == NULL)
		return;
	out->sum_votes = 0u;
	out->max_expected = 0u;
	out->n_systems = 0u;
	out->local_counted = 0u;
	if (club == NULL)
		return;

	for (i = 0; i < club->n_csb; i++) {
		const struct vms_csb *csb = &club->csb[i];

		if (!cnxman_quorum_form_set_contains(csb))
			continue;
		out->sum_votes += (uint32_t)csb->votes;
		if ((uint32_t)csb->expected_votes > out->max_expected)
			out->max_expected = (uint32_t)csb->expected_votes;
		out->n_systems++;
		if ((csb->flags & VMS_CSB_F_LOCAL) != 0u)
			out->local_counted = 1u;
	}
}

int cnxman_quorum_could_found(uint16_t old_cevotes, uint16_t votes,
			      uint16_t expected_votes,
			      const struct cnxman_form_set *set,
			      uint16_t *out_quorum)
{
	uint32_t expected;
	uint16_t cevotes;
	uint16_t quorum;

	if (out_quorum != NULL)
		*out_quorum = 0u;
	/*
	 * VOTES == 0 FIRST and on its own terms (pp. 7-28, 7-33). Reaching the
	 * arithmetic with a zero would let an EXPECTED_VOTES of 0 -- a cluster
	 * nobody configured -- produce quorum 1 > 0 and still refuse: correct,
	 * but by accident. The explicit refusal is what makes "a non-voting
	 * system never founds" a property of this function.
	 */
	if (set == NULL || votes == 0u)
		return 0;

	/* p. 7-6 step 2's EXPECTED_VOTES term is the LARGEST one in the
	 * proposed set, and the system being asked about is in it -- but a
	 * caller may ask about a system whose CSB is not (the election's
	 * "could that one have formed this?"), so its own figure is folded in
	 * explicitly rather than assumed to be inside the max already. */
	expected = set->max_expected;
	if ((uint32_t)expected_votes > expected)
		expected = (uint32_t)expected_votes;

	cevotes = cnxman_quorum_cevotes(old_cevotes, expected, set->sum_votes);
	quorum = cnxman_quorum_of_cevotes(cevotes);
	if (out_quorum != NULL)
		*out_quorum = quorum;
	return set->sum_votes >= (uint32_t)quorum;
}

int cnxman_quorum_form_votes_suffice(const struct vms_cluster *cl,
				     const struct cnxman_form_set *set,
				     uint16_t *out_quorum)
{
	struct cnxman_form_set own;
	const struct vms_csb *local;

	if (out_quorum != NULL)
		*out_quorum = 0u;
	if (cl == NULL)
		return 0;
	if (set == NULL) {
		cnxman_quorum_form_set(&cl->club, &own);
		set = &own;
	}
	/*
	 * THE FOUNDER MUST BE IN ITS OWN PROPOSED SET. The same reasoning
	 * cnxman_quorum_enforce_ready() applies to a running member: a sum that
	 * silently omits the local system is worse than no sum.
	 */
	if (!set->local_counted)
		return 0;
	local = quorum_local_csb(&cl->club);
	if (local == NULL)
		return 0;
	/*
	 * ... and the founder's own VOTES / EXPECTED_VOTES are read from that
	 * same block, the one already inside set->sum_votes and
	 * set->max_expected. cnxman_club_init() loaded it from SYSGEN; reading
	 * cl->params again here would be a SECOND source for a number the sum
	 * already took from the first.
	 */
	return cnxman_quorum_could_found(cl->club.cevotes, local->votes,
					 local->expected_votes, set,
					 out_quorum);
}

void cnxman_quorum_recompute(struct vms_club *club)
{
	uint32_t i;
	uint32_t max_expected = 0u;
	uint32_t sum_votes = 0u;
	uint32_t present_votes = 0u;
	uint16_t new_cevotes;

	if (club == NULL)
		return;

	for (i = 0; i < club->n_csb; i++) {
		const struct vms_csb *csb = &club->csb[i];

		if (!quorum_csb_counts(csb))
			continue;
		if ((uint32_t)csb->expected_votes > max_expected)
			max_expected = (uint32_t)csb->expected_votes;
		sum_votes += (uint32_t)csb->votes;
		if (quorum_csb_present(csb))
			present_votes += (uint32_t)csb->votes;
	}

	/*
	 * p. 7-6: New CEVOTES = max{EXPECTED_VOTES; SUM VOTES; Old CEVOTES}.
	 * club->cevotes IS "Old CEVOTES" on entry -- reading the CLUB's own
	 * persisted field back into the max{}, rather than keeping a separate
	 * running-max cache, is what makes the value never decrease on its
	 * own (pp. 7-10/7-11) with no extra bookkeeping to keep in step.
	 *
	 * The max{} and the (CEVOTES+2)/2 are cnxman_quorum_cevotes() and
	 * cnxman_quorum_of_cevotes() above -- the same two the founding
	 * predicate uses, so there is exactly one spelling of each.
	 */
	new_cevotes = cnxman_quorum_cevotes(club->cevotes, max_expected,
					    sum_votes);

	club->cevotes = new_cevotes;
	club->quorum = cnxman_quorum_of_cevotes(new_cevotes);
	club->expected_votes = (uint16_t)max_expected;
	club->quorum_lost = (uint8_t)(present_votes < (uint32_t)club->quorum);

	/* club->qdisk_votes is a TRACKED readback (design SS3.4 lists it among
	 * the fields this item computes), kept in step here even though it
	 * plays no part in the max{} above -- see cnxman_quorum_qdskvotes()'s
	 * own doc comment for why folding it in is P8's job, not this one's. */
	club->qdisk_votes = cnxman_quorum_qdskvotes(club);
}

/* ==========================================================================
 * ENFORCEMENT (FC-P8.1, rd vms-b6d) -- the three PURE predicates
 *
 * The arithmetic above answers "does this node have quorum?". These three
 * answer "may the executive ACT on that answer?", which is a different
 * question and the one the join-transient makes subtle. The contract, the
 * page cites and the honest-zero hazard are in vms_cnxman_quorum.h SS
 * "ENFORCEMENT".
 * ========================================================================== */

/*
 * The LOCAL CSB, or NULL when the CLUB has none. One spelling, shared by the
 * two predicates below and by cnxman_quorum_qdskvotes().
 */
static const struct vms_csb *quorum_local_csb(const struct vms_club *club)
{
	if (club == NULL || club->local_csb < 0)
		return NULL;
	if ((uint32_t)club->local_csb >= club->n_csb)
		return NULL;
	return &club->csb[(uint32_t)club->local_csb];
}

int cnxman_quorum_enforce_ready(const struct vms_cluster *cl)
{
	const struct vms_csb *local;

	if (cl == NULL)
		return 0;
	if (cl->state != VMS_CLUSTER_MEMBER)
		return 0;
	local = quorum_local_csb(&cl->club);
	if (local == NULL)
		return 0;
	/*
	 * The SAME condition the arithmetic itself applies to a CSB before it
	 * counts a vote (quorum_csb_counts above): in use, SELECTED, and its
	 * PARAMS really learned. If this node's own block does not yet satisfy
	 * it, club->quorum was computed over a set this node is not even in --
	 * a figure to report, never one to act on.
	 */
	return quorum_csb_counts(local);
}

void cnxman_quorum_arm_update(struct vms_cluster *cl)
{
	if (cl == NULL)
		return;
	if (!cnxman_quorum_enforce_ready(cl))
		return;
	if (cl->club.quorum_lost)
		return;
	cl->club.quorum_armed = 1u;
}

int cnxman_quorum_hang_active(const struct vms_cluster *cl)
{
	if (cl == NULL)
		return 0;
	if (!cl->club.quorum_armed)
		return 0;          /* never perceived quorum: still forming */
	if (!cl->club.quorum_lost)
		return 0;          /* has it now */
	return cnxman_quorum_enforce_ready(cl);
}

/*
 * THE RUNNING NODE'S RECOMPUTE TRIGGER (rd vms-d0d).
 *
 * cnxman_quorum_recompute() above is the arithmetic; this is the ONE predicate
 * that says a node may run it on itself. It exists because a JOINER has no
 * other way to hold quorum figures at all: p. 7-42 task 2 copies the PROPOSED
 * cells to the effective ones, and the proposed cells are the COORDINATOR's
 * arithmetic -- a node that was admitted never ran one, so on the live 2-node
 * cluster node B counted both members and carried CEVOTES/QUORUM of zero while
 * node A's real VOTES sat learned in B's own CSB table (measured, #1119).
 *
 * The answer is NOT to assert a quorum at commit: it is to do on the joiner
 * exactly what the founder does at genesis -- walk THIS node's own CSB table
 * and apply p. 7-6 to the votes really in it. Every summand is a CSB that
 * received a real PARAMS record (csb->params_valid, enforced by the walk); a
 * peer that has not advertised contributes nothing, so the figure can be
 * INCOMPLETE but is never INVENTED.
 *
 * THE TWO CONDITIONS, and why each one is load-bearing (INV-6):
 *
 *   - cl->state == VMS_CLUSTER_MEMBER. Only phase2_commit_local_membership()
 *     ever sets it, and only from the LOCAL CSB really carrying MEMBER. A node
 *     that is merely connected to a cluster, or joining one, has no membership
 *     to compute a quorum over, and a quorum published before admission is the
 *     local-only fabrication this invariant names outright.
 *   - the LOCAL CSB itself counts. Its VOTES/EXPECTED_VOTES are this node's own
 *     SYSGEN parameters, learned at cnxman_club_init(); if they are not in the
 *     table this node cannot even state its own contribution, and a sum that
 *     silently omits the local system is worse than no sum.
 *
 * Idempotent (the recompute is), so every caller may fire it on every event
 * that could change the answer: design SS3.7's "recomputed on transitions"
 * plus each PARAMS record a member advertises in between.
 *
 * Returns nonzero iff the arithmetic really ran.
 */
int cnxman_quorum_member_recompute(struct vms_cluster *cl)
{
	/* THE TWO CONDITIONS ARE cnxman_quorum_enforce_ready() (FC-P8.1). They
	 * were written twice -- once here, once for the enforcement gate -- and
	 * they are the same two, so there is ONE spelling of them: a node may
	 * run the arithmetic on itself exactly when it is entitled to act on
	 * the result. A second copy is how the two come to disagree. */
	if (!cnxman_quorum_enforce_ready(cl))
		return 0;

	cnxman_quorum_recompute(&cl->club);
	/*
	 * ... and the same arithmetic run LATCHES this node's perception of
	 * quorum (FC-P8.1). It belongs here rather than at the callers because
	 * the latch's whole meaning is "a member really ran the arithmetic and
	 * really had quorum" -- which is precisely the event this function is.
	 * A caller that could recompute WITHOUT arming would be a caller that
	 * silently disarms enforcement.
	 */
	cnxman_quorum_arm_update(cl);
	return 1;
}

uint16_t cnxman_quorum_qdskvotes(const struct vms_club *club)
{
	const struct vms_csb *local;

	if (club == NULL || club->local_csb < 0)
		return 0u;
	if ((uint32_t)club->local_csb >= club->n_csb)
		return 0u;

	local = quorum_local_csb(club);
	if (local == NULL || !local->in_use || !local->params_valid)
		return 0u;
	return local->qdskvotes;
}
