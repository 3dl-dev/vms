/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_barrier_fsm.h - the cluster state-transition BARRIER, PARTICIPANT
 * side (FC-P3.5).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.4 (CLUB/CSB), SS3.5,
 * SS3.9 (pure FSM, injected ops, injected clock, no raw wire offsets outside the
 * codec). Wire spec: docs/cluster-protocol-spec.md SS4(o)/(p)/(q)/(r).
 * Book grounding: docs/design-cluster-book-grounding.md SS2.4, SS3.4, D13.
 * The CLUB/CSB this drives is vms_cnxman_csb.h; the frames are built and read
 * ONLY through vms_cluster_codec_cm.h.
 *
 * ===========================================================================
 * WHY THIS FILE EXISTS, IN ONE SENTENCE FROM THE SPEC
 *
 * "A joiner that ignores the barrier does not merely fail to join -- it BREAKS
 * THE CLUSTER. The coordinator's barrier stays permanently one member short, so
 * the transition times out, %CNXMAN, aborting VAXcluster state transition is
 * logged, and the healthy members are dropped. Observed twice." (spec SS4(p).)
 *
 * A participant's obligation is therefore small, fixed and absolute:
 *
 *   1. ANSWER the transition open (cat 0x01 op 0x09 / 0x08 / 0x0d) with the
 *      grounded 0x81 echo -- this is the book's Phase 1 acknowledgement.
 *   2. NEVER answer the GO (op 0x0a) or a RELEASE (op 0x0c). No 0x8a and no
 *      0x8c exists in any capture; both carry txn=0, so there is nothing to
 *      correlate and answering would invent a message VMS never sends.
 *   3. Send exactly TWELVE op-0x0b requests, one per release, and never a
 *      thirteenth.
 *   4. Answer the cat-0x02 op-0x0d rebuild records the coordinator INTERLEAVES
 *      with the barrier. Five unanswered ones froze a real barrier at step 5.
 *   5. Do NOT time a step out. The coordinator holds release N until the
 *      SLOWEST member reports, so per-step latency grows with cluster size.
 * ===========================================================================
 *
 * ===========================================================================
 * THE PHASE MODEL -- AND THE CORRECTION THAT MATTERS MOST HERE
 *
 * *VAXcluster Principles* (Davis 1993) pp. 7-40..7-42 describes an ADD as two
 * phases with a barrier-synchronised rebuild at the end:
 *
 *   PHASE 1 (p. 7-40/7-41) -- the coordinator PROPOSES: nodemap, proposed
 *      quorum / computed expected votes / quorum-disk votes, foundation
 *      timestamp, founder's SCSSYSTEMID, and the rebuild TYPE. Each receiver
 *      stores those in its CLUB's PROPOSED data cells, "ignored outside the
 *      context of a cluster state transition", runs its consistency checks and
 *      "acknowledges to VAX_A that it has received and processed the
 *      information". The transition can still be ABANDONED here.
 *
 *   PHASE 2 (p. 7-42) -- "At this point, the state transition is COMMITTED; it
 *      passes beyond the 'point of no return' and cannot be abandoned." Each
 *      system then, at its own pace and NOT in lock step: copies the nodemap
 *      into the CSBs; copies proposed -> effective quorum cells; stores the
 *      total votes; stores "the total number of members ... the total number of
 *      CSBs whose SELECTED flags are set"; sets the MEMBER flag; sets its own
 *      CLUB's CLUSTER flag; and fills in the Lock Directory Weight Vector.
 *
 *   ONLY THEN the rebuild: "rebuilding the lock management database requires a
 *      high degree of synchronization. Thus, each system waits until all the
 *      other systems are ready to do the rebuild. Then they all go through the
 *      rebuild procedure in absolute unison, carefully synchronized by VAX_A."
 *      The transition is over when the coordinator releases its coordinator
 *      lock (p. 7-42).
 *
 * >> THE COUNT COMMITS IN PHASE 2, BEFORE THE REBUILD (p. 7-42). <<
 * The member count is NOT gated on the DLM rebuild completing. It is a Phase 2
 * bookkeeping task that runs before the lock-step rebuild begins. This file
 * implements exactly that: cnxman_barrier_phase2_committed() is true from the
 * GO onward -- before step 1's op-0x0b has been answered, and before any
 * rebuild record has been echoed. A design that waited for the rebuild would
 * leave a node reading `member` with a stale count, which the book calls
 * anomalous (book-grounding D13, p. 7-42 vs p. 7-37).
 *
 * MAPPING THE PHASES ONTO THE WIRE -- **INFERRED**, and marked so at the table.
 * The transcript names the phases; the captures name the opcodes; nothing
 * published joins them (book-grounding SS6: FC-P3.5's residual gate is exactly
 * "mapping of op-09/0a/0b/0c to 'wait until all ready -> rebuild in unison'").
 * The mapping this file implements, and the reason each leg is the only one
 * that fits BOTH sources:
 *
 *   op 0x09/0x08/0x0d open  = PHASE 1 proposal   -- it is the only per-member
 *       message that is ANSWERED (p. 7-41's acknowledgement) and the only one
 *       carrying the nodemap (p. 7-40's Phase 1 content, spec SS4(p)'s bitmap).
 *   op 0x0a GO              = PHASE 2 commit     -- it is NEVER answered, which
 *       is exactly a point-of-no-return notification: p. 7-42 has no reply and
 *       nothing left to abandon.
 *   12 x (0x0b -> 0x81/0x0b -> 0x0c) = the SYNCHRONISED REBUILD -- "each system
 *       waits until all the other systems are ready", which is what a release
 *       held until the slowest member reports IS (spec SS4(p) measured the
 *       coordinator holding step 5 for 89 ms).
 *   release #12             = the coordinator releasing its coordinator lock.
 *
 * If a capture ever contradicts this mapping, the fix is the table cell and its
 * INFERRED comment -- not a redesign.
 * ===========================================================================
 *
 * WHAT THIS FILE DOES NOT DO. It does not coordinate (FC-P3.12 owns the
 * `12 x (M-1)` obligation), does not join (FC-P3.3), does not compute quorum
 * (FC-P3.7 owns the vote arithmetic and fills the CLUB's proposed cells; this
 * file copies proposed -> effective at Phase 2 ONLY when they were really
 * filled, and never invents a zero quorum), and does not rebuild the lock
 * database (FC-P5.5). In P3 an inbound rebuild record is offered to the DLM
 * through the vms_cnxman.h callback and, when no DLM is attached, answered with
 * the codec's GROUNDED verbatim echo -- which asserts nothing (spec SS4(p):
 * "The echo returns the coordinator's own record with a result code and claims
 * nothing -- which is exactly why a lock-less joiner answers all 216").
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CNXMAN_BARRIER_FSM_H
#define OVMX_VMS_CNXMAN_BARRIER_FSM_H

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_phase2.h"
#include "vms_dlm_scs.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * 1. The published constants
 * ========================================================================== */

/*
 * TWELVE. Spec SS4(p), a census of 41 captures / 40 transitions / 30 completed
 * barriers: "Every completed barrier tops out at exactly step 12 -- indices
 * 1...12, no gaps, no 13. M=2 (16 barriers), M=3 (11), M=4 (3): ZERO variance",
 * and "no peer ever announces the step total" (an exhaustive scan of all 54
 * op-0x09 opens and all 65 class-0x02 GOs finds no field equal to 12). So 12 is
 * a constant a participant must carry itself -- and one whose grounding stops
 * at four members (below).
 */
#define CNXMAN_BARRIER_STEPS 12u

/*
 * The honest bound on that evidence. Spec SS4(p): "the largest cluster in the
 * entire library is VAX1+VAX2+VAX3+OVMX (bitmap 0x1e), and OVMX is one of the
 * four ... Treat 12 as GROUNDED-to-M=4. Nothing above 4 is grounded" --
 * followed by "instrument for a mismatch rather than trusting it". A
 * transition whose committed member count exceeds this is not refused (that
 * would break a cluster over our own ignorance); it is COUNTED in
 * `m_above_grounded` and logged, so a real >4-member transition arrives as
 * evidence rather than as a mystery.
 */
#define CNXMAN_BARRIER_M_GROUNDED CNXMAN_PHASE2_M_GROUNDED

/*
 * The watchdog cadence. NOT a protocol timeout and NOT a published value: it is
 * how often this node NOTICES that a step is still outstanding, so a stalled
 * barrier shows up in the diagnostics instead of as a silent hang. Nothing
 * expires when it fires -- spec SS4(p): "Do not time out on a step merely
 * because it is slow."
 */
#define CNXMAN_BARRIER_WATCH_MS 1000u

/* ==========================================================================
 * 2. The states
 *
 * Five, each of which is a real position in the published phase model. There is
 * no state for "waiting for the DLM": the count commits before the rebuild
 * (p. 7-42), so the rebuild cannot be a gate on membership.
 * ========================================================================== */
enum cnxman_barrier_state {
	CNXMAN_BARRIER_IDLE      = 0, /* no transition in progress            */
	CNXMAN_BARRIER_OPEN      = 1, /* Phase 1: an open arrived + was ack'd */
	CNXMAN_BARRIER_STEP      = 2, /* Phase 2 committed; a step is in flight*/
	CNXMAN_BARRIER_COMPLETE  = 3, /* release #12 seen; the transition is over*/
	CNXMAN_BARRIER_ABANDONED = 4, /* the coordinator aborted it (op 0x04) */
	CNXMAN_BARRIER_STATE__COUNT
};

/* ==========================================================================
 * 3. What a received frame did
 * ========================================================================== */
enum cnxman_barrier_rx {
	CNXMAN_BARRIER_RX_CONSUMED = 0, /* it was a barrier frame; handled     */
	CNXMAN_BARRIER_RX_NOT_MINE = 1, /* another CM FSM owns it: route on    */
	CNXMAN_BARRIER_RX_BAD      = 2  /* it did not parse as a CM frame      */
};

/* ==========================================================================
 * 4. Body[0:8] -- via the CSB, not a link
 *
 * A frame this FSM ORIGINATES (the op-0x0b step) or ANSWERS (the 0x81 echo, the
 * op-0x0d rebuild echo) needs body[0:8] (send/ack message numbers, the
 * transaction id, and the correlation token whose derivation is UNKNOWN, spec
 * SS4(j)) stamped from the destination's real dialogue state.
 *
 * THE FSM DOES NOT OWN THOSE AND MUST NOT INVENT THEM. A prior implementation
 * used the barrier step ordinal as the token; it collided with its own step-1
 * value, the coordinator dropped the frame, its recv_ack froze, and the barrier
 * stalled and then REGRESSED. Design SS3.2.4 ruling E1 (FC-P3.15) closed that:
 * this FSM finds the destination's CSB with `cnxman_club_find_csid()`
 * (vms_cnxman_csb.h) and calls `cnxman_envelope_stamp(csb, body, is_response)`
 * -- the ONE function permitted to write body[0:8] -- after the codec has
 * built body[8:132]. No CSB for that destination means this FSM ORIGINATES
 * NOTHING, which is the honest outcome (INV-6), not a zero-filled frame. There
 * is no more `cnxman_barrier_link_ops`/`next_out`: abs [0,72) is the port's
 * (vms_pe.h) and SCS's (vms_scs.h) alone, filled by the `cnxman_ops.send`/
 * `respond` glue this FSM's `body` argument reaches through.
 * ========================================================================== */

/* ==========================================================================
 * 5. The context
 *
 * No globals (design SS3.9 rule 3). Every counter below is incremented from a
 * real dispatch of a real frame; none is a placeholder and none is displayed
 * as cluster state.
 * ========================================================================== */
struct cnxman_barrier {
	struct vms_cluster                   *cl;
	const struct cnxman_ops              *ops;

	/* The lock manager's wire arm (vms_cnxman.h SS5). NULL is a REAL VMS
	 * configuration -- a node with no distributed locking still joins --
	 * and is the P3 default: the rebuild records are then answered with
	 * the codec's grounded verbatim echo, which asserts no lock state. */
	const struct dlm_scs_role_ops        *dlm;

	/* ---- the transition in progress ---- */
	uint8_t  state;             /* enum cnxman_barrier_state              */
	uint8_t  tr_class;          /* body[17]: VMS_CM_CLASS_ADD/REMOVE/DEPART*/
	uint8_t  step;              /* 1..12: the step whose op-0b is in flight*/
	uint8_t  phase2_committed;  /* the p. 7-42 tasks have run              */
	uint32_t epoch;             /* body[12:16] of the open/GO              */
	vms_csid_t coordinator_csid;
	uint8_t  coordinator_valid; /* 0 = we could not identify the sender    */
	uint8_t  open_seen;         /* an op-09/08/0d preceded this GO         */
	uint8_t  bitmap;            /* body[55] as received; 0 unless valid    */
	uint8_t  bitmap_valid;      /* only an op-0x09 ADD open carries one    */
	uint8_t  bitmap_popcount;   /* == the post-transition member count     */
	uint8_t  pad[3];

	/* ---- what this node has done, all counted from real dispatches ---- */
	uint32_t transitions_seen;
	uint32_t transitions_completed;
	uint32_t transitions_abandoned;
	uint32_t transitions_superseded; /* a new epoch replaced a pending open */
	uint32_t opens_answered;
	uint32_t aux_echoes;         /* op-0x0f, the class-0x03 extra step     */
	uint32_t steps_sent;         /* op-0x0b originations                   */
	uint32_t step_acks;          /* 0x81/0x0b -- an ACK, NOT the release   */
	uint32_t releases;           /* op-0x0c releases consumed              */
	uint32_t silences;           /* pairs grounded as never-answered       */
	uint32_t ungrounded;         /* pairs with no allowlist row: logged    */
	uint32_t send_failures;      /* no CSB for the destination; nothing sent*/

	/* ---- the interleaved rebuild records ---- */
	uint32_t rebuild_records;    /* cat-0x02 op-0x0d received              */
	uint32_t rebuild_dlm;        /* ... answered from REAL lock state      */
	uint32_t rebuild_echoed;     /* ... answered with the grounded echo    */
	uint32_t rebuild_declined;   /* ... the DLM declined; we sent nothing  */

	/* ---- instrumentation: the things the spec says to watch ---- */
	uint32_t step_mismatch;      /* a release whose index != our step      */
	uint32_t late_releases;      /* a release with no barrier running      */
	uint32_t slow_steps;         /* watchdog fired; NEVER an abandonment   */
	uint32_t m_above_grounded;   /* committed member count > 4 (spec SS4(p))*/
	uint32_t bitmap_span_residual; /* a nonzero byte outside body[55]      */
	uint32_t bitmap_bit0;        /* bit 0 set -- "bit 0 is never set"      */
	uint32_t bitmap_short;       /* fewer bits than members we can account  */
	uint32_t max_slot_seen;      /* highest CSV slot the wire has asserted */
	uint32_t nodemap_unmapped;   /* set bits with no CSB we could match    */
	uint32_t count_mismatch;     /* local count != the open's popcount     */
	uint32_t ignored_events;     /* no table cell: ignored and COUNTED     */

	/*
	 * The one scratch buffer every built BODY goes through (design sec
	 * 3.2.4: this FSM emits bodies, never a frame). In the context, not
	 * on the stack: this code runs on a VAX kernel stack.
	 */
	uint8_t scratch[VMS_CM_BODY_LEN];

	/*
	 * The reply buffer handed to the DLM (vms_dlm_scs.h RULE A: the lock
	 * manager fills a buffer the connection manager owns and NEVER sends,
	 * so it cannot emit an uncorrelated grant). A DLM that writes nothing
	 * leaves reply.len at 0, which is the honest silence.
	 */
	uint8_t dlm_reply[VMS_CM_BODY_LEN];
};

/* ==========================================================================
 * 6. Lifecycle
 * ========================================================================== */

/* Bind the FSM to a node. Sends nothing, arms nothing. */
void cnxman_barrier_init(struct cnxman_barrier *b, struct vms_cluster *cl,
			 const struct cnxman_ops *ops);

/* Install (or, with NULL, detach) the lock manager's wire arm. */
void cnxman_barrier_set_dlm(struct cnxman_barrier *b,
			    const struct dlm_scs_role_ops *dlm);

/* ==========================================================================
 * 7. Events
 * ========================================================================== */

/*
 * One inbound `VMS$VAXcluster` frame. The FSM classifies it through the CM
 * codec, maps it to a shared enum cnxman_event, and dispatches the
 * [state][event] table. `from_csid` is the sender as the connection manager
 * identified it; `from_valid` is 0 when it could not -- in which case no
 * coordinator identity is recorded (a zero CSID is never "node zero").
 *
 * Returns CNXMAN_BARRIER_RX_NOT_MINE for every CM frame another FSM owns (the
 * join dialogue's 0x02/0x03/0x05/0x06/0x14, the coordinator's inbound 0x0b,
 * the relay 0x12), so the caller routes it on rather than this file guessing.
 */
enum cnxman_barrier_rx cnxman_barrier_rx_frame(struct cnxman_barrier *b,
					       const uint8_t *frame,
					       uint32_t len,
					       vms_csid_t from_csid,
					       int from_valid);

/*
 * The barrier watchdog (CNXMAN_TIMER_BARRIER). INSTRUMENT ONLY. Spec SS4(p):
 * "the coordinator holds 0x0c#N until the slowest member reports, so per-step
 * wait grows with M. DO NOT TIME OUT ON A STEP MERELY BECAUSE IT IS SLOW." This
 * counts and logs; it never abandons and never re-sends.
 */
void cnxman_barrier_timer(struct cnxman_barrier *b);

/*
 * Connectivity to the coordinator was lost mid-transition. The barrier cannot
 * complete without it, so the transition is abandoned locally and the DLM is
 * told (completed = 0) so a partial rebuild unwinds. Membership is NOT touched:
 * p. 7-30 forbids presuming a member departed, and p. 7-42 forbids un-doing a
 * committed Phase 2.
 */
void cnxman_barrier_coordinator_lost(struct cnxman_barrier *b);

/* ==========================================================================
 * 8. Readback
 * ========================================================================== */

/* The p. 7-42 answer: has Phase 2 run for the transition in progress? True
 * from the GO onward -- BEFORE step 1 and BEFORE any rebuild record. */
int cnxman_barrier_phase2_committed(const struct cnxman_barrier *b);

/* Fill `out` with the transition in progress; nonzero when there is none (NOT
 * a zeroed struct that reads like a transition at epoch 0). */
int cnxman_barrier_transition(const struct cnxman_barrier *b,
			      struct cnxman_transition *out);

/* Names, for the console line and the diagnostics. */
const char *cnxman_barrier_state_name(enum cnxman_barrier_state s);

#endif /* OVMX_VMS_CNXMAN_BARRIER_FSM_H */
