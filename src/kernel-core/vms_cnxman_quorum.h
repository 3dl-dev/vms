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
 * TRACKING ONLY (P3 scope, this item's own outcome line). This module
 * computes the numbers on every membership transition; it never blocks or
 * allows one and never suspends process activity. Enforcement -- VOTES>0
 * gating the scheduler/ACP issue points on quorum loss, and the Quorum Disk
 * Watcher's liveness fold-in -- is P8 (design SS3.7 "Enforcement (P8)").
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
 * This node's own TRACKED QDSKVOTES (its SYSGEN quorum-disk vote count,
 * learned into the local CSB at cnxman_club_init() time -- always valid once
 * the CLUB exists). NOT folded into cevotes/quorum/quorum_lost -- see the
 * grounding paragraph above. Returns 0 for a NULL club or before the local
 * CSB's params were learned (never a guessed nonzero).
 */
uint16_t cnxman_quorum_qdskvotes(const struct vms_club *club);

#endif /* OVMX_VMS_CNXMAN_QUORUM_H */
