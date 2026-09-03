/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_phase2.h - the p. 7-42 PHASE 2 COMMIT, shared by the barrier
 * participant (FC-P3.5) and the transition coordinator (FC-P3.12).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (CLUB/CSB), SS3.9
 * (pure TU, injected ops, no globals). Book grounding:
 * docs/design-cluster-book-grounding.md SS2.4 and D13.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS SEPARATELY FROM EITHER FSM
 *
 * *VAXcluster Principles* (Davis 1993) p. 7-42 lists the Phase 2 tasks ONCE,
 * and every system in the transition runs the SAME list -- the coordinator
 * included. It is not a participant obligation and a coordinator obligation; it
 * is one obligation with two callers:
 *
 *   1. copy the CLUB's nodemap into the CSBs of the SELECTED systems;
 *   2. copy the PROPOSED quorum cells to the EFFECTIVE ones;
 *   3. store the member count -- "simply the total number of CSBs whose
 *      SELECTED flags are set";
 *   4. set this system's own CLUSTER flag.
 *
 * >> AND ALL FOUR RUN BEFORE THE SYNCHRONISED REBUILD. << The member count is
 * NOT gated on the DLM rebuild (book-grounding D13). Two implementations of
 * that law would be two chances to drift, and a coordinator whose own count
 * disagreed with the count it just made every member compute is exactly the
 * "committed member count differs from the nodemap" fault the instrumentation
 * below exists to catch. So there is one implementation and both FSMs call it.
 *
 * INV-6. Nothing here asserts a value the executive does not hold: quorum is
 * copied ONLY if FC-P3.7 really filled the proposed cells, membership is
 * rewritten ONLY from nodemap bits that matched a CSB whose CSID this node has
 * LEARNED, and the count is taken from the CSBs rather than from the wire's
 * popcount (the two are then COMPARED, and a disagreement is counted).
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CNXMAN_PHASE2_H
#define OVMX_VMS_CNXMAN_PHASE2_H

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"

/*
 * The honest bound on the barrier evidence. Wire spec SS4(p): "the largest
 * cluster in the entire library is VAX1+VAX2+VAX3+OVMX (bitmap 0x1e) ... Treat
 * 12 as GROUNDED-to-M=4. Nothing above 4 is grounded" -- followed by
 * "instrument for a mismatch rather than trusting it". A transition whose
 * committed member count exceeds this is NOT refused (that would break a
 * cluster over our own ignorance); it is COUNTED, so a real >4-member
 * transition arrives as evidence rather than as a mystery.
 */
#define CNXMAN_PHASE2_M_GROUNDED 4u

/*
 * How many CSV slots the GROUNDED membership bitmap byte can express. Wire spec
 * SS4(p): the field is body[55], "one byte holds only 8 slots while the library
 * already reaches slot 5", and its true width is UNDETERMINED. Bit k is the
 * member holding CSID index k and bit 0 is never set (CSV slot 0 is never used,
 * book p. 7-25), so the expressible slots are 1..7.
 */
#define CNXMAN_PHASE2_BITMAP_SLOTS 8u

/* ==========================================================================
 * What the caller knows about the transition being committed
 *
 * The participant reads these out of the coordinator's open; the coordinator
 * built them itself from its own CSBs. Either way they are REAL: `bitmap` is
 * the byte that went on (or came off) the wire, never a reconstruction.
 * ========================================================================== */
struct cnxman_phase2_in {
	uint8_t bitmap;          /* body[55] as sent/received                  */
	uint8_t bitmap_valid;    /* 0 when the transition carried NO nodemap   */
	uint8_t bitmap_popcount; /* == the post-transition member count (SS4(p))*/
	uint8_t pad;
};

/* ==========================================================================
 * The instrumentation the spec says to watch
 *
 * Deltas, not totals: the caller folds them into its own counters, so a
 * coordinator and a participant each keep their own history and neither hides
 * the other's. Every one of these is an OBSERVATION that would settle an open
 * question (the bitmap's real width, the >4-member barrier), so they are
 * counted and logged, never acted on by guessing.
 * ========================================================================== */
struct cnxman_phase2_stats {
	uint32_t nodemap_unmapped;  /* set bits with no CSB we could match     */
	uint32_t count_mismatch;    /* local count != the nodemap's popcount   */
	uint32_t bitmap_short;      /* more members than the nodemap named     */
	uint32_t m_above_grounded;  /* committed member count > 4 (SS4(p))     */
	uint32_t nodemap_unreadable;/* not one bit matched: membership untouched*/
};

/* ==========================================================================
 * Run the four p. 7-42 tasks. Returns the committed member count.
 *
 * `st` is zeroed on entry and filled with this call's deltas. `ops` supplies
 * the injected clock (club->last_transition_ms) and the %CNXMAN console lines;
 * both members may be absent. A NULL `cl` or `in` returns 0 and touches
 * nothing.
 * ========================================================================== */
uint32_t cnxman_phase2_commit(struct vms_cluster *cl,
			      const struct cnxman_phase2_in *in,
			      struct cnxman_phase2_stats *st,
			      const struct cnxman_ops *ops);

/*
 * popcount of the grounded bitmap byte. Exposed because BOTH sides need it and
 * both must get the same answer: the participant counts the bits it received,
 * the coordinator counts the bits it is about to assert.
 */
uint32_t cnxman_phase2_popcount8(uint8_t v);

#endif /* OVMX_VMS_CNXMAN_PHASE2_H */
