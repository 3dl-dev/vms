/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_coord_fsm.h - the cluster state transition, COORDINATOR side
 * (FC-P3.12).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (CLUB/CSB), SS3.7
 * (the coordinator role), SS5.5 (selection -- INFERRED), SS3.9 (pure FSM,
 * injected ops, injected clock, no raw wire offset outside a codec TU). Wire
 * spec: docs/cluster-protocol-spec.md SS4(o)/(p)/(q)/(r)/(O.31). Book
 * grounding: docs/design-cluster-book-grounding.md SS2.4, SS3.4, D7, D13, D17.
 * The participant half of the same transition is vms_cnxman_barrier_fsm.h; the
 * Phase 2 commit both halves run is vms_cnxman_phase2.h.
 *
 * ===========================================================================
 * THE OBLIGATION, IN ONE LAW
 *
 * "The frame count scales exactly: #0x0b = #0x0c = 12 x (M-1), in 30 of 30 ...
 * It is ONE cluster-wide lock-stepped barrier, not M-1 independent runs:
 * 0x0c#N never precedes the last 0x0b#N -- 0 violations out of 12 steps in
 * every transition ... The coordinator holds 0x0c#N until the SLOWEST member
 * reports." (spec SS4(p).)
 *
 * A participant's cost is flat: 12 steps, whatever the cluster size. The
 * 12 x (M-1) scaling is ENTIRELY this file's. And the failure mode is not "we
 * fail to coordinate" -- a coordinator that releases step N early breaks the
 * lock-step the whole rebuild depends on, and one that never releases it times
 * the transition out and DROPS HEALTHY MEMBERS.
 *
 * So M is never a parameter, never a constant and never a count of frames
 * received. It is derived, at the moment the transition opens, from the CLUB's
 * real CSBs -- the same SELECTED flags p. 7-49 says the member count is "simply"
 * the total of -- and then FROZEN for the life of the transition, because a
 * census against a set that moves underneath it is not a census.
 * ===========================================================================
 *
 * ===========================================================================
 * THE SELECTION PREDICATE -- INFERRED, and here is exactly what from
 *
 * Design SS5.5 records the predicate as INFERRED and offers "highest node
 * number", which the wire evidence itself distrusts (spec SS4(p): the only
 * predicate surviving both specimens is "highest DECnet node number", and it is
 * "confounded with highest SCSSYSTEMID and last to have joined").
 * docs/design-cluster-book-grounding.md D7 supersedes it from the published
 * description, and THAT is what this file implements:
 *
 *   FOR A JOIN, THE JOINER PICKS. Book pp. 7-37/7-38: the joining system
 *   chooses whom to ask -- highest VAXcluster protocol level, then highest ECO
 *   level, then the CSB nearest the end of the CLUB's queue -- and asks only
 *   once it has connectivity with as many members as those CSBs advertise. The
 *   wire says the same thing from the other side (spec SS4(p)): "a
 *   NON-COORDINATOR peer SILENTLY DISCARDS op 0x02", and the joiner "sends its
 *   op 0x02 to EXACTLY ONE peer, which relays the new node to the rest".
 *
 *   So a node does not decide it is the coordinator. IT IS TOLD. Receiving the
 *   op 0x02 IS the selection, and there is nothing further to compute -- which
 *   is why this file contains no ordering rule, no SCSSYSTEMID comparison and
 *   no node-number arithmetic. Design SS3.7's "OVMX never CLAIMS the role
 *   unprompted" is satisfied structurally: there is no code path that elects
 *   this node.
 *
 *   FOR A DEPARTURE, THE FIRST DETECTOR. Book p. 7-2: the coordinator is
 *   "effectively random ... very often the first VMS system to detect an
 *   event", and p. 7-30: a connection manager starts a transition after a
 *   connectivity loss "only if no other Connection Manager has already
 *   instituted a cluster state transition". FC-P3.6's CSB ladder already
 *   produces exactly that decision as CNXMAN_CSB_ACT_PROPOSE_TRANSITION; this
 *   file is where it is carried out.
 *
 *   AND THEN THE COORDINATOR LOCK. Book p. 7-32: the would-be coordinator must
 *   obtain permission from every selected system, "a system already granted to
 *   another refuses", and a collision backs off a random short interval.
 *
 * >> WHAT OVMX CAN AND CANNOT DO ABOUT THAT LOCK, HONESTLY. << No capture in
 * this project's library contains a coordinator-lock request or grant, and no
 * opcode is grounded for one, so OVMX CANNOT ASK. Inventing a frame for it is
 * the failure class that bugchecked two real VAXes. What OVMX implements is the
 * lock's OBSERVABLE CONSEQUENCE, in two halves, both of which are grounded:
 *
 *   local half  -- refuse to drive while another connection manager's
 *                  transition is open. That state is real: the CLUB's
 *                  transition_active with we_coordinate clear means a peer
 *                  opened one and this node acknowledged it. This is p. 7-30's
 *                  rule verbatim, and it is the same test FC-P3.6's ladder
 *                  already applies before it proposes.
 *   remote half -- the Phase 1 proposal is the grant. p. 7-41: each receiver
 *                  validates and either acknowledges or requests abandonment,
 *                  and the coordinator abandons on ANY rejection or
 *                  connectivity loss. A member already committed to another
 *                  coordinator will not acknowledge ours, so our Phase 1 never
 *                  completes and no GO is ever sent. The distributed mutual
 *                  exclusion is therefore enforced by a mechanism that IS
 *                  grounded, instead of by one this project would have to
 *                  invent.
 *
 * NO PROTOCOL TIMEOUT IS INVENTED ANYWHERE IN THIS FILE. Abandonment is
 * event-driven only (a collision, a lost participant, an abort). Spec SS4(p) is
 * explicit that a slow step is not a failure; and p. 7-41's real bound on a
 * silent participant is FC-P3.6's RECNXINTERVAL loop, which reports the loss as
 * an event. A waiting coordinator is COUNTED, never timed out.
 * ===========================================================================
 *
 * ===========================================================================
 * WHAT THIS FILE ORIGINATES, AND WHAT IT HONESTLY DOES NOT
 *
 * Originated, every field through vms_cluster_codec_cm.h, every value read from
 * real CLUB/CSB/connection-manager state:
 *
 *   op 0x12 RELAY   to each other member   (spec SS4(O.31), book p. 7-39)
 *   op 0x03 COMMIT  to the subject         (spec SS4(o) step 6)
 *   op 0x09 / 0x08  Phase 1 open           (spec SS4(p)/(r); ADD carries the
 *                                           nodemap this file BUILDS from CSBs)
 *   op 0x0a GO      Phase 2 commit         (spec SS4(p)/(r))
 *   0x81/0x0b       one per step reported  (spec SS4(p) barrier table)
 *   op 0x0c         12 releases per member (the 12 x (M-1) law)
 *
 * NOT originated, and each omission is COUNTED so it shows up in the
 * diagnostics rather than being discovered on a real cluster:
 *
 *   op 0x05 lock/resource-rebuild burst and op 0x06 MEMBERSHIP burst -- the
 *       membership record's {SCSSYSTEMID, incarnation, CSID} triple (book
 *       p. 7-39) has NO isolated offset in any capture (spec SS4(j) "RE gaps
 *       left in SS4j"). A zero-filled membership burst would assert an empty
 *       cluster to every member, so it is omitted. CONSEQUENCE, stated plainly:
 *       a node this coordinator admits completes the transition and appears in
 *       our CSBs, but is NOT TOLD the CSID we assigned it -- op 0x06 is how a
 *       joiner learns its own (FC-P3.3). Closing that needs the op-0x06 record
 *       layout from a capture; it is a LAB item, not a guess.
 *   the ORIGINATING form of the cat-0x02 op-0x0d rebuild record -- its L1
 *       region body[16:34] is only ever observed inbound. This file therefore
 *       does not PUSH rebuild records; it does the other half of the
 *       coordinator's rebuild obligation, which is to HOLD the barrier while
 *       any are outstanding (spec SS4(p): the coordinator "gates the next step
 *       on them being answered"; five unanswered ones froze a real barrier at
 *       step 5). FC-P5.5 owns building and sending them and reports its
 *       outstanding count through cnxman_coord_rebuild_ops below.
 *   the Phase 1 proposal's quorum / votes / foundation-time / founder /
 *       rebuild-type cells (book p. 7-40) -- not isolated to an offset. Their
 *       bytes go out zero and coord->open_cells_omitted counts every open.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CNXMAN_COORD_FSM_H
#define OVMX_VMS_CNXMAN_COORD_FSM_H

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_phase2.h"
#include "vms_dlm_scs.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * 1. The published constants
 * ========================================================================== */

/* TWELVE, the same constant the participant carries (spec SS4(p): 41 captures,
 * 30 completed barriers, indices 1..12, zero variance at M=2/3/4). The
 * coordinator sends this many releases TO EACH participant. */
#define CNXMAN_COORD_BARRIER_STEPS 12u

/*
 * The collision back-off window, book p. 7-32's "random short interval".
 *
 * A pure FSM has no entropy source and design SS3.9 forbids reading one from the
 * substrate, so the interval is DERIVED: base + (injected clock XOR this node's
 * own SCSSYSTEMID) modulo the span. That is deterministic under a test clock
 * (so the back-off is exercisable at rung 1) and decorrelated between nodes (so
 * two colliding coordinators do not retry in lock step), which is the only
 * property the book's "random" is there to buy. Named as a substitute, not
 * passed off as randomness.
 */
#define CNXMAN_COORD_BACKOFF_BASE_MS 100u
#define CNXMAN_COORD_BACKOFF_SPAN_MS 400u

/* How often the coordinator NOTICES that a step or a Phase 1 is still
 * outstanding. NOT a protocol timeout: nothing expires when it fires. It is
 * also the beat on which a barrier held by an outstanding rebuild record
 * re-tries its release. */
#define CNXMAN_COORD_WATCH_MS 1000u

/* ==========================================================================
 * 2. The states
 *
 * Each is a real position in the published sequence: the wire's op 0x02 ->
 * op 0x12 -> op 0x03 prologue (spec SS4(O.31), decoded from a real-VAX
 * readmission) followed by the book's Phase 1 -> Phase 2 -> synchronised
 * rebuild (pp. 7-40..7-42).
 * ========================================================================== */
enum cnxman_coord_state {
	CNXMAN_COORD_IDLE      = 0, /* not coordinating anything            */
	CNXMAN_COORD_DEFER     = 1, /* asked, but the transition is not ours
				     * to open yet: backing off (p. 7-32)   */
	CNXMAN_COORD_RELAY     = 2, /* op 0x12 out; awaiting the members'
				     * connectivity confirmations          */
	CNXMAN_COORD_COMMIT    = 3, /* op 0x03 out; awaiting the subject    */
	CNXMAN_COORD_OPEN      = 4, /* PHASE 1: opens out; awaiting all acks*/
	CNXMAN_COORD_BARRIER   = 5, /* PHASE 2 committed; 12 x (M-1) running*/
	CNXMAN_COORD_COMPLETE  = 6, /* release #12 sent to every member     */
	CNXMAN_COORD_ABANDONED = 7, /* p. 7-41: a rejection, a lost member,
				     * or another coordinator won the race  */
	CNXMAN_COORD_STATE__COUNT
};

/* ==========================================================================
 * 3. What a received frame did (identical vocabulary to the participant's, so
 * the glue routes with one rule: offer to the coordinator, and on NOT_MINE
 * offer to the barrier)
 * ========================================================================== */
enum cnxman_coord_rx {
	CNXMAN_COORD_RX_CONSUMED = 0,
	CNXMAN_COORD_RX_NOT_MINE = 1,
	CNXMAN_COORD_RX_BAD      = 2
};

/* ==========================================================================
 * 4. The selection predicate, exposed
 *
 * Public because it is the item's INFERRED judgement and must be testable on
 * its own, without driving twenty frames to observe it indirectly.
 * ========================================================================== */
enum cnxman_coord_trigger {
	/* A joiner sent US its op 0x02. The joiner's choice IS the selection
	 * (book pp. 7-37/7-38; spec SS4(p)). */
	CNXMAN_COORD_TRIG_ASKED    = 0,
	/* This node detected a member's departure (book p. 7-2 "the first VMS
	 * system to detect an event"), which reaches us as FC-P3.6's
	 * CNXMAN_CSB_ACT_PROPOSE_TRANSITION. */
	CNXMAN_COORD_TRIG_DETECTED = 1
};

enum cnxman_coord_verdict {
	CNXMAN_COORD_DRIVE   = 0, /* we are the coordinator for this one     */
	CNXMAN_COORD_BACKOFF = 1, /* another CM holds it: back off (p. 7-32) */
	CNXMAN_COORD_REFUSE  = 2  /* we cannot coordinate this at all, and
				   * saying so is better than half-doing it  */
};

/* Why a REFUSE was returned -- recorded, logged and counted, never swallowed. */
enum cnxman_coord_refusal {
	CNXMAN_COORD_REF_NONE       = 0,
	CNXMAN_COORD_REF_NOT_MEMBER = 1, /* this node has no learned CSID yet */
	CNXMAN_COORD_REF_NO_SUBJECT = 2, /* no CSB for the node in question   */
	CNXMAN_COORD_REF_NO_SLOT    = 3, /* no CSV slot the grounded nodemap
					  * byte can express (SS4(p): "do not
					  * assume 8 slots")                 */
	CNXMAN_COORD_REF_NO_NODEMAP = 4, /* a member's slot is outside that
					  * byte: an open would LOSE it      */
	CNXMAN_COORD_REF_BUSY       = 5  /* we are already coordinating one   */
};

/* ==========================================================================
 * 5. Body[0:8] -- via the CSB, not a link
 *
 * Every ORIGINATED CM body needs body[0:8] (send/ack message numbers, the
 * transaction id, the correlation token whose derivation is UNKNOWN, spec
 * SS4(j)) stamped from the destination's real dialogue state. This FSM
 * already resolves every destination to a CLUB-slot CSB (`coord_csb_at()`),
 * so unlike the participant side it needs no separate lookup structure: the
 * codec builds body[8:132], then `cnxman_envelope_stamp(csb, body,
 * is_response)` (vms_cnxman_csb.h) -- the ONE function permitted to write
 * body[0:8] -- fills the rest (design sec 3.2.4 ruling E1, FC-P3.15). A CSB
 * this FSM cannot resolve is a refusal to transmit, never a zero-filled
 * body (INV-6): the prior `cnxman_coord_link_ops`/`next_out` indirection is
 * gone, since the CSB itself already IS that lookup.
 */

/*
 * The lock manager's half of the rebuild gate (spec SS4(p): the coordinator
 * interleaves cat-0x02 op-0x0d records with the barrier "and gates the next
 * step on them being answered"). FC-P5.5 owns pushing the records; this FSM
 * owns holding the barrier while any are outstanding.
 *
 * Absent (NULL) is the P3 configuration and is HONEST, not a stub: with no DLM
 * attached this node pushes no rebuild records, so none can be outstanding, so
 * the gate is open -- which is exactly the state a lock-less member is in.
 */
struct cnxman_coord_rebuild_ops {
	/* How many rebuild records this node has pushed and not yet had
	 * answered. Read from the DLM's real in-flight table, never estimated. */
	uint32_t (*outstanding)(void *ctx);
	void *ctx;
};

/* ==========================================================================
 * 6. The context
 *
 * No globals (design SS3.9 rule 3). The census is indexed by the CLUB's own CSB
 * slot, so it CANNOT disagree with the membership it is a census of.
 * ========================================================================== */

/* Per-CSB census bits. */
#define CNXMAN_COORD_P_PARTICIPANT 0x01u /* frozen into this transition       */
#define CNXMAN_COORD_P_RELAY_ACK   0x02u /* answered our op 0x12              */
#define CNXMAN_COORD_P_PHASE1_ACK  0x04u /* answered our Phase 1 open         */
#define CNXMAN_COORD_P_SUBJECT     0x08u /* the node being added or removed   */

struct cnxman_coord {
	struct vms_cluster                    *cl;
	const struct cnxman_ops               *ops;
	const struct cnxman_coord_rebuild_ops *rebuild;
	const struct dlm_scs_role_ops         *dlm;

	/* ---- the transition we are driving ---- */
	uint8_t  state;            /* enum cnxman_coord_state                 */
	uint8_t  tr_class;         /* VMS_CM_CLASS_ADD / _REMOVE              */
	uint8_t  step;             /* 1..12: the step whose release is pending*/
	uint8_t  phase2_committed;
	uint32_t epoch;            /* THIS node's own CLUB epoch, advanced    */
	uint8_t  bitmap;           /* body[55], BUILT from our real CSBs      */
	uint8_t  bitmap_valid;     /* only a class-0x02 ADD carries one       */
	uint8_t  bitmap_popcount;
	uint8_t  last_refusal;     /* enum cnxman_coord_refusal               */
	int32_t  subject_csb;      /* CLUB slot of the joiner/departing node  */
	uint32_t n_participants;   /* M-1, FROZEN at the open                 */

	/* ---- the census, one cell per CLUB slot ---- */
	uint8_t part_flags[VMS_CLUB_MAX_CSB];
	uint8_t part_step[VMS_CLUB_MAX_CSB];  /* highest step each reported   */

	/* ---- the CSV knowledge a coordinator needs to assign a CSID ----
	 * Book p. 7-25: slots are handed out round-robin, slot 0 is never used,
	 * and a vacated slot is NOT reused by the next joiner (the wire agrees
	 * -- spec SS4(p) watches one node rejoin three times taking slots 3, 4
	 * and 5). So the next slot is one past the highest this node has ever
	 * seen, seeded from the CSIDs the cluster really assigned. */
	uint32_t max_slot_seen;
	uint32_t csids_assigned;

	/* ---- the back-off (p. 7-32) ---- */
	uint32_t backoff_due_ms;
	uint8_t  pending_trigger;   /* enum cnxman_coord_trigger              */
	uint8_t  pad[3];
	int32_t  pending_subject_csb;

	/* ---- everything below is counted from a real dispatch ---- */
	uint32_t transitions_driven;
	uint32_t transitions_completed;
	uint32_t transitions_abandoned;
	uint32_t deferrals;          /* p. 7-32 collisions we backed off from  */
	uint32_t refusals;
	uint32_t relays_sent;
	uint32_t relay_acks;
	uint32_t commits_sent;
	uint32_t commit_acks;
	uint32_t opens_sent;         /* Phase 1 proposals ORIGINATED           */
	uint32_t open_acks;          /* Phase 1 acknowledgements (p. 7-41)     */
	uint32_t gos_sent;           /* Phase 2 commits ORIGINATED             */
	uint32_t steps_received;     /* op 0x0b from members: must be 12x(M-1) */
	uint32_t step_acks_sent;     /* 0x81/0x0b: one per step received       */
	uint32_t releases_sent;      /* op 0x0c: THE 12 x (M-1) COUNT          */
	uint32_t send_failures;      /* no CSB for the destination; nothing sent*/

	/* ---- instrumentation: the honest omissions and the anomalies ---- */
	uint32_t open_cells_omitted;   /* Phase 1 cells with no known offset   */
	uint32_t relay_subject_omitted;/* relays sent with no subject field    */
	uint32_t membership_burst_omitted; /* op 0x06 we could not build       */
	uint32_t step_out_of_order;    /* a member reported a step we are not on*/
	uint32_t step_duplicates;      /* a retransmitted step: acked, not counted*/
	uint32_t epoch_mismatch;       /* a report for a different transition  */
	uint32_t rebuild_holds;        /* releases held for outstanding records*/
	uint32_t slow_steps;           /* watchdog fired; NEVER an abandonment */
	uint32_t slow_phase1;          /* a proposal still unacknowledged: the
					* visible symptom of a member that has
					* granted the coordinator lock to
					* somebody else (book p. 7-32)        */
	uint32_t collisions;           /* another CM opened one at the same time*/
	uint32_t m_above_grounded;     /* committed count > 4 (spec SS4(p))    */
	uint32_t count_mismatch;
	uint32_t nodemap_unmapped;
	uint32_t bitmap_short;
	uint32_t unknown_peer;         /* a frame from no CSB we could resolve */
	uint32_t ignored_events;       /* no table cell: ignored and COUNTED   */

	/* The one scratch buffer every built BODY goes through (design sec
	 * 3.2.4: this FSM emits bodies, never a frame) -- in the context, not
	 * on the stack: this code runs on a VAX kernel stack. */
	uint8_t scratch[VMS_CM_BODY_LEN];
};

/* ==========================================================================
 * 7. Lifecycle
 * ========================================================================== */

/* Bind the FSM to a node. Sends nothing, arms nothing, elects nobody. */
void cnxman_coord_init(struct cnxman_coord *c, struct vms_cluster *cl,
		       const struct cnxman_ops *ops);

/* Install (or, with NULL, detach) the lock manager's transition callbacks and
 * its rebuild-gate reporter. */
void cnxman_coord_set_dlm(struct cnxman_coord *c,
			  const struct dlm_scs_role_ops *dlm);
void cnxman_coord_set_rebuild(struct cnxman_coord *c,
			      const struct cnxman_coord_rebuild_ops *rb);

/* ==========================================================================
 * 8. Events
 * ========================================================================== */

/*
 * One inbound `VMS$VAXcluster` frame.
 *
 * `from_csb` is the CLUB slot the connection this frame arrived on belongs to,
 * or -1 when the connection manager could not resolve one. A CSB INDEX rather
 * than a CSID on purpose: a JOINER HAS NO CSID YET -- assigning it one is this
 * file's job -- so a CSID-keyed interface would force the caller to invent an
 * identity for the one message that exists because the sender has none. Every
 * identity this FSM uses is then read out of that real CSB.
 *
 * Returns CNXMAN_COORD_RX_NOT_MINE for every frame another FSM owns, INCLUDING
 * a peer's transition open/GO/abort while this node is not coordinating -- so
 * the glue routes it straight on to the participant barrier. When a peer's open
 * arrives while we ARE coordinating and we have not yet passed the point of no
 * return, this FSM abandons ours and STILL returns NOT_MINE, so the very frame
 * that told us we lost the race is handed to the participant half. That is the
 * hand-off, and it costs no extra round trip.
 */
enum cnxman_coord_rx cnxman_coord_rx_body(struct cnxman_coord *c,
					  const uint8_t *body, uint32_t len,
					  int32_t from_csb);

/*
 * FC-P3.6 hand-off: the CSB ten-state ladder returned
 * CNXMAN_CSB_ACT_PROPOSE_TRANSITION for a member whose reconnect window
 * expired (book p. 7-30). Runs the selection predicate and, on DRIVE, opens a
 * class-0x03 REMOVE. Returns the verdict so the caller can log it; a DEFER
 * arms the back-off and is retried from cnxman_coord_timer().
 */
enum cnxman_coord_verdict cnxman_coord_propose_remove(struct cnxman_coord *c,
						      int32_t subject_csb);

/*
 * The selection predicate on its own (SS4), so the item's INFERRED judgement is
 * directly testable. It answers ONE question -- "is this node the coordinator
 * for this event?" -- sends nothing and starts nothing; the only state it
 * writes is `last_refusal`. Whether the transition it would open can be
 * EXPRESSED on the grounded wire (a CSV slot inside the nodemap byte, a
 * nodemap that names every member) is a second question, answered when the
 * transition actually opens, because it needs the CSID this node is about to
 * assign.
 */
enum cnxman_coord_verdict cnxman_coord_select(struct cnxman_coord *c,
					      enum cnxman_coord_trigger trig,
					      int32_t subject_csb);

/*
 * The coordinator's beat (CNXMAN_TIMER_COORD / CNXMAN_TIMER_BARRIER). Three
 * jobs, none of which is a timeout: retry a deferred transition once the
 * back-off has elapsed; re-attempt a release that is being held for an
 * outstanding rebuild record; and COUNT a step or a Phase 1 that is still
 * waiting, so a stall is visible in the diagnostics instead of silent.
 */
void cnxman_coord_timer(struct cnxman_coord *c);

/*
 * Connectivity to a participant was lost mid-transition. Book p. 7-41: the
 * coordinator abandons on any rejection or connectivity loss -- but ONLY
 * before Phase 2. After the GO the transition "cannot be abandoned" (p. 7-42),
 * so a loss there is counted and the departing member is dropped from the
 * census rather than un-doing a committed commit.
 */
void cnxman_coord_participant_lost(struct cnxman_coord *c, int32_t csb_index);

/* Abandon the transition in progress (a Phase 1 rejection, or the glue
 * shutting the stack down). Idempotent; the DLM is told completed = 0. */
void cnxman_coord_abandon(struct cnxman_coord *c, const char *why);

/* ==========================================================================
 * 9. Readback
 * ========================================================================== */

/* Fill `out` with the transition this node is COORDINATING (we_coordinate = 1,
 * and subject_csid filled when it is known -- which, unlike the participant
 * side, it always is here). Nonzero when there is none. */
int cnxman_coord_transition(const struct cnxman_coord *c,
			    struct cnxman_transition *out);

/* The 12 x (M-1) law, as this transition actually ran it: how many releases
 * this node owes for the census it froze at the open. Zero outside a
 * transition. */
uint32_t cnxman_coord_expected_releases(const struct cnxman_coord *c);

const char *cnxman_coord_state_name(enum cnxman_coord_state s);
const char *cnxman_coord_verdict_name(enum cnxman_coord_verdict v);

#endif /* OVMX_VMS_CNXMAN_COORD_FSM_H */
