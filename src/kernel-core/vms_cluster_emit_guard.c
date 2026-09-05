/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_emit_guard.c - the emit-time wire-safety guard (note E82).
 *
 * READ vms_cluster_emit_guard.h FIRST. It carries why this exists (OVMX has
 * bugchecked real VAXes three times, and once in a 7-13 crash LOOP), where
 * every threshold below came from (tools/cluster/cm_wire_safety_audit.py's
 * measured GROUNDING table, zero false positives over 234 555 real frames),
 * and the two rules this file lives by: an unsafe frame is DROPPED, never
 * clamped (INV-6), and a correct frame is never refused.
 *
 * SHAPE. One decode, then a TABLE of independent checks in severity order:
 * every DROP is tested before every WARN, so the first finding a frame
 * produces is the most serious one it has. Each check is a handful of lines
 * over the decoded view and the caller's live circuit facts; none of them
 * reaches into another layer, allocates, takes a lock or reads a clock.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_cluster_emit_guard.h"

#include "vms_cluster_codec_cm.h"
#include "vms_cluster_codec_scs.h"

/* ==========================================================================
 * 1. Small primitives
 * ========================================================================== */

static void guard_zero(struct cm_guard *g)
{
	uint8_t *p = (uint8_t *)g;
	uint32_t i;

	for (i = 0u; i < (uint32_t)sizeof(*g); i++)
		p[i] = 0u;
}

void cm_guard_init(struct cm_guard *g)
{
	if (g != (struct cm_guard *)0)
		guard_zero(g);
}

/* An UNDECODED view is an all-zero one, never a stale one: every entry point
 * that can return without decoding starts here, so a caller can never read a
 * field left over from the previous frame (INV-6). */
static void guard_view_zero(struct cm_guard_frame *v)
{
	uint8_t *p = (uint8_t *)v;
	uint32_t i;

	for (i = 0u; i < (uint32_t)sizeof(*v); i++)
		p[i] = 0u;
}

static void finding_set(struct cm_guard_finding *out, uint8_t cls,
			uint8_t severity, uint32_t a, uint32_t b)
{
	out->cls = cls;
	out->severity = severity;
	out->pad0 = 0u;
	out->a = a;
	out->b = b;
}

/*
 * WHICH FRAMES THIS GUARD JUDGES.
 *
 * Spec sec 4(d): the VMS$VAXcluster class is a FIXED 190-byte SCA content, and
 * it is the only class whose Con.ID pair and SYSAP-body offsets the spec
 * grounds. VMS_FCLS_SCS_MSG is exactly that class in the codec's table, and in
 * THIS executive nothing else emits it: SCS routes a 132-byte SYSAP body to
 * the 190-content builder and every other SYSAP body is shorter (the MSCP
 * classes are 28/32/36/44/52 bytes, vms_cluster_codec_mscp.h), so a
 * 190-content emission is a connection-manager emission.
 *
 * The auditor makes the identical cut (`is_cm()` keys on the 190 content and
 * nothing else) and its calibration is a calibration OF THAT POPULATION, which
 * is the second reason not to widen it here.
 */
static int guard_class_is_cm(const struct vms_frame_info *fi)
{
	return fi->cls == (uint8_t)VMS_FCLS_SCS_MSG;
}

/*
 * ...AND THE SECOND, FAIL-OPEN GATE. The four categories below are the ones
 * spec sec 4(j)'s table grounds for the VMS$VAXcluster SYSAP. A 190-content
 * body carrying anything else is NOT judged and is COUNTED (`not_judged`).
 *
 * This is a decision to WITHHOLD a judgement, never to assert one: the codec's
 * own rule is that "which SYSAP owns a given frame is decided by its Con.ID
 * connection, above this codec, never by a wire field", and a guard that
 * refused a frame because of a category byte would be doing exactly that. Read
 * only as a suppressor it is safe -- the worst it can do is let an unjudged
 * frame through, which is what a guard that did not exist would do -- and it
 * keeps the envelope rules off bodies whose byte 0 is not a send-msg#.
 */
static int guard_category_is_cm(uint8_t category)
{
	switch ((uint8_t)(category & 0x7fu)) {
	case VMS_CM_CAT_CONFIG:
	case VMS_CM_CAT_DLM:
	case VMS_CM_CAT_ACK:
	case VMS_CM_CAT_MEMBERSHIP:
		return 1;
	default:
		return 0;
	}
}

/* ==========================================================================
 * 2. The decode -- once per frame, through the codec's own entries
 *
 * No raw wire offset appears in this file (design SS3.9 rule 2): the Con.ID
 * pair comes from the class-gated accessor, the SYSAP body from the SCS
 * codec's slicer, and the 10-byte transaction envelope from the CM codec's
 * parser. A frame the codec refuses to decode is NOT JUDGED -- this guard
 * never reads a field the codec would not hand it.
 * ========================================================================== */
static int guard_decode(const uint8_t *frame, uint32_t len,
			const struct vms_frame_info *fi,
			struct cm_guard_frame *view)
{
	const uint8_t *body = (const uint8_t *)0;
	struct vms_cm_envelope env;
	uint32_t body_len = 0u;
	uint32_t remote = 0u, local = 0u;

	view->judged = 0u;
	if (!guard_class_is_cm(fi))
		return 0;
	if (vms_scs_conid(frame, len, fi, &remote, &local) != VMS_CODEC_OK)
		return 0;
	if (vms_scs_msg_body(frame, len, &body, &body_len) != VMS_CODEC_OK)
		return 0;
	if (vms_cm_envelope_parse(body, body_len, &env) != VMS_CODEC_OK)
		return 0;
	if (!guard_category_is_cm(env.category))
		return 0;

	view->judged = 1u;
	view->category = env.category;
	view->opcode = env.opcode;
	view->is_response = (uint8_t)((env.category & 0x80u) ? 1u : 0u);
	view->send_msg = env.send_msg;
	view->ack_msg = env.ack_msg;
	view->txn = env.txn;
	view->pad0 = 0u;
	view->conid_local = local;
	view->conid_remote = remote;
	return 1;
}

/* ==========================================================================
 * 3. The checks
 *
 * Each answers 1 and fills `*out` when the frame is outside the measured
 * envelope, 0 otherwise. Every one of them is one vector of
 * cm_wire_safety_audit.py, with that vector's own measurement in the comment.
 * ========================================================================== */
struct guard_ctx {
	const struct cm_guard       *g;
	const struct cm_guard_facts *facts;
	const struct cm_guard_frame *view;
};

typedef int (*guard_check_fn)(const struct guard_ctx *c,
			      struct cm_guard_finding *out);

/* S10. MEASURED: zero of 306 670 corpus CM frames carry a zero Con.ID. A
 * frame addressed to a Con.ID the peer does not hold cannot resolve to a CDT
 * (spec sec 4(t)); spec sec 4(O.26) records OVMX emitting op 0x05/0x06 to
 * Con.ID 0 after a VC break -- and that is the class N3 crash. */
static int chk_conid(const struct guard_ctx *c, struct cm_guard_finding *out)
{
	if (c->view->conid_local != 0u && c->view->conid_remote != 0u)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_CONID_ZERO,
		    (uint8_t)CM_GUARD_SEV_DROP, c->view->conid_local,
		    c->view->conid_remote);
	return 1;
}

/* S8. Spec sec 4(p): op 0x0a (barrier GO) and op 0x0c (step release) get no
 * response of any kind and carry txn = 0, so there is nothing to correlate.
 * MEASURED: zero such responses across 47 captures -- answering one invents a
 * message VMS never sends, on a transaction the peer does not hold. */
static int chk_answered_notify(const struct guard_ctx *c,
			       struct cm_guard_finding *out)
{
	const struct cm_guard_frame *v = c->view;

	if (!v->is_response || (v->category & 0x7fu) != VMS_CM_CAT_CONFIG)
		return 0;
	if (v->opcode != CM_GUARD_OP_XITION_GO &&
	    v->opcode != CM_GUARD_OP_BARRIER_REL)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_ANSWERED_NOTIFY,
		    (uint8_t)CM_GUARD_SEV_DROP, v->category, v->opcode);
	return 1;
}

/* S9. MEASURED: zero of 103 413 real and zero of 2 953 non-reference
 * responses in the corpus carry txn == 0. Spec sec 4(j): the transaction
 * number is what a request and its response SHARE; a response with none names
 * a transaction the peer cannot look up. */
static int chk_response_txn(const struct guard_ctx *c,
			    struct cm_guard_finding *out)
{
	if (!c->view->is_response || c->view->txn != 0u)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_RESP_TXN_ZERO,
		    (uint8_t)CM_GUARD_SEV_DROP, c->view->category,
		    c->view->opcode);
	return 1;
}

/*
 * S2 -- THE OBSERVED CNXMGRERR, and the reason this file exists.
 *
 * Spec sec 4(j): the ack-msg# acknowledges the peer's HIGHEST send-msg# seen.
 * Acking a message the peer never sent asserts a conversation that did not
 * happen; E76 ran 8 and 13 ahead and bugchecked BOTH reference VAXes.
 *
 * NOT JUDGED until this port has really taken a CM frame from the peer. That
 * is the auditor's own honest-omission rule (INV-6): a high-water mark whose
 * history was not watched cannot be judged, and judging it would refuse the
 * first legitimate frame of every fresh circuit.
 */
static int chk_ack_backed(const struct guard_ctx *c,
			  struct cm_guard_finding *out)
{
	uint32_t bound;

	if (!c->g->peer_heard)
		return 0;
	bound = (uint32_t)c->g->peer_high_send_msg + CM_GUARD_ACK_SLACK;
	if ((uint32_t)c->view->ack_msg <= bound)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_ACK_UNBACKED,
		    (uint8_t)CM_GUARD_SEV_DROP, c->view->ack_msg,
		    c->g->peer_high_send_msg);
	return 1;
}

/*
 * S3 -- THE OBSERVED INVEXCEPTN.
 *
 * MEASURED over 6549 real advancing cat-0x04 acks: the ack-msg# advance is >= 3
 * every time; 1 and 2 occur zero times. Acking a burst frame-for-frame is the
 * E78 vector -- 254 acks in 31.6 ms bugchecked VAX2 and it stayed down.
 *
 * A NON-advancing ack is legitimate and is not judged (real nodes repeat one),
 * and the arithmetic is deliberately the auditor's own signed difference, so a
 * 16-bit wrap reads as a large negative and is likewise not judged rather than
 * being flagged on a boundary the corpus never exercised.
 */
static int chk_ack_coalesce(const struct guard_ctx *c,
			    struct cm_guard_finding *out)
{
	int32_t advance;

	if (c->view->category != VMS_CM_CAT_ACK || !c->g->ack_emitted)
		return 0;
	advance = (int32_t)c->view->ack_msg - (int32_t)c->g->last_ack_emitted;
	if (advance <= 0 || advance >= (int32_t)CM_GUARD_ACK_MIN_COALESCE)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_ACK_COALESCE,
		    (uint8_t)CM_GUARD_SEV_DROP, c->view->ack_msg,
		    c->g->last_ack_emitted);
	return 1;
}

/*
 * S12. Spec sec 4(g): abs 95 of the peer's 0x41 START is its CLUSTER_CREDITS
 * grant, byte-exact. *VAXcluster Principles* p. 2-43: one message costs one
 * credit and the acknowledgement returns it. Transmitting with more messages
 * outstanding than the peer granted over-runs its receive buffering.
 *
 * NOT JUDGED without a real grant and a real acknowledgement from the peer --
 * the auditor's own "not judged until the peer has acknowledged once", and the
 * reason there is no invented baseline here (INV-6).
 */
static int chk_credit_window(const struct guard_ctx *c,
			     struct cm_guard_finding *out)
{
	uint32_t outstanding;

	if (c->facts->send_credit_max == 0u || !c->facts->peer_ack_valid)
		return 0;
	if (c->facts->send_seq == 0u)
		return 0;
	outstanding = (uint32_t)(uint16_t)(c->facts->send_seq -
					   c->facts->peer_recv_ack);
	if (outstanding <= (uint32_t)c->facts->send_credit_max)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_CREDIT_OVERSEND,
		    (uint8_t)CM_GUARD_SEV_DROP, outstanding,
		    c->facts->send_credit_max);
	return 1;
}

/* Is this emission the first one on a Con.ID pair that is not the pair the
 * last emission rode? That is the auditor's "re-opened dialogue" -- a second
 * or later pair between two stations it has already watched. */
static int guard_is_fresh_dialogue(const struct guard_ctx *c)
{
	const struct cm_guard *g = c->g;

	if (!g->dlg_valid || g->dialogues == 0u)
		return 0;
	return g->dlg_local != c->view->conid_local ||
	       g->dlg_remote != c->view->conid_remote;
}

/*
 * S1 -- WARN, and deliberately so.
 *
 * Spec sec 4(j) grounds the send-msg# as strictly monotonic per sender,
 * starting at 1 on the first VC message. But the auditor's own re-measurement
 * CORRECTED the E76 reading: three reference captures show BOTH real VMS nodes
 * open a second VMS$VAXcluster Con.ID pair at send-msg# 9 -- continuing the
 * count they had already sent that peer, restarting at neither 1 nor 9's
 * predecessor -- with no bugcheck. "Opened at != 1" is therefore NOT the
 * crash, cannot be asserted as an invariant, and must not stop a frame. What
 * IS reported is the jump E76 really made: a number that neither restarts nor
 * continues, because the counter had been advanced by sends that never left.
 */
static int chk_envelope_open(const struct guard_ctx *c,
			     struct cm_guard_finding *out)
{
	const struct cm_guard *g = c->g;
	uint32_t continues;

	if (!guard_is_fresh_dialogue(c) || !g->own_sent || !g->peer_heard)
		return 0;
	continues = (uint32_t)g->own_high_send_msg + 1u;
	if (c->view->send_msg == 1u || (uint32_t)c->view->send_msg == continues)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_ENVELOPE_JUMP,
		    (uint8_t)CM_GUARD_SEV_WARN, c->view->send_msg,
		    g->own_high_send_msg);
	return 1;
}

/*
 * The mid-dialogue half of the same fact, and a WARN for the same reason.
 *
 * Spec sec 4(j)'s "strictly monotonic per sender" is grounded 2902/2902 on the
 * golden VC, so a send-msg# that does not advance is outside the envelope. It
 * is NOT a drop, because the auditor -- the only calibrated instrument this
 * campaign has -- does not judge the mid-dialogue case at all, and this file's
 * licence to refuse extends exactly as far as that calibration does.
 */
static int chk_envelope_advance(const struct guard_ctx *c,
				struct cm_guard_finding *out)
{
	const struct cm_guard *g = c->g;

	if (!g->dlg_sent || guard_is_fresh_dialogue(c))
		return 0;
	if ((int32_t)c->view->send_msg - (int32_t)g->dlg_last_send_msg > 0)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_ENVELOPE_STALL,
		    (uint8_t)CM_GUARD_SEV_WARN, c->view->send_msg,
		    g->dlg_last_send_msg);
	return 1;
}

/*
 * S4 -- the softer, secondary bound, at the auditor's own WARN severity.
 * MEASURED ceiling 111: the most cat-0x04 acks any real VMS node emitted in
 * any 50 ms window in the corpus (a node LEAVING, the busiest ack moment the
 * library contains).
 */
static int chk_ack_rate(const struct guard_ctx *c,
			struct cm_guard_finding *out)
{
	const struct cm_guard *g = c->g;
	uint32_t elapsed, would_be;

	if (c->view->category != VMS_CM_CAT_ACK)
		return 0;
	elapsed = c->facts->now_ms - g->ack_window_start_ms;
	if (g->ack_window_count == 0u || elapsed > CM_GUARD_ACK_WINDOW_MS)
		return 0;
	would_be = (uint32_t)g->ack_window_count + 1u;
	if (would_be <= CM_GUARD_ACK_MAX_PER_WINDOW)
		return 0;
	finding_set(out, (uint8_t)CM_GUARD_C_ACK_RATE,
		    (uint8_t)CM_GUARD_SEV_WARN, would_be,
		    CM_GUARD_ACK_MAX_PER_WINDOW);
	return 1;
}

/*
 * THE TABLE. Every DROP before every WARN, so the finding a frame reports is
 * the most serious one it has -- and so a frame that is going to be refused is
 * refused by the cheapest check that can see it.
 */
static const guard_check_fn guard_checks[] = {
	chk_conid,             /* S10 DROP */
	chk_answered_notify,   /* S8  DROP */
	chk_response_txn,      /* S9  DROP */
	chk_ack_backed,        /* S2  DROP */
	chk_ack_coalesce,      /* S3  DROP */
	chk_credit_window,     /* S12 DROP */
	chk_envelope_open,     /* S1  WARN */
	chk_envelope_advance,  /*     WARN */
	chk_ack_rate           /* S4  WARN */
};

#define GUARD_N_CHECKS ((uint32_t)(sizeof(guard_checks) / \
				   sizeof(guard_checks[0])))

/* ==========================================================================
 * 4. The entry points
 * ========================================================================== */

int cm_guard_check_tx(struct cm_guard *g, const struct cm_guard_facts *facts,
		      const uint8_t *frame, uint32_t len,
		      const struct vms_frame_info *fi,
		      struct cm_guard_frame *view,
		      struct cm_guard_finding *out)
{
	struct guard_ctx ctx;
	uint32_t i;

	if (out != (struct cm_guard_finding *)0)
		finding_set(out, (uint8_t)CM_GUARD_C_NONE,
			    (uint8_t)CM_GUARD_SEV_NONE, 0u, 0u);
	/* A guard that cannot run must never be the reason a frame does not
	 * go out: every missing argument answers EMIT. */
	if (g == (struct cm_guard *)0 || facts == (const struct cm_guard_facts *)0
	    || frame == (const uint8_t *)0 || fi == (const struct vms_frame_info *)0
	    || view == (struct cm_guard_frame *)0
	    || out == (struct cm_guard_finding *)0)
		return CM_GUARD_EMIT;
	guard_view_zero(view);

	/*
	 * S11 IS JUDGED FIRST, AND BEFORE THE BODY DECODE.
	 *
	 * MEASURED: 306 670 of 306 670 corpus CM frames are exactly 204 bytes
	 * (spec sec 4(d), the fixed 190-content class). A frame that ASSERTS
	 * that class in its SCA length and is not that long hands the peer a
	 * body it will index past -- and it is precisely the frame whose body
	 * the decode below would refuse, so judging it after the decode would
	 * mean never judging it at all. The length disagreeing with the class
	 * IS the whole finding; no field of the body is needed to state it.
	 */
	if (guard_class_is_cm(fi) && len != CM_GUARD_FRAME_BYTES) {
		view->judged = 1u;
		g->judged++;
		g->refused++;
		finding_set(out, (uint8_t)CM_GUARD_C_FRAME_SIZE,
			    (uint8_t)CM_GUARD_SEV_DROP, len,
			    CM_GUARD_FRAME_BYTES);
		g->last_class = out->cls;
		return CM_GUARD_REFUSE;
	}

	if (!guard_decode(frame, len, fi, view)) {
		g->not_judged++;
		return CM_GUARD_EMIT;
	}
	g->judged++;

	ctx.g = g;
	ctx.facts = facts;
	ctx.view = view;
	for (i = 0u; i < GUARD_N_CHECKS; i++) {
		if (!guard_checks[i](&ctx, out))
			continue;
		g->last_class = out->cls;
		if (out->severity == (uint8_t)CM_GUARD_SEV_DROP) {
			g->refused++;
			return CM_GUARD_REFUSE;
		}
		g->warned++;
		return CM_GUARD_EMIT;
	}
	return CM_GUARD_EMIT;
}

/* The 50 ms ack window, advanced only by acks that really went out. */
static void guard_note_ack_window(struct cm_guard *g, uint32_t now_ms)
{
	uint32_t elapsed = now_ms - g->ack_window_start_ms;

	if (g->ack_window_count == 0u || elapsed > CM_GUARD_ACK_WINDOW_MS) {
		g->ack_window_start_ms = now_ms;
		g->ack_window_count = 1u;
		return;
	}
	if (g->ack_window_count != 0xffffu)
		g->ack_window_count++;
}

/* The dialogue this emission rode. A pair different from the last one is a
 * new dialogue and is COUNTED, which is what makes the next fresh-open
 * judgement possible at all. */
static void guard_note_dialogue(struct cm_guard *g,
				const struct cm_guard_frame *view)
{
	if (g->dlg_valid && g->dlg_local == view->conid_local &&
	    g->dlg_remote == view->conid_remote)
		return;
	g->dlg_local = view->conid_local;
	g->dlg_remote = view->conid_remote;
	g->dlg_valid = 1u;
	/* A DIFFERENT connection is a DIFFERENT conversation: its send-msg#
	 * history starts empty, exactly as cnxman_csb_bind_connection() starts
	 * the CSB's own counters at zero on a rebind (E77). */
	g->dlg_sent = 0u;
	g->dlg_last_send_msg = 0u;
	if (g->dialogues != 0xffffu)
		g->dialogues++;
}

void cm_guard_sent(struct cm_guard *g, const struct cm_guard_frame *view,
		   uint32_t now_ms)
{
	if (g == (struct cm_guard *)0 || view == (const struct cm_guard_frame *)0)
		return;
	if (!view->judged)
		return;

	guard_note_dialogue(g, view);
	/* The NODE-level high-water mark is a plain maximum, exactly as the
	 * auditor keeps it -- it survives a dialogue rebind, because the
	 * fresh-open rule is stated against "what this node has already sent
	 * this peer" and not against one connection's counter. */
	if (!g->own_sent || (int32_t)view->send_msg -
			    (int32_t)g->own_high_send_msg > 0)
		g->own_high_send_msg = view->send_msg;
	g->own_sent = 1u;
	/* ...and the DIALOGUE-level one is simply the last number that went
	 * out on this Con.ID pair. */
	g->dlg_last_send_msg = view->send_msg;
	g->dlg_sent = 1u;

	if (view->category != VMS_CM_CAT_ACK)
		return;
	g->last_ack_emitted = view->ack_msg;
	g->ack_emitted = 1u;
	guard_note_ack_window(g, now_ms);
}

void cm_guard_rx(struct cm_guard *g, const uint8_t *frame, uint32_t len,
		 const struct vms_frame_info *fi)
{
	struct cm_guard_frame view;

	if (g == (struct cm_guard *)0 || frame == (const uint8_t *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return;
	if (!guard_decode(frame, len, fi, &view))
		return;
	/*
	 * A plain maximum over what really arrived, exactly as the auditor
	 * keeps it -- never a copy of the last frame's field, because a
	 * retransmit legitimately repeats a lower number (spec sec 4(j): 2 of
	 * 17 541 golden frames do).
	 */
	if (!g->peer_heard ||
	    (int32_t)view.send_msg - (int32_t)g->peer_high_send_msg > 0)
		g->peer_high_send_msg = view.send_msg;
	g->peer_heard = 1u;
}

int cm_guard_log_due(struct cm_guard *g, uint8_t cls, uint32_t now_ms)
{
	uint16_t bit;

	if (g == (struct cm_guard *)0 || cls == (uint8_t)CM_GUARD_C_NONE ||
	    cls >= (uint8_t)CM_GUARD_C__COUNT)
		return 0;

	/* The FIRST sighting of a vector always prints: that line IS the
	 * diagnosis, and losing it to a throttle would defeat the guard. */
	bit = (uint16_t)(1u << cls);
	if ((g->log_seen & bit) == 0u) {
		g->log_seen = (uint16_t)(g->log_seen | bit);
		g->log_next_ms = now_ms + CM_GUARD_LOG_INTERVAL_MS;
		g->log_primed = 1u;
		return 1;
	}
	/* Repeats are rate-limited, so the guard can never become the flood it
	 * exists to stop (wrap-safe: the difference, not the ordering). */
	if (g->log_primed && (int32_t)(now_ms - g->log_next_ms) < 0)
		return 0;
	g->log_next_ms = now_ms + CM_GUARD_LOG_INTERVAL_MS;
	g->log_primed = 1u;
	return 1;
}

const char *cm_guard_class_name(uint8_t cls)
{
	switch (cls) {
	case CM_GUARD_C_NONE:            return "-";
	case CM_GUARD_C_ENVELOPE_JUMP:   return "envelope-jump";
	case CM_GUARD_C_ACK_UNBACKED:    return "ack-unbacked";
	case CM_GUARD_C_ACK_COALESCE:    return "ack-coalesce";
	case CM_GUARD_C_ACK_RATE:        return "ack-rate";
	case CM_GUARD_C_ANSWERED_NOTIFY: return "answered-notification";
	case CM_GUARD_C_RESP_TXN_ZERO:   return "response-txn-zero";
	case CM_GUARD_C_CONID_ZERO:      return "conid-zero";
	case CM_GUARD_C_FRAME_SIZE:      return "frame-size";
	case CM_GUARD_C_CREDIT_OVERSEND: return "credit-oversend";
	case CM_GUARD_C_ENVELOPE_STALL:  return "envelope-stall";
	default:                         return "?";
	}
}
