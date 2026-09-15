/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_dlm_quorum.h - THE QUORUM GATE ON THE LOCK ENGINE (FC-P8.1, rd vms-b6d).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.7 "Enforcement (P8)":
 * "with VOTES>0 a quorum loss must SUSPEND the node (VMS: process scheduling
 * blocked, `%CNXMAN, quorum lost, blocking activity`) and resume on regain. In
 * OVMX the executive gates the seam it owns ... an honest subset documented as
 * such." THIS header is that subset's interface, and the subset is named
 * exactly: THE DISTRIBUTED LOCK MANAGER'S GRANT DECISION.
 *
 * ---------------------------------------------------------------------------
 * GROUNDING (clean-room, published description, page cites only, rule 8)
 *
 * *VAXcluster Principles* (Davis 1993) ch. 7 p. 7-4: a system perceives quorum,
 * and cluster activity proceeds, while the votes available to it are at least
 * QUORUM; otherwise it BLOCKS ACTIVITY and waits for quorum to be regained.
 * pp. 7-10/7-11: CEVOTES cannot decrease by itself, so a cluster that loses a
 * voting member does not quietly re-derive a smaller quorum it can still meet
 * -- it hangs. The hang is a STALL, not a failure: the published behaviour of a
 * VMScluster that loses quorum is that work STOPS and RESUMES intact when the
 * votes return; it is not a burst of error returns to every caller.
 * ---------------------------------------------------------------------------
 *
 * WHAT THE EXECUTIVE ACTUALLY DOES WITH IT, AND WHAT IT DOES NOT
 *
 *   IT DOES  stall the GRANT of a lock request while the hang is active: the
 *            request goes on the resource's ordinary waiting queue, the caller
 *            waits exactly as it waits for an incompatible holder, and the
 *            grant happens by itself when quorum returns
 *            (vms_lock_quorum_resume()).
 *   IT DOES  stall an UP-conversion, which is an acquisition by another name.
 *   IT DOES  NOT fail anything. No SS$_ status is invented for a quorum hang,
 *            and LCK$M_NOQUEUE does not turn one into SS$_NOTQUEUED: NOQUEUE
 *            says "do not queue me behind a HOLDER", and during a hang there
 *            need be no holder at all. A process that would have been suspended
 *            on a real VAX does not get an error code here instead.
 *   IT DOES  NOT touch a lock already granted, a $DEQ, a down-conversion, or
 *            any release path. You may always give a lock back during a hang --
 *            that is how the cluster is left in a state the rebuild can use.
 *   IT DOES  NOT suspend the executive. The fork thread, the connection
 *            manager, SCS and the port keep running THROUGHOUT, which is what
 *            lets the node notice quorum coming back at all. Only requesters
 *            wait, and they wait on the mechanism they already waited on.
 *
 * TWO LIMITS OF THIS SUBSET, NAMED RATHER THAN LEFT TO BE DISCOVERED
 *
 *   1. THE GATE IS APPLIED AT THE NODE THAT DECIDES THE GRANT. A $ENQ for a
 *      tree mastered on ANOTHER node is not decided here -- it becomes a proxy
 *      request and the MASTER grants it, applying its own club's gate on its
 *      own votes. So this node does not stall the POST of such a request. The
 *      case where that differs from a real VAX (this node hung, the master not)
 *      needs a partition in which the two disagree about quorum while still
 *      having a working circuit; in the two-node cluster both sides lose quorum
 *      together, and in a 2|1 partition the minority's circuit to the majority
 *      master is the very thing that broke, so its posts fail with a real
 *      path-lost status rather than being granted. Deferring the post until
 *      resume is a queue this rung does not build; it is recorded here as the
 *      shape of the follow-on, not left as a silent divergence.
 *   2. THE REBUILD PATH IS NOT GATED. vms_lock_dlm_assume_mastery() -- a node
 *      taking over mastery of a tree whose master departed (FC-P5.5) -- grants
 *      the promoted request on the spot. Stalling it would risk wedging the
 *      very transition whose completion is how quorum comes back. The rebuild
 *      is the transition's business (Davis pp. 7-40..7-42), not the hang's.
 *
 * WHY AN INJECTED OP AND NOT A FLAG IN THE LOCK ENGINE (INV-6). The engine must
 * not hold a copy of "quorum is lost": a copy is a value plumbed from one layer
 * to another that can be stale, or worse, be set by something that never read a
 * CLUB. The op reads the LIVE CLUB at the moment of the decision, through
 * cnxman_quorum_hang_active(), which is itself a pure read of real CSB-derived
 * state. When there is no cluster the ops are NULL and nothing ever stalls -- a
 * standalone node still locks, the same honest degradation
 * vms_lock_dlm_set_requester_ops(NULL) already gives the wire path.
 *
 * CONCURRENCY. `hang` is called from process context (a $ENQ) and from the fork
 * thread (an inbound cross-node request), and it reads two single-byte CLUB
 * flags without taking the fork mutex -- deliberately: the fork thread itself is
 * one of the callers, so acquiring that mutex here would be a self-deadlock. A
 * byte-sized flag published by the fork thread cannot tear, so the worst a racing
 * reader can see is the value from just before or just after a transition. That
 * is self-correcting by construction: the request that stalls one beat early is
 * released by the resume sweep, and the request that slips through one beat late
 * is a single grant on a node that had quorum microseconds earlier. Nothing is
 * fabricated either way -- both readings came from the CLUB.
 *
 * INCLUDES: none. This header is the seam between two facilities that share no
 * types -- the lock engine (src/kernel-core/vms_lock.c, whose struct vocabulary
 * is each substrate's vms_internal.h) and the connection manager (whose struct
 * vms_cluster is kernel-core). Keeping it include-free is what lets vms_lock.c
 * consume it without ever seeing a CLUB: the engine cannot read cluster state
 * directly even by accident, it can only ask.
 */
#ifndef OVMX_VMS_DLM_QUORUM_H
#define OVMX_VMS_DLM_QUORUM_H

/*
 * The gate, as the connection manager's DLM arm installs it. `hang` returns
 * nonzero while THIS node is in an armed quorum loss; `ctx` is the arm's own
 * struct vms_cluster. A NULL ops block (the default, and what vms_dlm_scs_stop
 * restores) means no cluster and therefore no hang, ever.
 */
struct vms_quorum_ops {
	int (*hang)(void *ctx);
	void *ctx;
};

/*
 * Install or remove the gate. Copied by value under the engine's own lock, so
 * a caller may pass a stack block; NULL removes it. Idempotent.
 */
void vms_lock_set_quorum_ops(const struct vms_quorum_ops *ops);

/*
 * QUORUM IS BACK -- release every request the hang stalled. Walks the resource
 * database, clears the stall marks, and runs the ordinary waiter-granting pass
 * on each resource, so the stalled requests complete through exactly the path a
 * $DEQ would have completed them through (completion AST for an async waiter, a
 * wake for a synchronous one). Safe to call when nothing is stalled.
 *
 * Called from the fork thread by the DLM arm's quorum_changed role op, on the
 * regain edge the connection manager observed -- never on a timer, and never by
 * the lock engine deciding for itself that quorum probably returned.
 */
void vms_lock_quorum_resume(void);

#endif /* OVMX_VMS_DLM_QUORUM_H */
