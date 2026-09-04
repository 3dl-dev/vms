/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_barrier_fsm.c - the cluster state-transition BARRIER, PARTICIPANT
 * side (FC-P3.5).
 *
 * The contract, the five participant obligations, the published phase model and
 * the INFERRED wire<->phase mapping are all in vms_cnxman_barrier_fsm.h. Read it
 * first; this file is the behaviour.
 *
 * READ THE TABLE, NOT THE PROSE. barrier_table[][] below IS the specification of
 * this machine: one cell per [state][event], one small handler per edge, every
 * populated cell citing the spec section or the book page that puts it there,
 * every INFERRED cell marked with its reason, and every empty cell an event the
 * evidence does not connect to that state -- ignored and COUNTED, never guessed.
 *
 * TWO STRUCTURAL SAFETIES, because the failure mode here is "break the cluster":
 *
 *   1. NOTHING IS ANSWERED EXCEPT THROUGH THE ALLOWLIST. barrier_respond_echo()
 *      refuses to build unless vms_cm_allow_table() carries a RESPOND row for
 *      that (category, opcode). The GO and the RELEASE are CONSUME rows there,
 *      so "never answer 0x0a / 0x0c" is not a rule this file remembers -- it is
 *      a shape this file cannot violate. Answering an ungrounded pair is what
 *      crashed two real VAXes (INCONSTATE, INVEXCEPTN).
 *
 *   2. NO RAW WIRE OFFSET. Every field read or written goes through
 *      vms_cluster_codec_cm.h. Two crashes came from body[N] arithmetic in
 *      orchestration code (design SS3.9 rule 2).
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * This TU is PURE: no seam call, no allocation, no clock but ops->now_ms -- so
 * it runs identically in both kmods, in the host unit tests and in the rung-2
 * N-node simulator.
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_phase2.h"
#include "vms_cnxman_barrier_fsm.h"
#include "vms_dlm_ldwv.h"   /* FC-P4.3: Phase 1 discards the directory */
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * Small shared helpers
 * ========================================================================== */

static void barrier_log(const struct cnxman_barrier *b, const char *msg)
{
	if (b->ops != NULL && b->ops->log != NULL)
		b->ops->log(b->ops->ctx, msg);
}

/* The injected clock is read by cnxman_phase2_commit() (which stamps
 * club->last_transition_ms) and by nothing else in this file: a participant
 * never times a barrier step out (spec SS4(p)). */

/* This TU calls no library (a pure TU builds on the host too, where the
 * substrate's memset is not in scope). */
static void barrier_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

/* The bitmap popcount is cnxman_phase2_popcount8(): the participant counting
 * the bits it received and the coordinator counting the bits it asserts must
 * get the same answer, so there is one implementation (vms_cnxman_phase2.h). */

/* ==========================================================================
 * One dispatched message
 * ========================================================================== */
struct barrier_msg {
	/* The SYSAP's OWN 132 bytes -- what SCS hands an input routine, and the
	 * only bytes a SYSAP is entitled to see (design SS3.2.4; E73). */
	const uint8_t         *body;
	uint32_t               len;
	struct vms_cm_envelope env;
	vms_csid_t             from_csid;
	int                    from_valid;
	/* The CLUB slot of the CSB whose connection carried it, or -1. See
	 * barrier_csb_out() for why a PARTICIPANT must address by this. */
	int32_t                from_csb;
};

/* ==========================================================================
 * The allowlist gate -- safety 1
 * ========================================================================== */

static const struct vms_wire_allow_entry *barrier_allow(const struct barrier_msg *m)
{
	return vms_wire_allow_find(vms_cm_allow_table(),
				   (uint8_t)VMS_SYSAP_VMS_VAXCLUSTER,
				   m->env.category, m->env.opcode);
}

/* Is this pair GROUNDED to carry the cat-0x01 echo recipe? Anything else --
 * a CONSUME row, or no row at all -- gets silence. */
static int barrier_echo_allowed(const struct barrier_msg *m)
{
	const struct vms_wire_allow_entry *e = barrier_allow(m);

	return e != NULL && e->action == (uint8_t)VMS_WIRE_ACT_RESPOND &&
	       e->recipe == (uint16_t)VMS_CM_RECIPE_ECHO;
}

static int barrier_dlm_recipe_allowed(const struct barrier_msg *m)
{
	const struct vms_wire_allow_entry *e = barrier_allow(m);

	return e != NULL && e->action == (uint8_t)VMS_WIRE_ACT_RESPOND &&
	       e->recipe == (uint16_t)VMS_CM_RECIPE_DLM_OP0D;
}

/* ==========================================================================
 * Getting a body out -- via the destination's real CSB (see the header's
 * "THE FSM DOES NOT OWN THOSE AND MUST NOT INVENT THEM"). No CSB for that
 * destination is a refusal to transmit, never a zero-filled body.
 * ========================================================================== */

/*
 * The CSB whose dialogue state stamps body[0:8] for the far end of the
 * transition, ADDRESSED BY THE CONNECTION IT ARRIVED ON (E73).
 *
 * WHY NOT BY CSID. A CSID is an identity the CLUSTER assigns during an ADD
 * transition, and a PARTICIPANT has no grounded way to learn a PEER's: the one
 * wire read this project has (vms_cm_membership_coordinator_csid, E30) reads
 * an op-0x06 that carries "the existing member re-asserting ITS OWN record or
 * another already-admitted member's" -- so which node it names is exactly what
 * is not decidable. cnxman_csb_set_csid() is therefore called only by the
 * COORDINATOR (which assigns them) and by cnxman_club_learn_local_csid(), and
 * vms_cnxman.c's own cnxman_ops_send() says so out loud: "Today this is the
 * ROUTINE case (integration note E30): no CSID is ever learned, so every
 * CSID-addressed origination honestly fails here." A barrier that can only
 * address by CSID cannot send one of its twelve steps.
 *
 * WHAT IS REAL is the CSB the message came in on. Book p. 7-23 makes a CSB
 * "the state of the SCS connection between the local SYS$CLUSTER and the
 * SYS$CLUSTER residing in the system associated with the CSB" -- the block IS
 * the connection record, and the glue resolved it from the Con.ID SCS
 * delivered on. Addressing the coordinator by it is a READ of executive state;
 * deriving a CSID for it would be an inference.
 *
 * `csb_index` < 0, out of range, or a slot not in use is NULL -- a refusal to
 * transmit, never a zero-filled body (INV-6).
 */
static struct vms_csb *barrier_csb_at(struct cnxman_barrier *b,
				      int32_t csb_index)
{
	struct vms_csb *csb;

	if (csb_index < 0)
		return NULL;
	csb = cnxman_club_csb_at(&b->cl->club, (uint32_t)csb_index);
	if (csb == NULL || !csb->in_use)
		return NULL;
	return csb;
}

static void barrier_note_send_failure(struct cnxman_barrier *b, const char *why)
{
	b->send_failures++;
	barrier_log(b, why);
}

/* Answer the request being dispatched, on its own connection, correlated with
 * its transaction (struct cnxman_ops's `respond`). */
static void barrier_emit_response(struct cnxman_barrier *b, uint32_t len)
{
	if (b->ops != NULL && b->ops->respond != NULL)
		(void)b->ops->respond(b->ops->ctx, b->scratch, len);
}

/* ==========================================================================
 * The two response recipes this FSM is entitled to
 * ========================================================================== */

/* The cat-0x01 0x81 echo + its three grounded mutations (spec SS4(p)/(r)). Used
 * for the transition open -- which IS the book's Phase 1 acknowledgement
 * (p. 7-41) -- and for the class-0x03 extra step op-0x0f. */
static void barrier_respond_echo(struct cnxman_barrier *b,
				 const struct barrier_msg *m)
{
	struct vms_csb *csb;
	uint32_t written = 0;

	if (!barrier_echo_allowed(m)) {
		b->ungrounded++;
		barrier_log(b, "%CNXMAN, no grounded response for this "
			       "transition message; sending nothing");
		return;
	}
	csb = barrier_csb_at(b, m->from_csb);
	if (csb == NULL) {
		barrier_note_send_failure(b,
			"%CNXMAN, no connection to the transition coordinator; "
			"open not acknowledged");
		return;
	}
	if (vms_cm_echo_response_build(m->body, m->len, b->tr_class,
				       b->scratch, (uint32_t)sizeof(b->scratch),
				       &written) != VMS_CODEC_OK) {
		barrier_note_send_failure(b,
			"%CNXMAN, transition-open response could not be built");
		return;
	}
	/* body[4:8] is already the request's echoed txn/token (the builder's
	 * verbatim copy); is_response=1 leaves it alone and fills send/ack. */
	cnxman_envelope_originate(csb, b->scratch, 1);
	barrier_emit_response(b, written);
}

/* The cat-0x02 op-0x0d rebuild record. Offered to the lock manager FIRST; the
 * grounded verbatim echo is the fallback (see barrier_h_rebuild). */
static int barrier_respond_dlm_echo(struct cnxman_barrier *b,
				    const struct barrier_msg *m)
{
	struct vms_csb *csb;
	uint32_t written = 0;

	if (!barrier_dlm_recipe_allowed(m)) {
		b->ungrounded++;
		return -1;
	}
	csb = barrier_csb_at(b, m->from_csb);
	if (csb == NULL) {
		barrier_note_send_failure(b,
			"%CNXMAN, no connection to answer a lock-rebuild record");
		return -1;
	}
	if (vms_cm_dlm_op0d_response_build(m->body, m->len, b->scratch,
					   (uint32_t)sizeof(b->scratch),
					   &written) != VMS_CODEC_OK) {
		barrier_note_send_failure(b,
			"%CNXMAN, lock-rebuild response could not be built");
		return -1;
	}
	cnxman_envelope_originate(csb, b->scratch, 1);
	barrier_emit_response(b, written);
	return 0;
}

/* Wrap a body the LOCK MANAGER produced from real lock state and send it on the
 * request's own connection (vms_dlm_scs.h RULE A). */
static int barrier_respond_dlm_body(struct cnxman_barrier *b,
				    const struct barrier_msg *m, uint32_t len)
{
	struct vms_csb *csb;
	uint32_t written = 0;

	if (len != VMS_CM_BODY_LEN)
		return -1;
	csb = barrier_csb_at(b, m->from_csb);
	if (csb == NULL) {
		barrier_note_send_failure(b,
			"%CNXMAN, no connection to return a lock-manager reply");
		return -1;
	}
	if (vms_cm_body_build(m->body, m->len, b->dlm_reply, len, b->scratch,
			      (uint32_t)sizeof(b->scratch),
			      &written) != VMS_CODEC_OK)
		return -1;
	/* body[4:8] is the request's txn/token, echoed by the builder itself
	 * (the DLM's reply never writes body[0:8]); is_response=1 leaves it
	 * and fills only send/ack. */
	cnxman_envelope_originate(csb, b->scratch, 1);
	barrier_emit_response(b, written);
	return 0;
}

/* ==========================================================================
 * Originating a barrier step
 * ========================================================================== */

/*
 * One of the twelve op-0x0b steps, ORIGINATED on the coordinator's own
 * connection (E73).
 *
 * ADDRESSED BY CSB, NOT BY CSID -- see barrier_csb_at() for the whole argument.
 * The short version: `ops->send(dst_csid, ...)` cannot resolve a peer whose
 * CSID this node has no grounded way to learn, and a participant that cannot
 * send its steps strands the coordinator's barrier, which times out and drops
 * healthy members (spec SS4(p)). `ops->send_csb(csb_index, ...)` addresses the
 * connection the executive HOLDS for that system, which is the same thing the
 * book says a CSB is.
 */
static void barrier_send_step(struct cnxman_barrier *b, uint32_t step)
{
	struct vms_csb *csb;
	uint32_t written = 0;

	csb = barrier_csb_at(b, b->coordinator_csb);
	if (csb == NULL) {
		barrier_note_send_failure(b,
			"%CNXMAN, no connection to the coordinator; barrier step "
			"not sent");
		return;
	}
	if (b->ops == NULL || b->ops->send_csb == NULL) {
		barrier_note_send_failure(b,
			"%CNXMAN, no transport for a barrier step; none sent");
		return;
	}
	if (vms_cm_barrier_build(b->epoch, step, b->scratch,
				 (uint32_t)sizeof(b->scratch),
				 &written) != VMS_CODEC_OK) {
		barrier_note_send_failure(b,
			"%CNXMAN, barrier step could not be built");
		return;
	}
	/* A genuine origination: the coordinator's CSB own txn/token belong
	 * at body[4:8]. */
	cnxman_envelope_originate(csb, b->scratch, 0);
	(void)b->ops->send_csb(b->ops->ctx, b->coordinator_csb, b->scratch,
			       written);
	b->step = (uint8_t)step;
	b->steps_sent++;
}

/* ==========================================================================
 * The DLM transition callbacks
 * ========================================================================== */

static void barrier_fill_transition(const struct cnxman_barrier *b,
				    struct cnxman_transition *tr)
{
	barrier_bzero(tr, (uint32_t)sizeof(*tr));
	tr->epoch = b->epoch;
	tr->tr_class = b->tr_class;
	tr->barrier_step = b->step;
	tr->coordinator_valid = b->coordinator_valid;
	tr->we_coordinate = 0u;   /* this file is the PARTICIPANT side */
	tr->coordinator_csid = b->coordinator_csid;
	/* subject_csid stays absent: the wire's open names the membership
	 * bitmap, not which slot is the subject, and inventing one would be a
	 * fabricated identity (INV-6). FC-P3.12 fills it where it is known. */
}

static void barrier_dlm_begin(struct cnxman_barrier *b)
{
	struct cnxman_transition tr;

	/*
	 * PHASE 1 DISCARDS THE DIRECTORY (FC-P4.3; Davis p. 6-33, pp. 7-40/7-41).
	 * A transition can move a resource name to a different directory node, so
	 * "all directory information cluster-wide is discarded" and the vector is
	 * refilled at Phase 2 (vms_cnxman_phase2.c task 5). Unconditional, and
	 * BEFORE the DLM is told the transition began: between here and the commit
	 * this node resolves no directory at all, which is the honest state -- the
	 * alternative is routing a lookup through a vector the cluster is in the
	 * middle of changing. The identical two lines are in the coordinator's
	 * coord_dlm_begin(): the book gives the SAME obligation to both sides, and
	 * each side owns its own Phase 1.
	 */
	if (b->cl != NULL)
		vms_ldwv_invalidate(&b->cl->club.ldwv);

	if (b->dlm == NULL || b->dlm->transition_begin == NULL)
		return;
	barrier_fill_transition(b, &tr);
	b->dlm->transition_begin(b->dlm->ctx, &tr);
}

static void barrier_dlm_end(struct cnxman_barrier *b, int completed)
{
	struct cnxman_transition tr;

	if (b->dlm == NULL || b->dlm->transition_end == NULL)
		return;
	barrier_fill_transition(b, &tr);
	b->dlm->transition_end(b->dlm->ctx, &tr, completed);
}

/* ==========================================================================
 * The membership bitmap -- Phase 1's nodemap, and the WIDTH instrumentation
 *
 * Spec SS4(p): popcount(body[55]) equals the post-transition member count in
 * 54 of 54 opens, bit k is the member holding CSID index k, bit 0 is never set,
 * and the field is CERTAINLY wider than the one byte the wire has shown --
 * extent and endianness UNDETERMINED. So this code READS one byte, RECORDS what
 * it read, and COUNTS every observation that would settle the width. It decodes
 * nothing beyond the grounded byte: a guessed encoding here loses a member
 * silently, which is exactly how a barrier ends up permanently one short.
 * ========================================================================== */

/* body[52:60] is all-zero outside body[55] in every specimen. A nonzero byte
 * anywhere else is THE observation that widens the field -- so it is counted
 * and logged, never decoded. */
static void barrier_check_bitmap_span(struct cnxman_barrier *b,
				      const struct barrier_msg *m)
{
	uint8_t span[VMS_CM_BITMAP_SPAN_LEN];
	uint32_t i;

	if (vms_cm_open_bitmap_span(m->body, m->len, span) !=
	    VMS_CODEC_OK)
		return;

	for (i = 0; i < VMS_CM_BITMAP_SPAN_LEN; i++) {
		if (i == VMS_CM_BITMAP_SPAN_IDX || span[i] == 0u)
			continue;
		b->bitmap_span_residual++;
		barrier_log(b, "%CNXMAN, membership bitmap is wider than the "
			       "grounded byte; recording, not decoding");
		return;
	}
}

/* The highest CSV slot the cluster has ever asserted at us. Slots are handed
 * out round-robin and a vacated slot is not reused (book p. 7-25), so this
 * climbs past the member count over a cluster's life -- which is precisely why
 * "8 slots" is not a safe assumption. */
static void barrier_record_slots(struct cnxman_barrier *b, uint8_t bitmap)
{
	uint32_t bit;

	for (bit = 0; bit < 8u; bit++) {
		if ((bitmap & (uint8_t)(1u << bit)) == 0u)
			continue;
		if (bit + 1u > b->max_slot_seen)
			b->max_slot_seen = bit + 1u;
	}
	/* "bit 0 is never set" (spec SS4(p)): CSV slot 0 is never used
	 * (p. 7-25). If it ever is, our base assumption about where the map
	 * starts is wrong -- record it rather than silently counting a member
	 * that does not exist. */
	if ((bitmap & 0x01u) != 0u)
		b->bitmap_bit0++;
}

static void barrier_take_bitmap(struct cnxman_barrier *b,
				const struct barrier_msg *m,
				const struct vms_cm_open *open)
{
	struct vms_club *club = &b->cl->club;
	uint32_t i;

	b->bitmap_valid = 0u;
	b->bitmap = 0u;
	b->bitmap_popcount = 0u;
	if (!open->has_bitmap)
		return;   /* op 0x08 / cat-01 op 0x0d carry no nodemap at all */

	b->bitmap_valid = 1u;
	b->bitmap = open->bitmap;
	b->bitmap_popcount = (uint8_t)cnxman_phase2_popcount8(open->bitmap);
	barrier_record_slots(b, open->bitmap);
	barrier_check_bitmap_span(b, m);

	/* p. 7-41: the receiver "stores in its CLUB the copy of the nodemap
	 * provided in the message" -- at PHASE 1. Applying it to the CSBs is a
	 * Phase 2 task (p. 7-42) and happens at the GO. */
	for (i = 0; i < (uint32_t)VMS_CLUB_BITMAP_WORDS; i++)
		club->bitmap[i] = 0u;
	club->bitmap[0] = (uint32_t)open->bitmap;
	club->bitmap_slots_seen = 8u;   /* what the wire has actually spoken about */

	if (b->bitmap_popcount > CNXMAN_BARRIER_M_GROUNDED) {
		barrier_log(b, "%CNXMAN, cluster larger than the grounded "
			       "four-member barrier evidence; instrumenting");
	}
}

/* ==========================================================================
 * PHASE 2 -- p. 7-42, and the whole point of this file
 *
 * "At this point, the state transition is committed; it passes beyond the
 * 'point of no return'." The four tasks run HERE, before the synchronised
 * rebuild, which is what makes the member count independent of the DLM.
 *
 * THE TASKS THEMSELVES ARE IN vms_cnxman_phase2.c, not here. p. 7-42 lists them
 * once and EVERY system in the transition runs the same list -- the coordinator
 * (FC-P3.12) included. Two implementations would be two chances to drift, and a
 * coordinator whose own count disagreed with the count it just made every member
 * compute is precisely the fault the instrumentation is there to catch. This
 * function is the participant's half: fill the input from what the coordinator's
 * open actually carried, run the shared tasks, fold the deltas into this FSM's
 * own counters.
 * ========================================================================== */

static void barrier_commit_phase2(struct cnxman_barrier *b)
{
	struct cnxman_phase2_in in;
	struct cnxman_phase2_stats st;

	in.bitmap = b->bitmap;
	in.bitmap_valid = b->bitmap_valid;
	in.bitmap_popcount = b->bitmap_popcount;
	in.pad = 0u;

	(void)cnxman_phase2_commit(b->cl, &in, &st, b->ops);

	b->nodemap_unmapped += st.nodemap_unmapped;
	b->count_mismatch += st.count_mismatch;
	b->bitmap_short += st.bitmap_short;
	b->m_above_grounded += st.m_above_grounded;
	/* What the coordinator's own nodemap said about THIS node, kept for the
	 * commit record (E79). Recorded here because this is where the map was
	 * really read; it is not re-derived anywhere else. */
	b->local_named = st.local_named;
	b->local_in_map = st.local_in_map;
	b->phase2_committed = 1u;
}

/* ==========================================================================
 * The handlers -- one per edge
 * ========================================================================== */

static void barrier_start_transition(struct cnxman_barrier *b,
				     const struct barrier_msg *m,
				     const struct vms_cm_open *open)
{
	struct vms_club *club = &b->cl->club;

	b->epoch = open->epoch;
	b->tr_class = open->cls;
	b->step = 0u;
	b->phase2_committed = 0u;
	b->open_seen = 1u;
	b->coordinator_valid = (uint8_t)(m->from_valid ? 1 : 0);
	b->coordinator_csid = m->from_valid ? m->from_csid : (vms_csid_t)0;
	/* The connection the proposal came in on IS the coordinator's, whether
	 * or not its CSID could be identified (E73). */
	b->coordinator_csb = m->from_csb;
	b->transitions_seen++;

	club->transition_active = 1u;
	club->transition_class = open->cls;
	club->barrier_step = 0u;
	club->epoch = open->epoch;
	club->we_coordinate = 0u;
	club->coordinator_valid = b->coordinator_valid;
	club->coordinator_csid = b->coordinator_csid;
}

/*
 * [*][RX_TR_OPEN] -- PHASE 1. Record the proposal, then acknowledge it with the
 * grounded 0x81 echo (p. 7-41: "each system normally acknowledges to VAX_A that
 * it has received and processed the information"; spec SS4(p): 0x81/0x09 is
 * 54/54 library-wide).
 */
static void barrier_h_open(struct cnxman_barrier *b, const struct barrier_msg *m)
{
	struct vms_cm_open open;

	if (vms_cm_open_parse(m->body, m->len, &open) != VMS_CODEC_OK) {
		/* Classifiable but not parseable: counted, never
		 * acted on with a partially-read body. */
		b->ignored_events++;
		return;
	}

	barrier_start_transition(b, m, &open);
	barrier_take_bitmap(b, m, &open);
	b->state = (uint8_t)CNXMAN_BARRIER_OPEN;
	barrier_dlm_begin(b);

	barrier_respond_echo(b, m);
	b->opens_answered++;
}

/*
 * [OPEN][RX_TR_OPEN] and [STEP][RX_TR_OPEN]. A repeat of the SAME epoch is the
 * coordinator retransmitting: answer it again and change nothing. A DIFFERENT
 * epoch while a barrier is already running is past p. 7-42's point of no
 * return, so it cannot replace the running transition; it is answered (a
 * coordinator that gates on the acknowledgement must not be stranded) and
 * COUNTED. Before the GO, a new epoch supersedes the pending proposal, which is
 * what abandoning-and-reproposing looks like from here.
 *
 * INFERRED. p. 7-41 gives the coordinator's abandon rules but not what a
 * participant does with an unsolicited second proposal, and no capture shows
 * one. The alternative -- ignoring it -- strands the coordinator, which is the
 * documented cluster-breaking outcome; answering and instrumenting is the
 * failure-safe reading.
 */
static void barrier_h_reopen(struct cnxman_barrier *b,
			     const struct barrier_msg *m)
{
	struct vms_cm_open open;

	if (vms_cm_open_parse(m->body, m->len, &open) != VMS_CODEC_OK) {
		/* Classifiable but not parseable: counted, never
		 * acted on with a partially-read body. */
		b->ignored_events++;
		return;
	}

	if (open.epoch == b->epoch) {
		barrier_respond_echo(b, m);   /* a retransmission */
		b->opens_answered++;
		return;
	}
	if (b->state == (uint8_t)CNXMAN_BARRIER_STEP) {
		b->transitions_superseded++;
		barrier_log(b, "%CNXMAN, a new transition opened while one is "
			       "committed; the running one stands");
		barrier_respond_echo(b, m);
		b->opens_answered++;
		return;
	}
	b->transitions_superseded++;
	barrier_dlm_end(b, 0);
	barrier_h_open(b, m);
}

/*
 * [OPEN][RX_TR_GO] and [IDLE][RX_TR_GO] -- PHASE 2, the point of no return.
 *
 * NEVER ANSWERED (spec SS4(p): op 0x0a carries txn=0 and no 0x8a exists in any
 * capture). The count commits HERE, before step 1 goes out and before a single
 * rebuild record is answered (p. 7-42).
 *
 * IDLE is a GROUNDED entry point, not a defensive one: "a class-0x03 removal
 * has no op 0x09 at all -- it starts directly at op 0x0a / tag 0x0360"
 * (spec SS4(p)).
 */
static void barrier_h_go(struct cnxman_barrier *b, const struct barrier_msg *m)
{
	struct vms_cm_open go;

	if (vms_cm_open_parse(m->body, m->len, &go) != VMS_CODEC_OK) {
		/* Classifiable but not parseable: counted, never
		 * acted on with a partially-read body. */
		b->ignored_events++;
		return;
	}

	/* Spec SS4(p): "An op 0x0a whose body[16:18] is not 0x0260 (e.g.
	 * 0x0460, seen on a running cluster) is NOT a barrier start." The tag
	 * is (class << 8) | role, so the role byte is the discriminator and the
	 * class byte says which of the three transitions this is (SS4(r)). */
	if (go.role != VMS_CM_ROLE_GO) {
		b->ignored_events++;
		return;
	}
	b->silences++;   /* answering it would invent a message VMS never sends */

	if (!b->open_seen || go.epoch != b->epoch) {
		/* A removal's GO arrives with no preceding open. */
		b->epoch = go.epoch;
		b->tr_class = go.cls;
		b->open_seen = 0u;
		b->phase2_committed = 0u;
		b->step = 0u;
		b->transitions_seen++;
		b->coordinator_valid = (uint8_t)(m->from_valid ? 1 : 0);
		b->coordinator_csb = m->from_csb;
		b->coordinator_csid = m->from_valid ? m->from_csid
						    : (vms_csid_t)0;
		b->cl->club.transition_active = 1u;
		b->cl->club.transition_class = go.cls;
		b->cl->club.epoch = go.epoch;
		b->cl->club.coordinator_valid = b->coordinator_valid;
		b->cl->club.coordinator_csid = b->coordinator_csid;
		barrier_dlm_begin(b);
	}

	barrier_commit_phase2(b);

	/* Spec SS4(r): "A class-0x04 self-departure emits its op 0x0a and
	 * starts NO barrier at all", and the class-0x04 dialogue is
	 * 0x12 -> 0x03 -> 0x0d -> 0x0a "and then nothing". */
	if (b->tr_class == VMS_CM_CLASS_DEPART) {
		b->state = (uint8_t)CNXMAN_BARRIER_COMPLETE;
		b->open_seen = 0u;
		b->cl->club.transition_active = 0u;
		b->cl->club.reformations++;
		b->transitions_completed++;
		barrier_dlm_end(b, 1);
		barrier_log(b, "%CNXMAN, completed VAXcluster state transition");
		return;
	}

	b->state = (uint8_t)CNXMAN_BARRIER_STEP;
	barrier_send_step(b, 1u);
	b->cl->club.barrier_step = b->step;
}

/*
 * [STEP][RX_BARRIER_ACK] -- the coordinator's 0x81/0x0b.
 *
 * IT IS NOT THE RELEASE (spec SS4(p)'s own table says so in as many words). A
 * participant that advanced on the ack would run ahead of a barrier whose whole
 * purpose is that nobody advances until everybody has reported.
 */
static void barrier_h_step_ack(struct cnxman_barrier *b,
			       const struct barrier_msg *m)
{
	(void)m;
	b->step_acks++;
}

/* Release #12 -- the transition is over (p. 7-42: the coordinator releases its
 * coordinator lock). Spec SS4(q): it "must NOT be answered, exactly like every
 * other release". */
static void barrier_finish(struct cnxman_barrier *b)
{
	/* THE COMMIT (E79). The only place this counter moves, and the only
	 * fact in this executive that means "the coordinator released the last
	 * barrier step". Recorded BEFORE the CLUB bookkeeping below so that a
	 * reader which sees the count move also sees the epoch that moved it. */
	b->commits++;
	b->commit_epoch = b->epoch;
	b->commit_class = b->tr_class;
	b->commit_local_named = b->local_named;
	b->commit_local_in_map = b->local_in_map;

	b->state = (uint8_t)CNXMAN_BARRIER_COMPLETE;
	b->open_seen = 0u;
	b->cl->club.transition_active = 0u;
	b->cl->club.barrier_step = (uint8_t)CNXMAN_BARRIER_STEPS;
	b->cl->club.reformations++;
	b->transitions_completed++;
	barrier_dlm_end(b, 1);
	barrier_log(b, "%CNXMAN, completed VAXcluster state transition");
}

/*
 * [STEP][RX_BARRIER] -- the coordinator's op-0x0c release of step N.
 *
 * NEVER ANSWERED. N < 12 sends N+1; N == 12 completes. A release whose index is
 * not the step we are on is instrumented and does NOT advance anything: the
 * count 12 is the only termination signal there is, so losing count is how a
 * participant ends up sending a thirteenth step or stopping at eleven.
 */
static void barrier_h_release(struct cnxman_barrier *b,
			      const struct barrier_msg *m)
{
	struct vms_cm_barrier rel;

	if (vms_cm_barrier_parse(m->body, m->len, &rel) != VMS_CODEC_OK) {
		/* Classifiable but not parseable: counted, never
		 * acted on with a partially-read body. */
		b->ignored_events++;
		return;
	}

	b->silences++;
	b->releases++;

	if (rel.epoch != b->epoch || rel.step != (uint32_t)b->step) {
		b->step_mismatch++;
		barrier_log(b, "%CNXMAN, barrier release does not match the "
			       "step in flight; not advancing");
		return;
	}
	if (rel.step >= (uint32_t)CNXMAN_BARRIER_STEPS) {
		barrier_finish(b);
		return;
	}
	barrier_send_step(b, rel.step + 1u);
	b->cl->club.barrier_step = b->step;
}

/* A release with no barrier running: recorded, never answered, never acted on. */
static void barrier_h_late_release(struct cnxman_barrier *b,
				   const struct barrier_msg *m)
{
	(void)m;
	b->silences++;
	b->late_releases++;
}

/*
 * [*][RX_REBUILD] -- the cat-0x02 op-0x0d records the coordinator INTERLEAVES
 * with the barrier and gates the next step on (spec SS4(p): five unanswered
 * ones froze a real barrier at step 5, and the coordinator retransmits each up
 * to 3x).
 *
 * The lock manager gets first refusal, with a reply buffer it fills from real
 * lock state (vms_dlm_scs.h RULE A/B). With no DLM attached -- the P3 default --
 * the answer is the codec's VERBATIM echo, which is GROUNDED to 1367/1367 real
 * responses and asserts nothing: "the echo returns the coordinator's own record
 * with a result code and claims nothing, which is exactly why a lock-less
 * joiner answers all 216" (spec SS4(p)). The real rebuild is FC-P5.5.
 */
static void barrier_h_rebuild(struct cnxman_barrier *b,
			      const struct barrier_msg *m)
{
	struct dlm_scs_request req;
	struct dlm_scs_reply reply;

	b->rebuild_records++;

	if (b->dlm == NULL || b->dlm->handle_request == NULL) {
		if (barrier_respond_dlm_echo(b, m) == 0)
			b->rebuild_echoed++;
		return;
	}

	barrier_bzero(&req, (uint32_t)sizeof(req));
	req.from_csid = m->from_valid ? m->from_csid : (vms_csid_t)0;
	req.category = m->env.category;
	req.opcode = m->env.opcode;
	/* The received FRAME, which is what every vms_cluster_codec_cm.h
	 * accessor takes: the DLM reads it through those and never by offset
	 * (vms_dlm_scs.h SS3). */
	req.body = m->body;
	req.len = m->len;

	reply.body = b->dlm_reply;
	reply.cap = (uint32_t)sizeof(b->dlm_reply);
	reply.len = 0u;

	if (b->dlm->handle_request(b->dlm->ctx, &req, &reply) != 0) {
		/* DECLINED -- counted, never hidden, and never papered over
		 * with an echo that would answer for a lock manager that just
		 * said it could not. */
		b->rebuild_declined++;
		return;
	}
	if (reply.len == 0u) {
		if (barrier_respond_dlm_echo(b, m) == 0)
			b->rebuild_echoed++;
		return;
	}
	if (barrier_respond_dlm_body(b, m, reply.len) == 0)
		b->rebuild_dlm++;
}

/*
 * [OPEN][RX_CLOSE] and [STEP][RX_CLOSE] -- the coordinator's cat-0x01 op-0x04
 * transition ABORT (role 0x50). A CONSUME row in the allowlist: it is a
 * notification and is never answered.
 *
 * The DLM is told completed = 0 so a partial rebuild unwinds. Membership is NOT
 * rolled back: p. 7-42 says a committed Phase 2 "cannot be abandoned", so an
 * abort arriving after the GO leaves the count where the commit put it.
 */
static void barrier_h_abort(struct cnxman_barrier *b,
			    const struct barrier_msg *m)
{
	(void)m;
	b->silences++;
	b->state = (uint8_t)CNXMAN_BARRIER_ABANDONED;
	b->cl->club.transition_active = 0u;
	b->transitions_abandoned++;
	barrier_dlm_end(b, 0);
	barrier_log(b, "%CNXMAN, aborting VAXcluster state transition");
}

/*
 * [OPEN][RX_TR_OPEN] for op-0x0f, the class-0x03 extra step (spec SS4(r), role
 * 0x30). It moves no state: it is one more grounded request the participant owes
 * an echo, and its echo alone skips the body[18] mutation (the codec handles
 * that). Routed outside the state table because the shared event vocabulary has
 * no cell for it and stretching one would misname it in every transcript.
 */
static void barrier_aux_echo(struct cnxman_barrier *b,
			     const struct barrier_msg *m)
{
	barrier_respond_echo(b, m);
	b->aux_echoes++;
}

/* ==========================================================================
 * The table. [state][event]; NULL = the evidence does not connect that event
 * to that state, so it is ignored and COUNTED rather than guessed.
 * ========================================================================== */
typedef void (*barrier_handler_t)(struct cnxman_barrier *,
				  const struct barrier_msg *);

static const barrier_handler_t
barrier_table[CNXMAN_BARRIER_STATE__COUNT][CNXMAN_EV__COUNT] = {
	/* [IDLE] nothing in progress. A class-0x02 ADD arrives as an open; a
	 * class-0x03 REMOVE arrives as a bare GO (spec SS4(p)). */
	[CNXMAN_BARRIER_IDLE] = {
		[CNXMAN_EV_RX_TR_OPEN]   = barrier_h_open,
		[CNXMAN_EV_RX_TR_GO]     = barrier_h_go,
		[CNXMAN_EV_RX_BARRIER]   = barrier_h_late_release,
		[CNXMAN_EV_RX_REBUILD]   = barrier_h_rebuild,
	},

	/* [OPEN] Phase 1 done and acknowledged; waiting for the commit. The
	 * coordinator interleaves rebuild records from here on. */
	[CNXMAN_BARRIER_OPEN] = {
		[CNXMAN_EV_RX_TR_OPEN]   = barrier_h_reopen,   /* INFERRED */
		[CNXMAN_EV_RX_TR_GO]     = barrier_h_go,
		[CNXMAN_EV_RX_BARRIER]   = barrier_h_late_release,
		[CNXMAN_EV_RX_REBUILD]   = barrier_h_rebuild,
		[CNXMAN_EV_RX_CLOSE]     = barrier_h_abort,
	},

	/* [STEP] Phase 2 committed and the barrier is running. */
	[CNXMAN_BARRIER_STEP] = {
		[CNXMAN_EV_RX_TR_OPEN]    = barrier_h_reopen,  /* INFERRED */
		[CNXMAN_EV_RX_BARRIER]    = barrier_h_release,
		[CNXMAN_EV_RX_BARRIER_ACK] = barrier_h_step_ack,
		[CNXMAN_EV_RX_REBUILD]    = barrier_h_rebuild,
		[CNXMAN_EV_RX_CLOSE]      = barrier_h_abort,
	},

	/* [COMPLETE] the transition is over. A member keeps answering rebuild
	 * records afterwards -- spec SS4(q) measured 216 of them around a join
	 * -- and the next transition starts with its own open. */
	[CNXMAN_BARRIER_COMPLETE] = {
		[CNXMAN_EV_RX_TR_OPEN]   = barrier_h_open,
		[CNXMAN_EV_RX_TR_GO]     = barrier_h_go,
		[CNXMAN_EV_RX_BARRIER]   = barrier_h_late_release,
		[CNXMAN_EV_RX_REBUILD]   = barrier_h_rebuild,
	},

	/* [ABANDONED] the coordinator aborted. Nothing here revives it; the
	 * next transition arrives as its own open (or, for a removal, its own
	 * GO), exactly as from IDLE. */
	[CNXMAN_BARRIER_ABANDONED] = {
		[CNXMAN_EV_RX_TR_OPEN]   = barrier_h_open,
		[CNXMAN_EV_RX_TR_GO]     = barrier_h_go,
		[CNXMAN_EV_RX_BARRIER]   = barrier_h_late_release,
		[CNXMAN_EV_RX_REBUILD]   = barrier_h_rebuild,
	},
};

/* ==========================================================================
 * Classification: which shared event is this frame?
 *
 * The table is indexed by MEANING, so an opcode re-assignment after a capture is
 * an edit here and nowhere else. Everything this file does not own returns
 * CNXMAN_EV__COUNT and the caller routes the frame on.
 * ========================================================================== */

static enum cnxman_event barrier_event_of_response(const struct barrier_msg *m)
{
	/* The coordinator's 0x81/0x0b acknowledgement of our step. Every other
	 * response belongs to whichever FSM originated the request. */
	if (m->env.opcode == VMS_CM_OP_BARRIER)
		return CNXMAN_EV_RX_BARRIER_ACK;
	return CNXMAN_EV__COUNT;
}

static enum cnxman_event barrier_event_of_config(const struct barrier_msg *m)
{
	switch (m->env.opcode) {
	case VMS_CM_OP_XITION_ADD:      /* 0x09, class-0x02 ADD open      */
	case VMS_CM_OP_XITION_REM:      /* 0x08, class-0x03 REMOVE open   */
	case VMS_CM_OP_DEPART_XITION:   /* 0x0d, class-0x04 departure open */
		return CNXMAN_EV_RX_TR_OPEN;
	case VMS_CM_OP_XITION_GO:
		return CNXMAN_EV_RX_TR_GO;
	case VMS_CM_OP_BARRIER_REL:
		return CNXMAN_EV_RX_BARRIER;
	case VMS_CM_OP_ABORT:
		return CNXMAN_EV_RX_CLOSE;
	default:
		/* 0x01/0x02/0x03/0x05/0x06/0x12/0x14 are the join dialogue
		 * (FC-P3.3); an inbound 0x0b is a member reporting to a
		 * COORDINATOR (FC-P3.12). Not ours. */
		return CNXMAN_EV__COUNT;
	}
}

static enum cnxman_event barrier_event_of(const struct barrier_msg *m)
{
	if (vms_wire_is_response(m->env.category))
		return barrier_event_of_response(m);
	if (m->env.category == VMS_CM_CAT_DLM &&
	    m->env.opcode == VMS_CM_OP_DLM_REBUILD)
		return CNXMAN_EV_RX_REBUILD;
	if (m->env.category == VMS_CM_CAT_CONFIG)
		return barrier_event_of_config(m);
	return CNXMAN_EV__COUNT;
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static enum cnxman_barrier_rx barrier_dispatch(struct cnxman_barrier *b,
					       const struct barrier_msg *m)
{
	enum cnxman_event ev = barrier_event_of(m);
	barrier_handler_t h;

	if (ev == CNXMAN_EV__COUNT) {
		/* op-0x0f is ours -- the class-0x03 extra step -- but it moves
		 * no state, so it never enters the table. */
		if (m->env.category == VMS_CM_CAT_CONFIG &&
		    m->env.opcode == VMS_CM_OP_0F) {
			barrier_aux_echo(b, m);
			return CNXMAN_BARRIER_RX_CONSUMED;
		}
		return CNXMAN_BARRIER_RX_NOT_MINE;
	}
	if ((unsigned)b->state >= (unsigned)CNXMAN_BARRIER_STATE__COUNT)
		return CNXMAN_BARRIER_RX_BAD;

	h = barrier_table[b->state][ev];
	if (h == NULL) {
		b->ignored_events++;
		return CNXMAN_BARRIER_RX_CONSUMED;
	}
	h(b, m);
	return CNXMAN_BARRIER_RX_CONSUMED;
}

enum cnxman_barrier_rx cnxman_barrier_rx_body(struct cnxman_barrier *b,
					      const uint8_t *body,
					      uint32_t len,
					      vms_csid_t from_csid,
					      int from_valid,
					      int32_t from_csb)
{
	struct barrier_msg m;

	if (b == NULL || b->cl == NULL || body == NULL)
		return CNXMAN_BARRIER_RX_BAD;
	if (vms_cm_envelope_parse(body, len, &m.env) != VMS_CODEC_OK)
		return CNXMAN_BARRIER_RX_BAD;

	m.body = body;
	m.len = len;
	m.from_csid = from_csid;
	m.from_valid = from_valid;
	m.from_csb = from_csb;
	return barrier_dispatch(b, &m);
}

/* ==========================================================================
 * The watchdog, connectivity loss, lifecycle and readback
 * ========================================================================== */

void cnxman_barrier_timer(struct cnxman_barrier *b)
{
	if (b == NULL || b->state != (uint8_t)CNXMAN_BARRIER_STEP)
		return;
	/* Spec SS4(p): "the coordinator holds 0x0c#N until the slowest member
	 * reports ... DO NOT TIME OUT ON A STEP MERELY BECAUSE IT IS SLOW."
	 * Counting is the whole action. */
	b->slow_steps++;
	if (b->slow_steps == 1u)
		barrier_log(b, "%CNXMAN, waiting for the cluster state "
			       "transition to release a barrier step");
	if (b->ops != NULL && b->ops->arm_timer != NULL)
		b->ops->arm_timer(b->ops->ctx, CNXMAN_TIMER_BARRIER,
				  (uint32_t)b->step, CNXMAN_BARRIER_WATCH_MS);
}

void cnxman_barrier_coordinator_lost(struct cnxman_barrier *b)
{
	if (b == NULL || b->cl == NULL)
		return;
	if (b->state != (uint8_t)CNXMAN_BARRIER_OPEN &&
	    b->state != (uint8_t)CNXMAN_BARRIER_STEP)
		return;
	b->state = (uint8_t)CNXMAN_BARRIER_ABANDONED;
	b->cl->club.transition_active = 0u;
	b->transitions_abandoned++;
	barrier_dlm_end(b, 0);
	barrier_log(b, "%CNXMAN, aborting VAXcluster state transition");
}

void cnxman_barrier_init(struct cnxman_barrier *b, struct vms_cluster *cl,
			 const struct cnxman_ops *ops)
{
	if (b == NULL)
		return;
	barrier_bzero(b, (uint32_t)sizeof(*b));
	b->cl = cl;
	b->ops = ops;
	b->state = (uint8_t)CNXMAN_BARRIER_IDLE;
	b->coordinator_csb = -1;   /* no transition, so no connection to one */
}

void cnxman_barrier_set_dlm(struct cnxman_barrier *b,
			    const struct dlm_scs_role_ops *dlm)
{
	if (b == NULL)
		return;
	b->dlm = dlm;
}

int cnxman_barrier_phase2_committed(const struct cnxman_barrier *b)
{
	return (b != NULL && b->phase2_committed != 0u) ? 1 : 0;
}

uint32_t cnxman_barrier_commits(const struct cnxman_barrier *b,
				struct cnxman_barrier_commit *out)
{
	if (b == NULL)
		return 0u;
	if (out != NULL && b->commits != 0u) {
		out->epoch = b->commit_epoch;
		out->tr_class = b->commit_class;
		out->local_named = b->commit_local_named;
		out->local_in_map = b->commit_local_in_map;
		out->pad = 0u;
	}
	return b->commits;
}

int cnxman_barrier_transition(const struct cnxman_barrier *b,
			      struct cnxman_transition *out)
{
	if (b == NULL || out == NULL)
		return -1;
	if (b->state != (uint8_t)CNXMAN_BARRIER_OPEN &&
	    b->state != (uint8_t)CNXMAN_BARRIER_STEP)
		return -1;
	barrier_fill_transition(b, out);
	return 0;
}

const char *cnxman_barrier_state_name(enum cnxman_barrier_state s)
{
	switch (s) {
	case CNXMAN_BARRIER_IDLE:      return "idle";
	case CNXMAN_BARRIER_OPEN:      return "open";
	case CNXMAN_BARRIER_STEP:      return "step";
	case CNXMAN_BARRIER_COMPLETE:  return "complete";
	case CNXMAN_BARRIER_ABANDONED: return "abandoned";
	default:                       return "?";
	}
}
