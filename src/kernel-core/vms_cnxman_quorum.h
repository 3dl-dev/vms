/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_quorum.h - the Quorum Algorithm: CEVOTES/QUORUM computation,
 * tracking only (FC-P3.7).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.7 ("the CLUB tracks
 * CEVOTES/QUORUM from every member's advertised VOTES and the local
 * EXPECTED_VOTES from day one so $GETSYI reports truth"), SS3.4 (the CLUB
 * fields this item populates -- cevotes, quorum, expected_votes, quorum_lost:
 * "FC-P3.7 computes these; FC-P3.6 does not write them, so nothing here is a
 * fabricated zero standing in for arithmetic that has not run").
 *
 * TRACKING (P3 scope) PLUS THE ENFORCEMENT PREDICATES (FC-P8.1, rd vms-b6d,
 * SS "ENFORCEMENT" below). This module computes the numbers on every membership
 * transition and answers whether they may be ACTED on; it still suspends
 * nothing itself -- it has no process, no lock and no queue to suspend. The
 * ACTOR is the lock manager, through the injected gate of vms_dlm_quorum.h.
 * The Quorum Disk Watcher's liveness fold-in remains P8.4, unground and
 * deliberately absent.
 *
 * ---------------------------------------------------------------------------
 * GROUNDING (clean-room, published description, page cites only, rule 8)
 *
 * *VAXcluster Principles* (Davis 1993) ch. 7 pp. 7-5/7-6: the Connection
 * Manager's three-step Quorum Algorithm --
 *
 *   1. Select the proposed set of members: normally every member it can see,
 *      but a departed system is excluded from the set (p. 7-6).
 *   2. New CEVOTES = max{ EXPECTED_VOTES from the selected set ;
 *                         SUM of VOTES from the selected set ;
 *                         Old CEVOTES }
 *      where Old CEVOTES defaults to 0 until a cluster actually forms and is
 *      otherwise the New CEVOTES value from the most recently COMPLETED
 *      cluster state transition (p. 7-6) -- and pp. 7-10/7-11 draw the
 *      consequence explicitly: the value "cannot decrease by itself".
 *   3. QUORUM = (New CEVOTES + 2) / 2 (p. 7-6).
 *
 * A system perceives quorum, and cluster activity proceeds, while the votes
 * currently AVAILABLE to it (its reachable members' VOTES) are >= QUORUM;
 * otherwise it blocks activity and waits for quorum to be regained (p. 7-4).
 * The five-node worked example (pp. 7-6/7-7) is what this module's host test
 * (test_cnxman_quorum.c) reproduces exactly, cross-checked against the public
 * *VMScluster Systems for OpenVMS* sec. 2.3.5/2.3.6's identical algorithm and
 * 3-node worked example. The HOST-ONLY transcript is cited by page only, per
 * clean-room rule 8 -- no text is reproduced here or in the test.
 *
 * The quorum disk (pp. 7-15/7-16) contributes its QDSKVOTES to the algorithm
 * only while it is ACTIVE and this system considers it TRUSTWORTHY -- a live
 * Quorum Disk Watcher fact this module does not have yet (P8 owns the
 * executive block-seam watcher, design SS3.7). cnxman_quorum_qdskvotes()
 * below TRACKS the configured value (this node's own QDSKVOTES SYSGEN
 * parameter, already learned into the local CSB by FC-P3.6's
 * cnxman_club_init()) for readback; it is deliberately NOT folded into
 * cevotes/quorum/quorum_lost, because assuming the disk is up would be
 * exactly the kind of unearned executive-state claim INV-6 forbids.
 * ---------------------------------------------------------------------------
 *
 * INV-6. Every VOTES/EXPECTED_VOTES this module sums is read from a real CSB
 * that LEARNED it (csb->params_valid) -- a CSB that never received a PARAMS
 * record contributes NOTHING, never a fabricated zero standing in for an
 * unknown vote. The "selected set of proposed members" is the CSB table's own
 * SELECTED flag (p. 7-49), never a caller-supplied list: nothing here is
 * counted until the executive itself has recorded that fact.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * This TU is PURE: no seam call, no allocation, no clock -- so it runs
 * identically in both kmods, the host unit tests and the N-node simulator.
 */
#ifndef OVMX_VMS_CNXMAN_QUORUM_H
#define OVMX_VMS_CNXMAN_QUORUM_H

#include "vms_cluster.h"

/*
 * Recompute club->cevotes / club->quorum / club->expected_votes /
 * club->quorum_lost from the CSB table's current SELECTED, params_valid
 * members (p. 7-6's "selected set of proposed members") and their PRESENT
 * reachability (the local CSB always counts; a remote one only in state
 * OPEN -- p. 7-30: membership is HELD, not counted present, across a
 * reconnect window).
 *
 * Idempotent, and safe to call every time membership changes (design SS3.7):
 * CEVOTES only ever grows, because club->cevotes itself is read back in as
 * "Old CEVOTES" (p. 7-6) -- there is no separate history table to keep in
 * step. A NULL club changes nothing.
 */
void cnxman_quorum_recompute(struct vms_club *club);

/*
 * RECOMPUTE IF -- AND ONLY IF -- THIS NODE IS REALLY A MEMBER (rd vms-d0d).
 *
 * The trigger a running node uses on itself, for the two events that can change
 * the answer: a transition COMMIT (design SS3.7 "recomputed on transitions",
 * p. 7-42) and a member's PARAMS record arriving in between (SS3.7 "the CLUB
 * tracks CEVOTES/QUORUM from every member's advertised VOTES ... from day one
 * so $GETSYI reports truth").
 *
 * It exists because the ADMITTED node has no other source for these figures:
 * p. 7-42 task 2's proposed->effective copy carries the COORDINATOR's
 * arithmetic, and a joiner never ran one -- so without this it counts its
 * fellow members and reports CEVOTES/QUORUM 0 while the peer's real advertised
 * VOTES sit in its own CSB table. Here it does what the founding node does at
 * genesis: applies p. 7-6 to the votes the executive really holds.
 *
 * INV-6: runs only when cl->state is VMS_CLUSTER_MEMBER (which only a real
 * Phase 2 commit of a real MEMBER flag ever sets) AND this node's own local CSB
 * is in the selected, params-learned set. Otherwise it changes NOTHING and
 * returns 0 -- an un-computed quorum stays honestly absent rather than becoming
 * a zero, or a local-only number, that the rest of the cluster never agreed to.
 *
 * Returns nonzero iff the recompute really ran. A NULL cluster changes nothing.
 */
int cnxman_quorum_member_recompute(struct vms_cluster *cl);

/*
 * The published formula, split into its two published steps so that every
 * caller in the executive computes quorum with ONE implementation
 * (single-ledger; a second copy is how a founding node and a running cluster
 * come to disagree about what quorum is).
 *
 *   cnxman_quorum_cevotes()    p. 7-6 step 2: max{EXPECTED_VOTES; SUM VOTES;
 *                              Old CEVOTES}, clamped to the CLUB's 16-bit
 *                              field.
 *   cnxman_quorum_of_cevotes() p. 7-6 step 3: (CEVOTES + 2) / 2.
 *
 * Both are pure arithmetic over values the CALLER has already read from real
 * state; neither reads or writes a CLUB.
 */
uint16_t cnxman_quorum_cevotes(uint16_t old_cevotes, uint32_t expected_votes,
			       uint32_t sum_votes);
uint16_t cnxman_quorum_of_cevotes(uint16_t cevotes);

/* ===========================================================================
 * FORMING FROM NOTHING -- p. 7-6 applied to a COLD FORMATION (rd vms-6d3d)
 *
 * THE PROPOSED SET IS NOT "THIS NODE ALONE". Step 1 selects "the proposed set
 * of members: normally every member it can see". For a node that is already in
 * a cluster that set is the SELECTED CSBs; for a node that is waiting to form
 * one there is no membership yet, so the set is THIS SYSTEM PLUS EVERY SYSTEM
 * IT CAN CURRENTLY SEE -- and the votes that are weighed against quorum are
 * THEIR COMBINED VOTES, not this node's own.
 *
 * MEASURED ON THE ORACLE (tests/lab/captures/vms-6d3d-coldform-ev2-20260924/),
 * two real OpenVMS VAX V7.3 systems, VOTES=1 / EXPECTED_VOTES=2 on each:
 *
 *   - VAX1 alone on the segment for 18 minutes printed
 *     `%SYSINIT, waiting to form or join a VMScluster system` and NOT ONE
 *     %CNXMAN line. Its own vote cannot meet quorum 2, so it did not form.
 *   - the instant VAX2 appeared, VAX1 logged `discovered node VAX2`,
 *     `established connection to node VAX2`, and 3.2 s later
 *     `proposed formation of a VAXcluster` -- with the two nodes' COMBINED
 *     two votes meeting the same quorum 2.
 *
 * The predecessor of this file demanded quorum from the founder's OWN votes,
 * which is strictly stronger than what VMS does and made the documented
 * two-node VMScluster (VOTES=1, EXPECTED_VOTES=2 on both) unformable by any
 * pair of OVMX nodes.
 *
 * INV-6. Every vote summed below is read from a CSB that really received that
 * system's PARAMS record (csb->params_valid) over a circuit that is really
 * OPEN right now. A system whose PARAMS have not arrived contributes NOTHING
 * -- an un-advertised VOTES is unknown, never a fabricated zero -- and a system
 * this node cannot currently reach contributes nothing either, which is the
 * same p. 7-4/7-5 "available" rule cnxman_quorum_recompute() applies.
 * =========================================================================== */

/*
 * The proposed set of a COLD formation, as three summary numbers. Held in a
 * struct rather than recomputed per question so that the founding gate and the
 * election below cannot be asking about two different sets.
 */
struct cnxman_form_set {
	uint32_t sum_votes;     /* SUM of VOTES over the set (p. 7-6 step 2) */
	uint32_t max_expected;  /* the largest EXPECTED_VOTES advertised in it */
	uint32_t n_systems;     /* how many systems are in it, this node included */
	uint8_t  local_counted; /* nonzero iff THIS node's own CSB is in the set */
};

/*
 * Walk the CLUB and fill *out. A CSB is in the set iff it is in use, it really
 * learned its PARAMS, and it is reachable NOW: the LOCAL CSB always is (it is
 * this system, not a connection that can be down), a remote one only in state
 * OPEN. Never reads a caller's opinion of who is present. A NULL club yields
 * the empty set rather than a guess.
 */
void cnxman_quorum_form_set(const struct vms_club *club,
			    struct cnxman_form_set *out);

/* Is THIS CSB one of the systems cnxman_quorum_form_set() counted? The one
 * spelling of the membership rule above, exported because the election has to
 * ask it about a single peer ("can I even judge what that system could form?")
 * without walking the CLUB a second time. */
int cnxman_quorum_form_set_contains(const struct vms_csb *csb);

/*
 * COULD THE SYSTEM DESCRIBED BY (votes, expected_votes, old_cevotes) FORM THE
 * CLUSTER THIS PROPOSED SET DESCRIBES?
 *
 * p. 7-6, all three steps, over `set`: New CEVOTES = max{that system's own
 * EXPECTED_VOTES; the largest EXPECTED_VOTES in the set; SUM VOTES; Old
 * CEVOTES}, QUORUM = (New CEVOTES + 2) / 2, and the set's combined votes must
 * meet it. VOTES == 0 is refused FIRST and on its own terms: FORM requires a
 * voting system (pp. 7-28, 7-33), and making that a property of this function
 * rather than of the numbers it happens to be handed is what keeps "a VOTES=0
 * node never founds" true however it is called.
 *
 * ONE function, asked about TWO different systems: this node (the founding
 * gate) and a peer (the election's "is that system a rival?"). A second copy
 * is how a node comes to defer forever to a system that could never have
 * formed anything.
 *
 * *out_quorum receives the figure the answer was judged against (0 on refusal
 * before any arithmetic ran).
 */
int cnxman_quorum_could_found(uint16_t old_cevotes, uint16_t votes,
			      uint16_t expected_votes,
			      const struct cnxman_form_set *set,
			      uint16_t *out_quorum);

/*
 * THE FOUNDING PREDICATE: may THIS node form the cluster `set` describes?
 *
 * cnxman_quorum_could_found() asked about this node -- its SYSGEN VOTES and
 * EXPECTED_VOTES and the CLUB's own Old CEVOTES -- with one extra condition
 * that only the local system has: its own CSB must be IN the set. If this node
 * cannot even state its own contribution (no local CSB, or one whose PARAMS
 * were never loaded) then the sum it would found on silently omits the founder,
 * which is worse than no sum at all.
 *
 * `set` may be NULL, in which case it is computed from the CLUB here.
 *
 * INV-6, and the reason this predicate is a function rather than an `if` at
 * the one call site: the CSID a founding node mints is the one value in the
 * whole stack that cannot be learned from anybody, so the condition that
 * permits minting it is load-bearing and is proved on its own
 * (tests/cluster/host/test_cnxman_genesis.c and its negctl). The predecessor
 * of this stack defaulted the local CSID to 1 unconditionally and became a
 * phantom cluster of one; the difference is exactly this function.
 */
int cnxman_quorum_form_votes_suffice(const struct vms_cluster *cl,
				     const struct cnxman_form_set *set,
				     uint16_t *out_quorum);

/* ===========================================================================
 * ENFORCEMENT -- may the executive ACT on the arithmetic? (FC-P8.1, rd vms-b6d)
 *
 * p. 7-4: a system perceives quorum, and cluster activity proceeds, while the
 * votes available to it are >= QUORUM; otherwise it BLOCKS ACTIVITY and waits
 * for quorum to be regained. That "blocks activity" is VMS's quorum hang: a
 * STALL, not an error. A lock request made during it is neither granted nor
 * refused -- it waits, exactly as a request waits for an incompatible holder --
 * and it completes on its own the moment quorum returns. Nothing is denied with
 * a status code and nothing in flight is invented (INV-6).
 *
 * WHY THE RAW FLAG IS NOT THE CONDITION. club->quorum_lost is the arithmetic's
 * answer AT THIS INSTANT, over the CSBs whose PARAMS this node has learned SO
 * FAR. During a join that set is legitimately empty or partial, and p. 7-6's
 * formula over an empty set yields QUORUM = (0+2)/2 = 1 with 0 votes present --
 * quorum_lost = 1, which is the HONEST arithmetic answer to "do I have quorum
 * right now?" and NOT a report that a cluster lost quorum. Enforcing on it
 * would freeze every node on every join, forever, before it could ever learn a
 * peer's votes. So enforcement is gated on the node having genuinely PERCEIVED
 * quorum first (club->quorum_armed, latched by cnxman_quorum_arm_update()), and
 * on it still being a committed member whose own CSB counts.
 *
 * All three are PURE reads of a real CLUB -- no clock, no seam, no cache. The
 * lock manager reaches cnxman_quorum_hang_active() through the injected
 * `struct vms_quorum_ops` of vms_dlm_quorum.h so that the value it acts on is
 * read from THIS state at decision time, never copied into the lock engine
 * where it could go stale (INV-6).
 * =========================================================================== */

/*
 * Is this node in a position to act on its own quorum arithmetic at all?
 * Nonzero iff it is a COMMITTED member (cl->state == VMS_CLUSTER_MEMBER) whose
 * own CSB satisfies the very condition the arithmetic applies before counting a
 * vote (in use, SELECTED, PARAMS learned). A joining node, a node whose local
 * CSB the CLUB has not selected, and a node with no cluster at all all answer 0
 * -- they have numbers to REPORT, none to enforce.
 */
int cnxman_quorum_enforce_ready(const struct vms_cluster *cl);

/*
 * Latch "this node has perceived quorum" (club->quorum_armed) when, and only
 * when, it is enforce-ready AND its current arithmetic says it HAS quorum.
 * Idempotent, monotone within one cluster life, and safe to call after every
 * recompute -- which is exactly how it is used (cnxman_quorum_apply()).
 */
void cnxman_quorum_arm_update(struct vms_cluster *cl);

/*
 * THE QUORUM HANG, as one predicate: nonzero iff this node is enforce-ready,
 * has latched a real perception of quorum, and has LOST it. This is the value
 * the lock manager stalls new grants on; it clears by itself the moment the
 * next recompute finds the votes back, which is what makes the hang resume
 * rather than need an unwind.
 */
int cnxman_quorum_hang_active(const struct vms_cluster *cl);

/*
 * This node's own TRACKED QDSKVOTES (its SYSGEN quorum-disk vote count,
 * learned into the local CSB at cnxman_club_init() time -- always valid once
 * the CLUB exists). NOT folded into cevotes/quorum/quorum_lost -- see the
 * grounding paragraph above. Returns 0 for a NULL club or before the local
 * CSB's params were learned (never a guessed nonzero).
 */
uint16_t cnxman_quorum_qdskvotes(const struct vms_club *club);

#endif /* OVMX_VMS_CNXMAN_QUORUM_H */
