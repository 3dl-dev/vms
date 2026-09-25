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
 *   op 0x05 MEMBERSHIP RECORDS and the op 0x06 MEMBERSHIP burst ARE originated
 *       now (rd vms-fc7): op 0x05 carries the grounded {SCSSYSTEMID, boot time,
 *       assigned CSID, CSV index} pairing -- the full member set to the joiner,
 *       the delta to each present member -- and op 0x06 carries this
 *       coordinator's own CSID at the grounded form-A offset. What is still NOT
 *       originated is the op-0x05 record's body[42:132], which is uninterpreted
 *       stale buffer in the reference and is zeroed and counted here rather
 *       than reproduced (Rule 8).
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
	CNXMAN_COORD_REF_BUSY       = 5, /* we are already coordinating one   */
	CNXMAN_COORD_REF_NO_QUORUM  = 6, /* GENESIS only: this node's own
					  * VOTES do not satisfy quorum, so it
					  * may not FORM a cluster (p. 7-6)   */
	/*
	 * GENESIS only, and the three halves of "is there already a cluster
	 * here?" -- see SS8b's ELECTION note. Each one is a read of real CSB
	 * state and each one means JOIN, not FORM.
	 */
	CNXMAN_COORD_REF_PEER_CLUSTER  = 7, /* a system present already holds a
					     * cluster identity: a CSID, or this
					     * CLUB's own MEMBER/SELECTED flag  */
	CNXMAN_COORD_REF_PEER_UNASKED  = 8, /* a system is present and this node
					     * has not yet completed a round of
					     * asking it to admit this node     */
	CNXMAN_COORD_REF_OUTRANKED     = 9, /* another founding candidate is
					     * present whose SCSSYSTEMID ranks
					     * ahead of ours: IT forms, we join */
	/*
	 * ADMISSION only (rd vms-1ac): this node was ASKED to admit a system,
	 * but it is not the member VMS's own observable names as the
	 * coordinator -- another MEMBER outranks it. Spec §4(p)'s "a
	 * NON-COORDINATOR peer SILENTLY DISCARDS op 0x02", carried out.
	 */
	CNXMAN_COORD_REF_NOT_SELECTED  = 10,
	/*
	 * ADMISSION only (rd vms-1ac): this node IS the one that would drive
	 * it, but a participant has not proved it runs this implementation and
	 * the class-0x02 transition-open this node can build is not grounded
	 * byte-for-byte for a foreign connection manager (see
	 * coord_open_is_grounded_for()). Refusing costs an admission round;
	 * emitting it cost a real OpenVMS VAX V7.3 a CNXMGRERR bugcheck.
	 */
	CNXMAN_COORD_REF_OPEN_UNGROUNDED = 11
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
	uint32_t memberships_sent;     /* op 0x06 MEMBERSHIP records ORIGINATED
					* -- one per admission, never a burst
					* (E78's crash vector)                */
	uint32_t membership_fields_omitted; /* grounded-offset fields left zero
					     * in a membership record we DID
					     * send: the countdown, the
					     * incarnation, the sub-record body
					     * (design note sec 6)             */
	uint32_t membrecs_sent;        /* op-0x05 MEMBERSHIP RECORDS originated
					* -- the full set to the joiner, the
					* delta to each present member        */
	uint32_t membrec_omitted;      /* a member this node holds no complete
					* identity for: NO record sent for it */
	uint32_t membrec_boot_omitted; /* records sent with body[28:36] zero --
					* no incarnation held for that member */
	uint32_t membrec_fields_omitted; /* body[42:132], the reference's stale
					  * buffer, zeroed per record          */
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
	/*
	 * THE TWO ADMISSION GATES (rd vms-1ac). `not_selected` counts the
	 * op-0x02s this node silently discarded because another MEMBER
	 * outranks it -- moving is NORMAL and is what a real non-coordinator
	 * does. `open_ungrounded` counts the admissions this node WOULD have
	 * driven and refused because it cannot build a byte-grounded
	 * class-0x02 open for a participant that does not run this
	 * implementation -- moving is a NAMED GAP, never a resting state.
	 */
	uint32_t not_selected;
	uint32_t open_ungrounded;
	/* 1 once coord_advance_epoch() has run for the transition in progress
	 * (rd vms-1ac). Per-transition, cleared by coord_open_transition(). */
	uint8_t  epoch_advanced;
	uint32_t ignored_events;       /* no table cell: ignored and COUNTED   */

	/* ---- GENESIS (SS9), both outcomes counted from a real call ---- */
	uint32_t genesis_opens;        /* founding transitions really opened   */
	uint32_t genesis_refused_noquorum; /* asked to found without quorum by
					    * its own votes -- REFUSED, and the
					    * count is the anti-LARP tripwire  */
	uint32_t genesis_refused_peer;     /* asked to found beside a system that
					    * already holds a cluster identity --
					    * REFUSED: this node JOINS, it does
					    * not compete                      */
	uint32_t genesis_refused_unasked;  /* asked to found while a system this
					    * node has not finished asking for
					    * admission is present -- REFUSED  */
	uint32_t genesis_refused_outranked;/* asked to found while a founding
					    * candidate that ranks ahead of this
					    * node is present -- REFUSED: THAT
					    * node forms and this one joins it */
	/*
	 * The SCSSYSTEMID this node last deferred to, read out of that peer's
	 * own CSB at the moment of the refusal. Diagnostics only, and an
	 * OMISSION when there has been none: `deferred_to_valid` 0 means this
	 * node has never stood down for anybody, never "system 0".
	 */
	vms_scs_sysid_t deferred_to_sysid;
	uint8_t  deferred_to_valid;
	/*
	 * THE FOUNDING REFUSAL THIS NODE HAS ALREADY SAID OUT LOUD (rd vms-151).
	 * The founding gate is asked once a second, by the same reconnect beat
	 * that does the discovery -- so a node that is waiting for the system
	 * ahead of it to form refuses once a second, for as long as it takes.
	 * The refusal is right; saying it every second is not: OPA0: is an
	 * operator's console, and VMS does not repeat itself there while a
	 * situation simply persists.
	 *
	 * So the line is EDGE-triggered on the reason: said when the reason
	 * CHANGES, silent while it holds, said again if it comes back. The
	 * COUNTERS above are untouched by this -- they count every refusal,
	 * because a count is state and not speech. CNXMAN_COORD_REF_NONE = this
	 * node has said nothing yet, which is where cnxman_coord_init()'s zeroed
	 * context starts and what a successful founding restores.
	 */
	uint8_t  genesis_said;     /* enum cnxman_coord_refusal, last ANNOUNCED */
	uint8_t  pad_form[2];

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
 * 8b. GENESIS -- forming a cluster from nothing (docs/design-cluster-genesis.md)
 *
 * The published formation algorithm: a node waiting to form or join applies
 * p. 7-6 to the PROPOSED SET -- itself plus every system it can currently see
 * -- and, when their COMBINED votes meet quorum, forms the cluster as its
 * founding member and coordinator, taking cluster generation 1; every later
 * node joins THROUGH it on the ordinary op-0x02 path. Without this the
 * executive cannot form a cluster at all -- a node needs a CSID to coordinate,
 * and a CSID is only ever learned from a coordinator's op-0x06, so two fresh
 * nodes deadlock.
 *
 * THE VOTES ARE COMBINED, NOT THIS NODE'S ALONE (rd vms-6d3d, MEASURED on two
 * real OpenVMS VAX V7.3 systems, capture vms-6d3d-coldform-ev2-20260924):
 * VOTES=1 / EXPECTED_VOTES=2 -- the documented two-node VMScluster -- has NO
 * node whose own vote meets quorum 2, and the oracle's first node sat in
 * `%SYSINIT, waiting to form or join a VMScluster system` for eighteen minutes
 * printing not one %CNXMAN line. The instant its peer appeared it logged
 * `established connection to node VAX2` and, 3.2 s later, `proposed formation
 * of a VAXcluster`. Demanding quorum from the FOUNDER's own votes -- what this
 * file did before -- is strictly stronger than VMS and made that configuration
 * unformable by any pair of OVMX nodes.
 *
 * THIS IS NOT A SECOND KIND OF CLUSTER. What it forms is an ordinary
 * VMScluster whose first member happens to be this executive; the systems that
 * join it afterwards -- another OVMX node or a real VAX -- arrive on the SAME
 * op-0x02 / relay / commit / Phase 1 / GO / barrier path, are admitted by the
 * SAME coordinator code below, and are committed by the SAME
 * cnxman_phase2_commit(). There is no founding-only branch anywhere past this
 * function: it opens a transition and then IS the coordinator, exactly as
 * being asked by a joiner makes it one.
 *
 * WHAT IT IS NOT. It is not "stamp yourself a member". It opens a REAL
 * transition (class ADD, subject none, nodemap = this node alone) and runs the
 * SAME Phase 1 / GO / Phase 2 / twelve-step chain every admission runs; the
 * membership it ends with is set by cnxman_phase2_commit() out of this node's
 * own CSB, exactly as a joiner's is. With no participants there is nothing to
 * propose to, so not one frame goes on the wire -- the identical degenerate
 * path a two-node cluster takes when it loses its peer.
 *
 * THE GATE (INV-6, and interop safety). Refused -- minting NOTHING and leaving
 * this node's state untouched -- unless ALL of: no CSID already learned; no
 * transition already in progress; a real local CSB; NOBODY ELSE MAY BE FORMING
 * OR HOLDING A CLUSTER HERE (the three clauses of the ELECTION note below); the
 * proposed set's combined votes satisfy quorum
 * (cnxman_quorum_form_votes_suffice(), p. 7-6); and the CSID it would mint is
 * nameable in the grounded nodemap byte. The votes gate is the load-bearing one
 * for INV-6 -- a VOTES=0 node NEVER founds, however this is called, and every
 * vote in the sum was read from a CSB that really received that system's PARAMS
 * record over a circuit that is really OPEN -- and the peer gates are the
 * load-bearing ones for interop. Every refusal is counted (`genesis_refused_noquorum`,
 * `genesis_refused_peer`, `genesis_refused_unasked`,
 * `genesis_refused_outranked`).
 *
 * ---------------------------------------------------------------------------
 * THE ELECTION -- WHO FORMS, WHEN TWO FRESH SYSTEMS CAN SEE EACH OTHER
 *
 * THE DEADLOCK THIS RESOLVES, measured. Two fresh OVMX nodes booted onto one
 * LAN open their SCS virtual circuits to each other within seconds and then sit
 * there: each holds a CSB for the other, so the old "NO OTHER SYSTEM PRESENT AT
 * ALL" clause refused BOTH of them, and neither could admit the other because
 * admission needs a coordinator and a coordinator needs a CSID. Twenty-five
 * minutes, blank CSID on both consoles, no `%CNXMAN ... is now a VAXcluster
 * member` on either (rd vms-151). "There is a system present" is the right
 * question to ask; "therefore I join it" was the wrong conclusion to draw from
 * it, because a system that is not in a cluster cannot admit anybody.
 *
 * So the clause is split into the three DIFFERENT facts it was conflating, and
 * only the first two forbid forming:
 *
 *  (1) THAT SYSTEM IS ALREADY IN A CLUSTER -> JOIN IT, never form. The honest
 *      executive reads: its CSB carries a CSID (a value only a cluster ever
 *      assigns -- this node learns one, it never guesses one), or this CLUB
 *      has it MEMBER/SELECTED. This is the clause that keeps a booting OVMX
 *      node from forming a singleton beside a live VAXcluster, and it is
 *      unchanged in strength (`genesis_refused_peer`).
 *
 *  (2) THAT SYSTEM HAS NOT BEEN ASKED YET -> ASK BEFORE FORMING. A CSID is not
 *      the only way a peer can be in a cluster -- a member this node has not
 *      yet exchanged membership records with carries none HERE. What settles it
 *      is the question the join FSM already asks: a member takes a membership
 *      request and coordinates an admission within milliseconds (spec SS4(o)),
 *      and a system that is not in a cluster cannot. So this node may form only
 *      after it has completed at least one FULL round in which every system it
 *      could see was asked to admit it and none did -- the join FSM's own
 *      `attempts_exhausted`, passed in as `struct cnxman_form_evidence` because
 *      it is the JOIN's fact and this file will not guess at it. Nobody visible
 *      at all needs no round: there was nobody to ask (`genesis_refused_unasked`).
 *
 *  (3) THAT SYSTEM IS ANOTHER FOUNDING CANDIDATE -> EXACTLY ONE OF US FORMS.
 *      Both nodes reach clause (2) together, so something must break the
 *      symmetry or they form two clusters and partition. Book p. 7-32's
 *      published mechanism is the COORDINATOR LOCK -- a would-be coordinator
 *      asks every selected system for permission, one already granted to
 *      another refuses, and a collision backs off a random short interval.
 *      OVMX CANNOT ASK: no capture in this project's library contains a
 *      coordinator-lock request or grant and no opcode is grounded for one, and
 *      inventing a frame for it is the failure class that bugchecked two real
 *      VAXes. What this file implements instead is a TOTAL ORDER over a value
 *      every candidate already advertises and every candidate therefore
 *      computes the same answer from -- the SCSSYSTEMID in its CSB -- and the
 *      LOWEST one forms. It is labelled for what it is: an OVMX DESIGN VALUE
 *      standing in for a mechanism OVMX has no grounding to speak, chosen
 *      because it is total (it cannot elect two), symmetric (both nodes decide
 *      identically from the same wire-learned numbers), and needs no frame.
 *      The DIRECTION is arbitrary and is not claimed to be VMS's.
 *
 *      WHO IS A CANDIDATE is itself read, not assumed, and it is read with the
 *      SAME predicate this node's own founding gate is about to be asked:
 *      cnxman_quorum_could_found() over the SAME proposed set, with that
 *      peer's own advertised VOTES and EXPECTED_VOTES in place of this node's.
 *      One formula, two subjects -- a second copy is how a node comes to defer
 *      forever to a system that could never have formed anything (rd vms-6d3d).
 *      FORM requires VOTES > 0 (pp. 7-28, 7-33), so a peer advertising zero
 *      votes falls out of that predicate rather than being special-cased.
 *
 *      Old CEVOTES is the one term a peer does not advertise, so 0 stands in
 *      for it -- the value that makes the peer MOST likely to qualify, i.e.
 *      most likely to be treated as a rival. Two peers are rivals with no
 *      arithmetic at all: one whose PARAMS have NOT arrived (an un-advertised
 *      VOTES is unknown, never a zero) and one this node cannot currently
 *      reach, so it is not in the proposed set and there is nothing to judge
 *      it against. Standing down costs a beat, forming beside somebody costs a
 *      partition (`genesis_refused_outranked`).
 *
 * WHAT THE LOSER DOES is nothing new: it keeps the join drive it was already
 * running, and the moment the winner is a member its op-0x02 is taken and it is
 * admitted on the ORDINARY path. There is no second code path for "the node
 * that lost the election".
 * ---------------------------------------------------------------------------
 *
 * WHAT THIS FUNCTION DOES NOT DECIDE: *when* to ask. A booting node must not
 * conclude "there is nobody here" faster than it can hear somebody -- the
 * discovery window is the caller's (vms_cnxman.c's cnxman_try_genesis(), which
 * waits out RECNXINTERVAL of real discovery beats first).
 *
 * Returns 0 only when phase2 really committed a membership -- read back from
 * the executive's own state, never assumed. Nonzero otherwise, with
 * `last_refusal` naming the reason and one %CNXMAN line saying it out loud.
 * ========================================================================== */

/* p. 7-25: the CSID sequence starts at 1, and a cluster formed from nothing has
 * by definition never used this slot before, so a founder is always generation
 * 1. A RE-formation after total cluster loss would have to advance it, which
 * needs a generation that survives the loss -- this executive persists none,
 * and inventing one is what INV-6 forbids (design note, "Generation source"). */
#define CNXMAN_COORD_GENESIS_GEN 1u

/*
 * THE FOUNDER'S CSV SLOT, and why it is not `SCSSYSTEMID & 0x3ff`.
 *
 * rd vms-3a7c settled the assignment rule against the lab oracle and both
 * repository captures: a coordinator hands out the ROUND-ROBIN, monotonically
 * advancing CSV slot (p. 7-25), never a function of the SCSSYSTEMID -- SCSSYSTEMID
 * 1986 was assigned slot 3 and 1026 was assigned slot 3, neither of which its own
 * system id can produce. coord_assign_slot() has implemented that for every
 * admission since. The FOUNDING path had not been moved over, and still built
 * `(1 << 16) | (SCSSYSTEMID & 0x3ff)` -- a reading the capture that suggested it
 * cannot distinguish, because the two real founders it shows (sysid 1025 -> CSID
 * 0x00010001, sysid 1027 -> 0x00010003) have system ids whose bottom ten bits
 * happen to EQUAL their slots.
 *
 * It is not a harmless difference. A cluster formed from nothing has used no
 * slot, so the founder takes the first one the same round-robin walk would hand
 * out -- slot 1, since slot 0 is never used -- and any node whose SCSSYSTEMID's
 * bottom ten bits fall outside the grounded nodemap byte could not found AT ALL
 * under the old rule: SCSSYSTEMID 1987 asks for slot 963 and is refused
 * NO_SLOT. The founder now goes through coord_next_slot(), the one walk every
 * other assignment already uses, so a founder and an admission cannot assign
 * slots differently.
 */

/*
 * THE JOIN'S FACT, carried to the FORM decision (SS8b clause (2)).
 *
 * `admission_rounds` is the join FSM's own `attempts_exhausted`: how many
 * COMPLETE rounds this node has finished in which every system it could see was
 * asked to admit it and none took the request. It is read out of the real join
 * FSM by the caller and passed here because it is that FSM's state, not this
 * one's -- there is no second counter and nothing is inferred from a clock.
 *
 * A NULL evidence pointer means "no round has been completed", which is the
 * strictest reading and the one a caller that does not run a join FSM must get.
 */
struct cnxman_form_evidence {
	uint32_t admission_rounds;
};

int cnxman_coord_found(struct cnxman_coord *c,
		       const struct cnxman_form_evidence *ev);

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
