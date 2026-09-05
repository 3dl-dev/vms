/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_emit_guard.h - THE EMIT-TIME WIRE-SAFETY GUARD (integration
 * note E82): the last thing between this executive and a bugchecked peer.
 *
 * ===========================================================================
 * WHY IT EXISTS
 * ===========================================================================
 * OVMX has crashed the reference cluster. Twice by a vector we found only
 * AFTERWARDS, in a pcap:
 *
 *   * `CNXMGRERR` (note E76) -- a SYSAP transaction envelope that acked a
 *     send-msg# the peer had never sent. BOTH reference VAXes took a fatal
 *     bugcheck.
 *   * `INVEXCEPTN` (note E78) -- 254 `cat 0x04` acks in 31.6 ms, one per frame
 *     of a coordinator's membership burst. VAX2 bugchecked and stayed down.
 *
 * And once as a CRASH LOOP (note E80's re-fire): a single vector, combined
 * with this node's own resilience, crashed each VAX 7-13 times in one run --
 * crash, reboot, OVMX re-issues, crash again.
 *
 * tools/cluster/cm_wire_safety_audit.py already MEASURES that envelope, and it
 * is calibrated: ZERO findings over 19 pure-reference captures / 234 555 real
 * CM frames, while flagging all three observed crashes. But it is a POST-HOC
 * auditor over a capture file. It did not prevent one crash, because by the
 * time it runs the frame has been on the wire for hours.
 *
 * This file is that same measured envelope, executed AT EMIT TIME, inside the
 * executive, on the frame that is about to leave. A frame outside the envelope
 * is REFUSED. The join then fails -- safely, with a named reason -- instead of
 * taking a production cluster down.
 *
 * ===========================================================================
 * IT IS A BACKSTOP, NOT THE FIX
 * ===========================================================================
 * The FSMs above must emit correct frames; when they do, NOTHING here ever
 * fires. This exists so that a bug that survives review, host tests and the
 * simulator still cannot crash a peer. Every DROP below is a defect upstream
 * and is recorded as one (a counter, one throttled console line, and a
 * `CNXMAN_DIAG_R_UNSAFE_EMIT` ring record naming the class).
 *
 * ===========================================================================
 * THE THRESHOLDS ARE THE AUDITOR'S, NOT NEW ONES
 * ===========================================================================
 * Every constant and every rule below is copied from
 * tools/cluster/cm_wire_safety_audit.py's GROUNDING table, with that file's
 * own measurement quoted beside it. Nothing here is a new invariant invented
 * for the executive, because a new invariant would be an UNCALIBRATED one --
 * and a guard that refuses a CORRECT frame breaks the join just as thoroughly
 * as a crash. The auditor's zero-false-positive record over 234 555 real
 * frames is the only evidence that these rules pass all correct output, so
 * these are exactly those rules and no others.
 *
 * TWO DELIBERATE DEVIATIONS, both toward fewer refusals:
 *
 *   1. The auditor reports S1 (a dialogue opened at a send-msg# that neither
 *      restarts at 1 nor continues the sender's own count) at WARN, because
 *      its own re-measurement found real VMS nodes opening a second Con.ID
 *      pair at send-msg# 9. It is a WARN here too -- COUNTED and logged, never
 *      dropped. The FATAL half of the E76 vector is S2, which IS a drop.
 *   2. S4 (the ack-rate ceiling) is the auditor's softer, secondary bound and
 *      is reported at WARN there. It is a WARN here.
 *
 * ===========================================================================
 * INV-6: NOTHING HERE IS FABRICATED, AND NOTHING IS CLAMPED
 * ===========================================================================
 * Every value this guard compares against is a real observation:
 *
 *   - the peer's high-water send-msg# is the maximum over CM frames THIS PORT
 *     REALLY RECEIVED and delivered (`cm_guard_rx`, called from the port's own
 *     delivery path). A peer that has said nothing is `peer_heard == 0` and
 *     the ack rule is NOT JUDGED -- the auditor's own honest-omission rule
 *     ("a capture cannot judge a counter whose history it did not watch").
 *   - our own high-water send-msg# is the maximum over CM frames THIS PORT
 *     REALLY TRANSMITTED (`cm_guard_sent`, called only after the substrate
 *     accepted the bytes). It is NOT advanced by a refused send -- which is
 *     precisely the defect that produced E76 ("its counter was incremented on
 *     sends that were refused and never left the node").
 *   - the credit facts come from the circuit the caller holds, passed in as
 *     `struct cm_guard_facts` -- read by the caller off live `struct pe_vc`
 *     state, never defaulted here.
 *
 * An unsafe frame is DROPPED. It is never rewritten, never clamped and never
 * "made safe": a frame whose envelope is wrong carries a wrong ASSERTION, and
 * editing the assertion into a plausible one is exactly the fabrication INV-6
 * forbids. The caller is told, and the failure propagates as an ordinary send
 * refusal.
 *
 * ===========================================================================
 * WHAT IT COSTS
 * ===========================================================================
 * Allocation-free, lock-free, clock-free (the caller passes its own injected
 * `now_ms`), no globals, no strings on the judging path. The whole judgement
 * is a codec header parse, a 10-byte envelope parse and a dozen integer
 * comparisons per originated frame -- and NONE of it runs on the retransmit
 * path, which re-sends bytes this guard already passed.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * PURE TU: no seam call, no allocation, no clock, no globals -- so it runs
 * identically in both kmods, in the R1 host tests and in the rung-2 simulator.
 */
#ifndef OVMX_VMS_CLUSTER_EMIT_GUARD_H
#define OVMX_VMS_CLUSTER_EMIT_GUARD_H

#include "vms_cluster_codec.h"

/* ==========================================================================
 * 1. The vectors -- one per measured invariant, named after the auditor's
 *    own S<n> vector so a console line, a ring record and a pcap audit of the
 *    same run use ONE vocabulary.
 * ========================================================================== */
enum cm_guard_class {
	CM_GUARD_C_NONE = 0,
	CM_GUARD_C_ENVELOPE_JUMP  = 1,  /* S1  WARN: opened at a jumped #   */
	CM_GUARD_C_ACK_UNBACKED   = 2,  /* S2  DROP: acked what was never
					 *          sent (the CNXMGRERR)     */
	CM_GUARD_C_ACK_COALESCE   = 3,  /* S3  DROP: per-frame ack burst
					 *          (the INVEXCEPTN)         */
	CM_GUARD_C_ACK_RATE       = 4,  /* S4  WARN: past the busiest real
					 *          50 ms window             */
	CM_GUARD_C_ANSWERED_NOTIFY = 5, /* S8  DROP: answered a notification */
	CM_GUARD_C_RESP_TXN_ZERO  = 6,  /* S9  DROP: response with no txn    */
	CM_GUARD_C_CONID_ZERO     = 7,  /* S10 DROP: a Con.ID no CDT holds   */
	CM_GUARD_C_FRAME_SIZE     = 8,  /* S11 DROP: not the fixed class len */
	CM_GUARD_C_CREDIT_OVERSEND = 9, /* S12 DROP: past the peer's grant   */
	CM_GUARD_C_ENVELOPE_STALL = 10, /* WARN: send-msg# did not advance   */
	CM_GUARD_C__COUNT
};

/* What the guard decided about one frame. */
enum cm_guard_verdict {
	CM_GUARD_EMIT   = 0,   /* inside the measured envelope: send it     */
	CM_GUARD_REFUSE = 1    /* outside it: DO NOT put this on the wire   */
};

enum cm_guard_severity {
	CM_GUARD_SEV_NONE = 0,
	CM_GUARD_SEV_WARN = 1, /* counted + logged; the frame still goes    */
	CM_GUARD_SEV_DROP = 2  /* counted + logged; the frame does NOT go   */
};

/* ==========================================================================
 * 2. The measured constants (cm_wire_safety_audit.py's GROUNDING table)
 * ========================================================================== */

/*
 * Spec sec 4(d): the VMS$VAXcluster class is a fixed 190-byte SCA content.
 * MEASURED: every CM frame in the corpus is exactly 204 bytes -- 306 670 of
 * 306 670 counting all sources, 299 224 of 299 224 counting reference nodes.
 * (VMS_CM_FRAME_LEN carries the same 204 in the codec that owns the class.)
 */
#define CM_GUARD_FRAME_BYTES 204u

/*
 * MEASURED: over 6549 ADVANCING cat-0x04 acks from real VMS nodes, the SYSAP
 * ack-msg# advances by 3 or more EVERY time -- an advance of 1 occurs 0 times
 * and an advance of 2 occurs 0 times. Real VMS coalesces; it never acks a
 * burst frame-for-frame. This is the E78 `INVEXCEPTN` vector's invariant.
 */
#define CM_GUARD_ACK_MIN_COALESCE 3u

/*
 * MEASURED: the most cat-0x04 acks any real VMS node emitted inside any 50 ms
 * window, anywhere in the corpus (a node LEAVING, the busiest ack moment the
 * library contains). A secondary, softer bound than the coalescing law above,
 * and reported at WARN for that reason.
 */
#define CM_GUARD_ACK_MAX_PER_WINDOW 111u
#define CM_GUARD_ACK_WINDOW_MS       50u

/*
 * How far an ack may legitimately run ahead of the peer's highest send-msg#.
 * MEASURED: over 234 555 reference CM frames exactly two acks run ahead at
 * all, and both by exactly +1 -- a frame the peer sent and tcpdump missed, not
 * a claim about a message that does not exist. The E76 crash ran ahead by 8
 * and by 13, so keeping the corpus's own tolerance costs the check nothing.
 *
 * IN THE EXECUTIVE there is no capture loss -- this guard sees every frame the
 * port really delivered -- so the tolerance is pure headroom. It is kept
 * anyway, at exactly the auditor's value, because the guard's whole licence to
 * refuse is that these thresholds are the calibrated ones.
 */
#define CM_GUARD_ACK_SLACK 1u

/*
 * Spec sec 4(p)/sec 4(q): op 0x0a (barrier GO) and op 0x0c (step release) are
 * NOTIFICATIONS -- they carry txn = 0 and are never answered. MEASURED: zero
 * responses correlated to either, in 47 captures.
 */
#define CM_GUARD_OP_XITION_GO   0x0au
#define CM_GUARD_OP_BARRIER_REL 0x0cu

/*
 * How long a circuit may go without a console line about the SAME vector.
 * The guard must never become the flood: E78's own crash was a per-frame
 * reflex, and a per-frame console line answering a per-frame defect would be
 * the same mistake one layer up. The FIRST occurrence of each distinct class
 * always prints (that is the diagnosis); every repeat is rate-limited.
 *
 * OVMX's OWN choice, labelled as one -- there is no published VMS constant for
 * a console throttle and none is claimed.
 */
#define CM_GUARD_LOG_INTERVAL_MS 10000u

/* ==========================================================================
 * 3. The per-circuit ledger
 *
 * One of these lives in each `struct pe_vc`, because every quantity below is a
 * property of the conversation with ONE peer system. The auditor keys its own
 * high-water marks on the (source, destination) STATION pair for the same
 * reason, and its measured correction to E76 is why they are NOT per Con.ID
 * pair: three reference captures show a real VMS node open a brand-new Con.ID
 * pair and CONTINUE its send-msg# from the previous one.
 *
 * Every field is an observation. There is no default that stands in for a
 * value this port has not seen.
 * ========================================================================== */
struct cm_guard {
	/* ---- the PEER's stream, from CM frames this port really took ---- */
	uint16_t peer_high_send_msg;
	uint8_t  peer_heard;        /* a CM frame really arrived from it     */

	/* ---- OUR stream, from CM frames this port really transmitted ---- */
	uint16_t own_high_send_msg;
	uint8_t  own_sent;

	/* ---- the dialogue (Con.ID pair) the last emission rode ---- */
	uint32_t dlg_local;
	uint32_t dlg_remote;
	uint8_t  dlg_valid;
	uint8_t  dlg_sent;          /* something was emitted on THIS pair    */
	uint16_t dialogues;         /* distinct pairs emitted on            */
	/*
	 * The last send-msg# emitted ON THIS DIALOGUE, and NOT the node-level
	 * high-water mark above it. The two are different on purpose: E77's
	 * connection rebind legitimately RESTARTS this node's counter at 1 on a
	 * new Con.ID pair, so measuring "did it advance" against the node's own
	 * maximum would report every frame of a healthy re-opened dialogue.
	 */
	uint16_t dlg_last_send_msg;
	uint16_t pad0;

	/* ---- the cat-0x04 ack stream this node emits ---- */
	uint16_t last_ack_emitted;
	uint8_t  ack_emitted;
	uint8_t  pad1;
	uint32_t ack_window_start_ms;
	uint16_t ack_window_count;
	uint16_t pad2;

	/* ---- the console throttle (see CM_GUARD_LOG_INTERVAL_MS) ---- */
	uint32_t log_next_ms;
	uint16_t log_seen;          /* bit per class already announced once  */
	uint8_t  log_primed;        /* log_next_ms holds a real deadline     */
	uint8_t  last_class;        /* enum cm_guard_class of the last find  */

	/* ---- counters, every one a thing that really happened ---- */
	uint32_t judged;            /* CM frames this guard judged           */
	uint32_t not_judged;        /* sends outside the judged class        */
	uint32_t refused;           /* frames REFUSED (never emitted)        */
	uint32_t warned;            /* findings that did not stop the frame  */
};

/* ==========================================================================
 * 4. What the CALLER must read out of live executive state
 *
 * These are not the guard's to know: the transport window belongs to the
 * circuit. The caller fills them from the `struct pe_vc` it is holding at the
 * instant of the send, so every credit comparison below is made against the
 * PEER'S OWN GRANT as read from its START body (spec sec 4(g) abs 95) -- never
 * a number this file supplies.
 * ========================================================================== */
struct cm_guard_facts {
	uint16_t send_seq;        /* the VC sequence this frame will consume */
	uint16_t peer_recv_ack;   /* the cumulative ack the PEER last sent   */
	uint8_t  send_credit_max; /* the peer's grant; 0 = none learned yet  */
	uint8_t  peer_ack_valid;  /* the peer has acknowledged at least once */
	uint16_t pad0;
	uint32_t now_ms;          /* the caller's injected clock             */
};

/*
 * The decoded frame, computed ONCE by cm_guard_check_tx() and handed back to
 * cm_guard_sent() so a frame is parsed exactly once on the send path.
 * `judged` clear means this send was not a VMS$VAXcluster CM frame and NOTHING
 * below it is meaningful.
 */
struct cm_guard_frame {
	uint8_t  judged;
	uint8_t  category;      /* body[8] VERBATIM, response bit included   */
	uint8_t  opcode;        /* body[9]                                   */
	uint8_t  is_response;   /* category & 0x80                           */
	uint16_t send_msg;      /* body[0:2]                                 */
	uint16_t ack_msg;       /* body[2:4]                                 */
	uint16_t txn;           /* body[4:6]                                 */
	uint16_t pad0;
	uint32_t conid_local;   /* abs 68, OURS                              */
	uint32_t conid_remote;  /* abs 64, the destination endpoint's        */
};

/* One finding: which vector, how severe, and the two numbers that name it.
 * `a`/`b` are the OBSERVED and the BOUND, in that order, so a console line or
 * a ring record can print the fact without this file composing a string. */
struct cm_guard_finding {
	uint8_t  cls;        /* enum cm_guard_class                          */
	uint8_t  severity;   /* enum cm_guard_severity                       */
	uint16_t pad0;
	uint32_t a;
	uint32_t b;
};

/* ==========================================================================
 * 5. The service
 * ========================================================================== */

/* Reset a ledger to "nothing observed". Called when a circuit object is
 * allocated, so a reused slot never inherits another system's history. */
void cm_guard_init(struct cm_guard *g);

/*
 * JUDGE ONE FRAME ABOUT TO BE EMITTED.
 *
 * `frame`/`len`/`fi` are the fully-built frame the caller is about to hand to
 * the substrate, classified by the codec. `facts` is the live circuit state
 * (SS4). Returns CM_GUARD_EMIT or CM_GUARD_REFUSE; `*out` names the finding
 * (cls == CM_GUARD_C_NONE when there is none) and `*view` carries the decode
 * for cm_guard_sent().
 *
 * MUTATES ONLY THE COUNTERS AND THE LOG THROTTLE. The observation ledgers
 * (own high-water, dialogue, ack stream, ack window) are advanced by
 * cm_guard_sent() alone, because a frame that is judged is not yet a frame
 * that was SENT -- and recording a send that never happened is E76 itself.
 *
 * A NULL `g`, `frame`, `fi` or `view` answers CM_GUARD_EMIT: this guard never
 * becomes the reason a correct frame does not go out.
 */
int cm_guard_check_tx(struct cm_guard *g, const struct cm_guard_facts *facts,
		      const uint8_t *frame, uint32_t len,
		      const struct vms_frame_info *fi,
		      struct cm_guard_frame *view,
		      struct cm_guard_finding *out);

/*
 * THE FRAME REALLY LEFT. Advance the observation ledgers.
 *
 * Called ONLY after the substrate accepted the bytes. `view` is the decode
 * cm_guard_check_tx() produced for the very same frame; a `view` whose
 * `judged` is clear advances nothing.
 */
void cm_guard_sent(struct cm_guard *g, const struct cm_guard_frame *view,
		   uint32_t now_ms);

/*
 * A CM FRAME REALLY ARRIVED from this circuit's peer, in sequence, and was
 * delivered. Raises the peer's high-water send-msg#: the only thing this node
 * is entitled to acknowledge. Frames that were discarded (out of sequence,
 * unclassifiable, not the CM class) are NOT recorded -- acking one would be
 * acking a message this node did not take.
 */
void cm_guard_rx(struct cm_guard *g, const uint8_t *frame, uint32_t len,
		 const struct vms_frame_info *fi);

/*
 * MAY THIS FINDING PRINT A CONSOLE LINE NOW? (throttle, SS2's
 * CM_GUARD_LOG_INTERVAL_MS). The FIRST occurrence of each class always
 * answers 1; repeats answer 1 at most once per interval. Mutates the
 * throttle state, so it is called exactly once per line actually printed.
 *
 * The guard decides WHETHER; the caller, which owns the injected log op and
 * knows the peer's name, decides WHAT -- this TU composes no strings beyond
 * cm_guard_class_name() below.
 */
int cm_guard_log_due(struct cm_guard *g, uint8_t cls, uint32_t now_ms);

/* The stable short name of a vector, for a console line and for the userland
 * ring dumper. Never NULL. */
const char *cm_guard_class_name(uint8_t cls);

#endif /* OVMX_VMS_CLUSTER_EMIT_GUARD_H */
