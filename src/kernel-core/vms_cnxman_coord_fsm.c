/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_coord_fsm.c - the cluster state transition, COORDINATOR side
 * (FC-P3.12).
 *
 * The obligation (the `12 x (M-1)` law), the INFERRED selection predicate and
 * its book grounding, what this file originates and what it honestly does not,
 * are all in vms_cnxman_coord_fsm.h. Read it first; this file is the behaviour.
 *
 * READ THE TABLE, NOT THE PROSE. coord_table[][] below IS the specification of
 * this machine: one cell per [state][event], one small handler per edge, every
 * populated cell citing the spec section or the book page that puts it there,
 * and every empty cell an event the evidence does not connect to that state --
 * ignored and COUNTED, never guessed.
 *
 * THREE STRUCTURAL SAFETIES, because a broken coordinator does not fail to
 * join, it DROPS HEALTHY MEMBERS:
 *
 *   1. THE CENSUS IS INDEXED BY CLUB SLOT. There is no parallel participant
 *      list that could disagree with the membership it is a census of. M is
 *      derived from the SELECTED CSBs at the moment the transition opens and
 *      then FROZEN, so a CSB that changes mid-barrier cannot silently shrink or
 *      grow the set a release is waiting on.
 *
 *   2. A RELEASE IS ONLY EVER SENT FROM coord_try_release(), which checks
 *      "every frozen participant has reported THIS step" first. `0x0c#N never
 *      precedes the last 0x0b#N -- 0 violations out of 12 steps in every
 *      transition` (spec SS4(p)) is therefore a shape this file cannot violate,
 *      not a rule it remembers.
 *
 *   3. NO RAW WIRE OFFSET. Every field written or read goes through
 *      vms_cluster_codec_cm.h, and every envelope counter through the
 *      connection manager's own next_out. Nothing here computes a token, an
 *      epoch field position, or a nodemap bit position by hand.
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
#include "vms_cnxman_coord_fsm.h"
#include "vms_cluster_codec_cm.h"

/* ==========================================================================
 * Small shared helpers
 * ========================================================================== */

static void coord_log(const struct cnxman_coord *c, const char *msg)
{
	if (c->ops != NULL && c->ops->log != NULL)
		c->ops->log(c->ops->ctx, msg);
}

static uint32_t coord_now(const struct cnxman_coord *c)
{
	if (c->ops != NULL && c->ops->now_ms != NULL)
		return c->ops->now_ms(c->ops->ctx);
	return 0u;
}

/* This TU calls no library (a pure TU builds on the host too, where the
 * substrate's memset is not in scope). */
static void coord_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

static struct vms_club *coord_club(struct cnxman_coord *c)
{
	return &c->cl->club;
}

/* A CLUB slot, or NULL for -1 / out of range / a free slot. Every identity this
 * file uses comes through here, so none of them can be a caller's opinion. */
static struct vms_csb *coord_csb_at(struct cnxman_coord *c, int32_t index)
{
	struct vms_club *club = coord_club(c);

	if (index < 0 || (uint32_t)index >= club->n_csb)
		return NULL;
	if (!club->csb[index].in_use)
		return NULL;
	return &club->csb[index];
}

static int coord_is_active(const struct cnxman_coord *c)
{
	return c->state == (uint8_t)CNXMAN_COORD_RELAY ||
	       c->state == (uint8_t)CNXMAN_COORD_COMMIT ||
	       c->state == (uint8_t)CNXMAN_COORD_OPEN ||
	       c->state == (uint8_t)CNXMAN_COORD_BARRIER;
}

static int coord_is_participant(const struct cnxman_coord *c, uint32_t i)
{
	return (c->part_flags[i] & CNXMAN_COORD_P_PARTICIPANT) != 0u;
}

/* ==========================================================================
 * One dispatched message
 * ========================================================================== */
struct coord_msg {
	const uint8_t         *frame;
	uint32_t               len;
	struct vms_frame_info  fi;
	struct vms_cm_envelope env;
	int32_t                from_csb;
};

/* ==========================================================================
 * Getting a frame out
 *
 * Both directions go through the connection manager's link. A refused link is a
 * refusal to transmit, never a zero-filled frame (INV-6).
 * ========================================================================== */

static int coord_link_out(struct cnxman_coord *c, vms_csid_t dst,
			  struct vms_cm_link *link, struct vms_cm_envelope *env)
{
	coord_bzero(link, (uint32_t)sizeof(*link));
	coord_bzero(env, (uint32_t)sizeof(*env));
	if (c->link == NULL || c->link->next_out == NULL)
		return -1;
	return c->link->next_out(c->link->ctx, dst, link, env);
}

static void coord_note_send_failure(struct cnxman_coord *c, const char *why)
{
	c->send_failures++;
	coord_log(c, why);
}

static void coord_emit(struct cnxman_coord *c, vms_csid_t dst, uint32_t len)
{
	if (c->ops != NULL && c->ops->send != NULL)
		(void)c->ops->send(c->ops->ctx, dst, c->scratch, len);
}

static void coord_emit_response(struct cnxman_coord *c, uint32_t len)
{
	if (c->ops != NULL && c->ops->respond != NULL)
		(void)c->ops->respond(c->ops->ctx, c->scratch, len);
}

/*
 * Prepare to originate one frame to the CSB at `i`: resolve its LEARNED CSID
 * and pull the envelope counters. Nonzero means nothing may be sent -- an
 * unlearned CSID is "not yet known", never "node zero" (INV-6).
 */
static int coord_out_to(struct cnxman_coord *c, uint32_t i, vms_csid_t *dst,
			struct vms_cm_link *link, struct vms_cm_envelope *env)
{
	struct vms_csb *csb = &coord_club(c)->csb[i];

	if (!csb->in_use || !csb->csid_valid) {
		coord_note_send_failure(c,
			"%CNXMAN, a system in the transition has no cluster "
			"system id; nothing sent to it");
		return -1;
	}
	*dst = csb->csid;
	if (coord_link_out(c, *dst, link, env) != 0) {
		coord_note_send_failure(c,
			"%CNXMAN, no connection to a system in the transition; "
			"nothing sent to it");
		return -1;
	}
	return 0;
}

/* ==========================================================================
 * THE SELECTION PREDICATE (header SS "THE SELECTION PREDICATE -- INFERRED")
 * ========================================================================== */

static enum cnxman_coord_verdict coord_refuse(struct cnxman_coord *c,
					      enum cnxman_coord_refusal why,
					      const char *msg)
{
	c->last_refusal = (uint8_t)why;
	c->refusals++;
	coord_log(c, msg);
	return CNXMAN_COORD_REFUSE;
}

enum cnxman_coord_verdict cnxman_coord_select(struct cnxman_coord *c,
					      enum cnxman_coord_trigger trig,
					      int32_t subject_csb)
{
	struct vms_club *club;

	if (c == NULL || c->cl == NULL)
		return CNXMAN_COORD_REFUSE;
	c->last_refusal = (uint8_t)CNXMAN_COORD_REF_NONE;
	club = coord_club(c);

	/*
	 * A node that has not LEARNED its own CSID cannot name itself in a
	 * nodemap and must not drive anything. The predecessor of this stack
	 * defaulted the local CSID to 1 from an insmod parameter and became a
	 * phantom cluster of one; nothing here has a default.
	 */
	if (!club->local_csid_valid)
		return coord_refuse(c, CNXMAN_COORD_REF_NOT_MEMBER,
			"%CNXMAN, this node has no cluster system id; it "
			"cannot coordinate a state transition");

	/* One at a time from this node. */
	if (coord_is_active(c))
		return coord_refuse(c, CNXMAN_COORD_REF_BUSY,
			"%CNXMAN, already coordinating a state transition");

	/*
	 * THE COORDINATOR LOCK, local half. Book p. 7-30: start a transition
	 * "only if no other Connection Manager has already instituted" one;
	 * p. 7-32: a system already granted to another refuses, and a collision
	 * backs off a random short interval. `transition_active` with
	 * `we_coordinate` clear is exactly "we have granted it to someone else"
	 * -- real state, set when this node acknowledged a peer's Phase 1.
	 */
	if (club->transition_active && !club->we_coordinate)
		return CNXMAN_COORD_BACKOFF;

	/* The subject has to be a system this node actually holds a CSB for. */
	if (coord_csb_at(c, subject_csb) == NULL)
		return coord_refuse(c, CNXMAN_COORD_REF_NO_SUBJECT,
			"%CNXMAN, no system block for the subject of the "
			"proposed state transition");

	/*
	 * And that is the whole predicate. There is deliberately NO ordering
	 * rule here -- no highest node number, no SCSSYSTEMID comparison. For a
	 * JOIN the joiner already chose (book pp. 7-37/7-38), and receiving its
	 * op 0x02 IS the choice; for a DEPARTURE this node is the first
	 * detector (p. 7-2) and the test above is p. 7-30's condition. Design
	 * SS3.7's "OVMX never claims the role unprompted" holds structurally:
	 * there is no code path in this file that elects this node.
	 */
	(void)trig;
	return CNXMAN_COORD_DRIVE;
}

/*
 * Book p. 7-32's "random short interval", derived rather than randomised: see
 * the header's CNXMAN_COORD_BACKOFF_* note. Deterministic under an injected
 * clock, decorrelated between nodes by SCSSYSTEMID.
 */
static uint32_t coord_backoff_ms(const struct cnxman_coord *c)
{
	uint64_t id = c->cl->params.scssystemid;
	uint32_t mix = coord_now(c) ^ (uint32_t)id ^ (uint32_t)(id >> 16);

	return CNXMAN_COORD_BACKOFF_BASE_MS +
	       (mix % CNXMAN_COORD_BACKOFF_SPAN_MS);
}

static void coord_enter_backoff(struct cnxman_coord *c,
				enum cnxman_coord_trigger trig,
				int32_t subject_csb)
{
	uint32_t ms = coord_backoff_ms(c);

	c->state = (uint8_t)CNXMAN_COORD_DEFER;
	c->pending_trigger = (uint8_t)trig;
	c->pending_subject_csb = subject_csb;
	c->backoff_due_ms = coord_now(c) + ms;
	c->deferrals++;
	coord_log(c, "%CNXMAN, another system is coordinating a state "
		     "transition; deferring");
	if (c->ops != NULL && c->ops->arm_timer != NULL)
		c->ops->arm_timer(c->ops->ctx, CNXMAN_TIMER_COORD, 0u, ms);
}

/* ==========================================================================
 * CSID ASSIGNMENT -- the one thing only a coordinator does
 *
 * Book p. 7-25, in full and implemented as written: the CSID is
 * `(sequence << 16) | index`, the index is a Cluster System Vector slot, SLOT 0
 * IS NEVER USED, the sequence starts at 1 and increments only on REUSE of a
 * slot, slots are handed out round-robin, and a rejoining system gets a NEW CSB
 * and a NEW CSID -- never its old one back.
 *
 * The vector itself is not a structure this node keeps: OVMX learns CSIDs, so
 * the occupied slots ARE the CSIDs of the CSBs it holds. The next slot is one
 * past the highest ever seen, which is why a vacated slot is not reused -- and
 * the wire agrees: spec SS4(p) watches one node rejoin three times taking slots
 * 3, 4 and 5, with bitmaps 0x0a -> 0x12 -> 0x22.
 * ========================================================================== */

static void coord_seed_max_slot(struct cnxman_coord *c)
{
	struct vms_club *club = coord_club(c);
	uint32_t i;

	for (i = 0; i < club->n_csb; i++) {
		uint32_t slot;

		if (!club->csb[i].in_use || !club->csb[i].csid_valid)
			continue;
		slot = (uint32_t)(club->csb[i].csid & 0xffffu);
		if (slot > c->max_slot_seen)
			c->max_slot_seen = slot;
	}
}

/*
 * The slot a joiner WOULD get. Returns 0 when the next one falls outside the
 * membership bitmap byte the wire has actually grounded -- spec SS4(p): "one
 * byte holds only 8 slots while the library already reaches slot 5 ... Do not
 * assume 8 slots." A transition whose nodemap cannot name every member is
 * REFUSED, not opened.
 *
 * Deliberately separate from the assignment below: nothing is stamped on a CSB
 * until the whole transition is known to be expressible, so a refusal leaves no
 * half-assigned identity behind.
 */
static uint32_t coord_next_slot(struct cnxman_coord *c)
{
	uint32_t slot;

	coord_seed_max_slot(c);
	slot = c->max_slot_seen + 1u;
	if (slot == 0u)
		slot = 1u;   /* CSV slot 0 is never used (p. 7-25) */
	if (slot >= CNXMAN_PHASE2_BITMAP_SLOTS)
		return 0u;
	return slot;
}

/* Stamp it. Sequence 1: this slot has never been used, and this implementation
 * never reuses one, so no higher sequence can arise here (p. 7-25). */
static void coord_assign_slot(struct cnxman_coord *c, struct vms_csb *subject,
			      uint32_t slot)
{
	cnxman_csb_set_csid(subject, (vms_csid_t)((1u << 16) | slot));
	c->max_slot_seen = slot;
	c->csids_assigned++;
}

/* ==========================================================================
 * THE NODEMAP -- built from real CSBs, never from a received frame
 *
 * Spec SS4(p): `popcount(body[55])` equals the post-transition member count in
 * 54 of 54 opens with zero residuals, bit k is the member holding CSID index k,
 * and bit 0 is never set. So the coordinator's map is exactly "one bit per
 * system that will be in the cluster, at its own CSV slot" -- and if any of
 * those slots does not fit the grounded byte the open is REFUSED, because an
 * open that silently drops a member is how a barrier ends up permanently one
 * short.
 * ========================================================================== */

static int coord_set_slot_bit(uint8_t *map, const struct vms_csb *csb)
{
	uint32_t slot;

	if (!csb->csid_valid)
		return -1;
	slot = (uint32_t)(csb->csid & 0xffffu);
	if (slot == 0u || slot >= CNXMAN_PHASE2_BITMAP_SLOTS)
		return -1;
	*map |= (uint8_t)(1u << slot);
	return 0;
}

/*
 * `extra_slot` is the slot the joiner is ABOUT to be given (0 for none). It is
 * passed in rather than read off the subject's CSB because the assignment is
 * only made once this map is known to hold -- see coord_next_slot().
 */
static int coord_build_nodemap(struct cnxman_coord *c, uint32_t extra_slot,
			       uint8_t *out)
{
	struct vms_club *club = coord_club(c);
	struct vms_csb *local = cnxman_club_local(club);
	uint8_t map = 0u;
	uint32_t i;

	if (local == NULL || coord_set_slot_bit(&map, local) != 0)
		return -1;
	for (i = 0; i < club->n_csb; i++) {
		if (!coord_is_participant(c, i))
			continue;
		if ((int32_t)i == c->subject_csb && extra_slot != 0u)
			continue;   /* its slot is `extra_slot`, below */
		if (coord_set_slot_bit(&map, &club->csb[i]) != 0)
			return -1;
	}
	if (extra_slot != 0u) {
		if (extra_slot >= CNXMAN_PHASE2_BITMAP_SLOTS)
			return -1;
		map |= (uint8_t)(1u << extra_slot);
	}
	*out = map;
	return 0;
}

/* ==========================================================================
 * THE CENSUS -- M, frozen
 * ========================================================================== */

/*
 * Freeze the participant set. p. 7-49: the members are "the total number of
 * CSBs whose SELECTED flags are set". For an ADD the joiner is not SELECTED yet
 * (that is what this transition is FOR) and is added explicitly; for a REMOVE
 * the departing system is excluded, because a departed node cannot answer a
 * barrier step and waiting for it is precisely how a transition times out.
 *
 * Returns M-1: the number of systems this coordinator owes twelve releases.
 */
static uint32_t coord_freeze_participants(struct cnxman_coord *c)
{
	struct vms_club *club = coord_club(c);
	uint32_t i, n = 0u;

	coord_bzero(c->part_flags, (uint32_t)sizeof(c->part_flags));
	coord_bzero(c->part_step, (uint32_t)sizeof(c->part_step));

	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];

		if (!csb->in_use || (csb->flags & VMS_CSB_F_LOCAL) != 0u)
			continue;
		if ((int32_t)i == c->subject_csb)
			continue;   /* handled below, per class */
		if ((csb->flags & VMS_CSB_F_SELECTED) == 0u)
			continue;
		if (!csb->csid_valid)
			continue;
		c->part_flags[i] = CNXMAN_COORD_P_PARTICIPANT;
		n++;
	}

	if (c->subject_csb >= 0) {
		uint32_t s = (uint32_t)c->subject_csb;

		c->part_flags[s] |= CNXMAN_COORD_P_SUBJECT;
		if (c->tr_class == VMS_CM_CLASS_ADD) {
			c->part_flags[s] |= CNXMAN_COORD_P_PARTICIPANT;
			n++;
		}
	}
	c->n_participants = n;
	return n;
}

/* Has every frozen participant acknowledged the Phase 1 proposal (p. 7-41)? */
static int coord_all_phase1_acked(const struct cnxman_coord *c)
{
	uint32_t i;

	for (i = 0; i < c->cl->club.n_csb; i++) {
		if (!coord_is_participant(c, i))
			continue;
		if ((c->part_flags[i] & CNXMAN_COORD_P_PHASE1_ACK) == 0u)
			return 0;
	}
	return 1;
}

/*
 * Has every OTHER member confirmed our relay? The subject is excluded: the
 * relay tells the established members about the joiner, so the joiner is not
 * one of its recipients (spec SS4(O.31)). A two-node cluster therefore has an
 * empty relay set and reaches the commit immediately, which is exactly what the
 * reference two-node join does.
 */
static int coord_all_relayed(const struct cnxman_coord *c)
{
	uint32_t i;

	for (i = 0; i < c->cl->club.n_csb; i++) {
		if (!coord_is_participant(c, i))
			continue;
		if ((int32_t)i == c->subject_csb)
			continue;
		if ((c->part_flags[i] & CNXMAN_COORD_P_RELAY_ACK) == 0u)
			return 0;
	}
	return 1;
}

/* Has every frozen participant reported step `step`? THE gate on a release. */
static int coord_all_reported(const struct cnxman_coord *c, uint32_t step)
{
	uint32_t i;

	for (i = 0; i < c->cl->club.n_csb; i++) {
		if (!coord_is_participant(c, i))
			continue;
		if ((uint32_t)c->part_step[i] < step)
			return 0;
	}
	return 1;
}

uint32_t cnxman_coord_expected_releases(const struct cnxman_coord *c)
{
	if (c == NULL || !coord_is_active(c))
		return 0u;
	return CNXMAN_COORD_BARRIER_STEPS * c->n_participants;
}

/* ==========================================================================
 * The DLM transition callbacks
 * ========================================================================== */

static void coord_fill_transition(const struct cnxman_coord *c,
				  struct cnxman_transition *tr)
{
	coord_bzero(tr, (uint32_t)sizeof(*tr));
	tr->epoch = c->epoch;
	tr->tr_class = c->tr_class;
	tr->barrier_step = c->step;
	tr->we_coordinate = 1u;   /* this file is the COORDINATOR side */
	tr->coordinator_valid = c->cl->club.local_csid_valid;
	tr->coordinator_csid = c->cl->club.local_csid;
	if (c->subject_csb >= 0) {
		const struct vms_csb *s = &c->cl->club.csb[c->subject_csb];

		/* Unlike the participant side, the coordinator always knows the
		 * subject -- it is the system it was asked about. */
		if (s->in_use && s->csid_valid) {
			tr->subject_csid = s->csid;
			tr->subject_csid_valid = 1u;
		}
	}
}

static void coord_dlm_begin(struct cnxman_coord *c)
{
	struct cnxman_transition tr;

	if (c->dlm == NULL || c->dlm->transition_begin == NULL)
		return;
	coord_fill_transition(c, &tr);
	c->dlm->transition_begin(c->dlm->ctx, &tr);
}

static void coord_dlm_end(struct cnxman_coord *c, int completed)
{
	struct cnxman_transition tr;

	if (c->dlm == NULL || c->dlm->transition_end == NULL)
		return;
	coord_fill_transition(c, &tr);
	c->dlm->transition_end(c->dlm->ctx, &tr, completed);
}

static uint32_t coord_rebuild_outstanding(const struct cnxman_coord *c)
{
	if (c->rebuild == NULL || c->rebuild->outstanding == NULL)
		return 0u;
	return c->rebuild->outstanding(c->rebuild->ctx);
}

/* ==========================================================================
 * ORIGINATION -- one small sender per grounded opcode
 * ========================================================================== */

static void coord_send_relay(struct cnxman_coord *c, uint32_t i)
{
	struct vms_cm_link link;
	struct vms_cm_envelope env;
	vms_csid_t dst;
	uint32_t written = 0;

	if (coord_out_to(c, i, &dst, &link, &env) != 0)
		return;
	if (vms_cm_relay_build(&link, &env, c->tr_class, c->epoch, c->scratch,
			       (uint32_t)sizeof(c->scratch), &written) !=
	    VMS_CODEC_OK) {
		coord_note_send_failure(c,
			"%CNXMAN, transition relay could not be built");
		return;
	}
	coord_emit(c, dst, written);
	c->relays_sent++;
	/* The subject's identity has no grounded offset in this body (codec
	 * header sec 5b). Counted, so the gap is visible rather than guessed. */
	c->relay_subject_omitted++;
}

static void coord_send_commit(struct cnxman_coord *c, uint32_t i)
{
	struct vms_cm_link link;
	struct vms_cm_envelope env;
	vms_csid_t dst;
	uint32_t written = 0;

	if (coord_out_to(c, i, &dst, &link, &env) != 0)
		return;
	if (vms_cm_commit_build(&link, &env, c->tr_class, c->epoch, c->scratch,
				(uint32_t)sizeof(c->scratch), &written) !=
	    VMS_CODEC_OK) {
		coord_note_send_failure(c,
			"%CNXMAN, membership commit could not be built");
		return;
	}
	coord_emit(c, dst, written);
	c->commits_sent++;
}

static void coord_send_open(struct cnxman_coord *c, uint32_t i)
{
	struct vms_cm_link link;
	struct vms_cm_envelope env;
	vms_csid_t dst;
	uint32_t written = 0;

	if (coord_out_to(c, i, &dst, &link, &env) != 0)
		return;
	if (vms_cm_xition_open_build(&link, &env, c->tr_class, c->epoch,
				     c->bitmap, (int)c->bitmap_valid,
				     c->scratch, (uint32_t)sizeof(c->scratch),
				     &written) != VMS_CODEC_OK) {
		coord_note_send_failure(c,
			"%CNXMAN, transition proposal could not be built");
		return;
	}
	coord_emit(c, dst, written);
	c->opens_sent++;
	/* Book p. 7-40's proposed quorum / votes / foundation time / founder /
	 * rebuild type ride in bytes no capture has isolated (codec header
	 * sec 5b). They go out zero and the omission is counted, per open. */
	c->open_cells_omitted++;
}

static void coord_send_go(struct cnxman_coord *c, uint32_t i)
{
	struct vms_cm_link link;
	struct vms_cm_envelope env;
	vms_csid_t dst;
	uint32_t written = 0;

	if (coord_out_to(c, i, &dst, &link, &env) != 0)
		return;
	if (vms_cm_go_build(&link, &env, c->tr_class, c->epoch, c->scratch,
			    (uint32_t)sizeof(c->scratch), &written) !=
	    VMS_CODEC_OK) {
		coord_note_send_failure(c,
			"%CNXMAN, transition commit could not be built");
		return;
	}
	coord_emit(c, dst, written);
	c->gos_sent++;
}

static void coord_send_release(struct cnxman_coord *c, uint32_t i, uint32_t step)
{
	struct vms_cm_link link;
	struct vms_cm_envelope env;
	vms_csid_t dst;
	uint32_t written = 0;

	if (coord_out_to(c, i, &dst, &link, &env) != 0)
		return;
	if (vms_cm_release_build(&link, &env, c->epoch, step, c->scratch,
				 (uint32_t)sizeof(c->scratch), &written) !=
	    VMS_CODEC_OK) {
		coord_note_send_failure(c,
			"%CNXMAN, barrier release could not be built");
		return;
	}
	coord_emit(c, dst, written);
	c->releases_sent++;
}

/* Fan one origination out to every frozen participant. The ONLY way this file
 * addresses the cluster, so "the coordinator runs the same dialogue with every
 * member" (spec SS4(p)) is a shape, not a rule to remember. */
typedef void (*coord_fanout_fn)(struct cnxman_coord *, uint32_t);

static void coord_fanout(struct cnxman_coord *c, coord_fanout_fn fn)
{
	uint32_t i;

	for (i = 0; i < c->cl->club.n_csb; i++) {
		if (coord_is_participant(c, i))
			fn(c, i);
	}
}

/* ==========================================================================
 * PHASE 2 -- p. 7-42, the same four tasks every system runs
 * ========================================================================== */

/*
 * p. 7-46: a removed member has MEMBER cleared and REMOVED set. Done here from
 * the real CSB, before the shared count runs, so the count that commits is the
 * post-removal one.
 */
static void coord_retire_subject(struct cnxman_coord *c)
{
	struct vms_csb *s = coord_csb_at(c, c->subject_csb);

	if (s == NULL)
		return;
	cnxman_csb_clear_flags(s, (uint16_t)(VMS_CSB_F_SELECTED |
					     VMS_CSB_F_MEMBER));
	cnxman_csb_set_flags(s, (uint16_t)VMS_CSB_F_REMOVED);
}

static void coord_commit_phase2(struct cnxman_coord *c)
{
	struct cnxman_phase2_in in;
	struct cnxman_phase2_stats st;

	if (c->tr_class == VMS_CM_CLASS_REMOVE)
		coord_retire_subject(c);

	in.bitmap = c->bitmap;
	in.bitmap_valid = c->bitmap_valid;
	in.bitmap_popcount = c->bitmap_popcount;
	in.pad = 0u;

	(void)cnxman_phase2_commit(c->cl, &in, &st, c->ops);

	c->nodemap_unmapped += st.nodemap_unmapped;
	c->count_mismatch += st.count_mismatch;
	c->bitmap_short += st.bitmap_short;
	c->m_above_grounded += st.m_above_grounded;
	c->phase2_committed = 1u;
}

/* ==========================================================================
 * The phase drivers
 * ========================================================================== */

static void coord_claim_club(struct cnxman_coord *c)
{
	struct vms_club *club = coord_club(c);

	/*
	 * The epoch is THIS node's own, advanced. Spec SS4(r) grounds only that
	 * it is monotone (a capture runs 3, 4, 6, 7, 9, 11); the increment rule
	 * is not published, so the honest choice is the smallest step that
	 * preserves the one property that IS grounded, taken from the CLUB's
	 * real epoch -- never a constant and never a counter of our own.
	 */
	c->epoch = club->epoch + 1u;
	club->epoch = c->epoch;
	club->transition_active = 1u;
	club->transition_class = c->tr_class;
	club->we_coordinate = 1u;
	club->barrier_step = 0u;
	club->coordinator_valid = club->local_csid_valid;
	club->coordinator_csid = club->local_csid;
}

static void coord_release_club(struct cnxman_coord *c)
{
	struct vms_club *club = coord_club(c);

	club->transition_active = 0u;
	club->we_coordinate = 0u;
}

static void coord_try_go(struct cnxman_coord *c);

static void coord_enter_open(struct cnxman_coord *c)
{
	c->state = (uint8_t)CNXMAN_COORD_OPEN;
	coord_fanout(c, coord_send_open);
	/*
	 * A transition with NOBODY to propose to still has to commit: the last
	 * two-node cluster losing a member reconfigures entirely locally, with
	 * 12 x (M-1) = 0 frames. Waiting for an acknowledgement from an empty
	 * set would hang this node in Phase 1 forever.
	 */
	coord_try_go(c);
}

static void coord_enter_commit(struct cnxman_coord *c)
{
	if (c->tr_class != VMS_CM_CLASS_ADD || c->subject_csb < 0) {
		coord_enter_open(c);
		return;
	}
	c->state = (uint8_t)CNXMAN_COORD_COMMIT;
	coord_send_commit(c, (uint32_t)c->subject_csb);
}

/* Release step N to every frozen participant. */
static void coord_release_step_to_all(struct cnxman_coord *c, uint32_t step)
{
	uint32_t i;

	for (i = 0; i < c->cl->club.n_csb; i++) {
		if (coord_is_participant(c, i))
			coord_send_release(c, i, step);
	}
}

/* Release #12: the transition is over, and p. 7-42's "the coordinator releases
 * its coordinator lock" is this. */
static void coord_finish(struct cnxman_coord *c)
{
	c->state = (uint8_t)CNXMAN_COORD_COMPLETE;
	c->cl->club.barrier_step = (uint8_t)CNXMAN_COORD_BARRIER_STEPS;
	c->cl->club.reformations++;
	coord_release_club(c);
	c->transitions_completed++;
	coord_dlm_end(c, 1);
	coord_log(c, "%CNXMAN, completed VAXcluster state transition");
}

/*
 * Release step N to everybody, then advance -- for as long as the gate stays
 * open. Called from exactly two places (a step report and the beat), and it is
 * the ONLY caller of coord_release_step_to_all, which is what makes "0x0c#N
 * never precedes the last 0x0b#N" unrepresentable rather than merely intended.
 *
 * The loop matters only in the degenerate case: with one or more participants
 * the very next check fails (nobody has reported N+1 yet) and it runs exactly
 * once, exactly as a step-by-step version would. With NO participants -- the
 * last two-node cluster removing its peer -- it walks all twelve steps here,
 * because there is nobody left to report and nothing left to wait for.
 */
static void coord_try_release(struct cnxman_coord *c)
{
	while (c->state == (uint8_t)CNXMAN_COORD_BARRIER &&
	       coord_all_reported(c, (uint32_t)c->step)) {
		/*
		 * Spec SS4(p): the coordinator interleaves lock-rebuild records
		 * with the barrier "and gates the next step on them being
		 * answered" -- five unanswered ones froze a real barrier at
		 * step 5. Holding is correct; the beat re-tries.
		 */
		if (coord_rebuild_outstanding(c) != 0u) {
			c->rebuild_holds++;
			return;
		}
		coord_release_step_to_all(c, (uint32_t)c->step);
		if ((uint32_t)c->step >= CNXMAN_COORD_BARRIER_STEPS) {
			coord_finish(c);
			return;
		}
		c->step = (uint8_t)(c->step + 1u);
		c->cl->club.barrier_step = c->step;
	}
}

static void coord_try_go(struct cnxman_coord *c)
{
	if (c->state != (uint8_t)CNXMAN_COORD_OPEN)
		return;
	if (!coord_all_phase1_acked(c))
		return;

	/* PHASE 2, the point of no return (p. 7-42). The GO is never answered
	 * (spec SS4(p)), so nothing is awaited here. */
	coord_fanout(c, coord_send_go);
	coord_commit_phase2(c);

	c->state = (uint8_t)CNXMAN_COORD_BARRIER;
	c->step = 1u;
	c->cl->club.barrier_step = 1u;
	/* No-op with participants (nobody has reported step 1 yet); with none,
	 * this is where the degenerate transition completes. */
	coord_try_release(c);
}

static void coord_try_commit(struct cnxman_coord *c)
{
	if (c->state != (uint8_t)CNXMAN_COORD_RELAY)
		return;
	/*
	 * Spec SS4(O.31), decoded from a real-VAX readmission: "the member does
	 * NOT commit the returner until it has relayed the join to the other
	 * member and heard back" -- the Rule of Total Connectivity, p. 7-39.
	 * The relay set excludes the subject, which is why a two-node cluster
	 * reaches the commit with nothing to wait for.
	 */
	if (!coord_all_relayed(c))
		return;
	coord_enter_commit(c);
}

/* ==========================================================================
 * Opening a transition
 * ========================================================================== */

static void coord_abandon_internal(struct cnxman_coord *c, const char *why)
{
	if (!coord_is_active(c))
		return;
	c->state = (uint8_t)CNXMAN_COORD_ABANDONED;
	coord_release_club(c);
	c->transitions_abandoned++;
	coord_dlm_end(c, 0);
	coord_log(c, why);
}

/*
 * Common opening for both classes: freeze the census, build the nodemap, and
 * only then commit anything. Returns 0, or nonzero when the transition CANNOT
 * be expressed on the grounded wire -- in which case NOTHING has happened: no
 * frame sent, no CLUB claimed, no identity stamped on a CSB.
 *
 * `subject_slot` is the CSV slot a joiner is about to receive (0 for a
 * removal). The assignment happens here, after the map holds, so a refusal
 * cannot leave a half-admitted system carrying a CSID the cluster never saw.
 */
static int coord_open_transition(struct cnxman_coord *c, uint8_t tr_class,
				 int32_t subject_csb, uint32_t subject_slot)
{
	c->tr_class = tr_class;
	c->subject_csb = subject_csb;
	c->step = 0u;
	c->phase2_committed = 0u;
	c->bitmap = 0u;
	c->bitmap_valid = 0u;
	c->bitmap_popcount = 0u;

	(void)coord_freeze_participants(c);

	/* Only the class-0x02 ADD open carries a nodemap (spec SS4(p)). */
	if (tr_class == VMS_CM_CLASS_ADD) {
		struct vms_csb *subject = coord_csb_at(c, subject_csb);
		uint8_t map = 0u;

		if (subject == NULL ||
		    coord_build_nodemap(c, subject_slot, &map) != 0) {
			(void)coord_refuse(c, CNXMAN_COORD_REF_NO_NODEMAP,
				"%CNXMAN, a system's cluster system id falls "
				"outside the membership map this protocol can "
				"express; transition not proposed");
			return -1;
		}
		c->bitmap = map;
		c->bitmap_valid = 1u;
		c->bitmap_popcount = (uint8_t)cnxman_phase2_popcount8(map);
		/* Book p. 7-25: a rejoining system gets a NEW CSID, never its
		 * old one back -- so any csid already on this CSB is replaced. */
		coord_assign_slot(c, subject, subject_slot);
	}

	coord_claim_club(c);
	c->transitions_driven++;
	coord_dlm_begin(c);
	return 0;
}

/* The op 0x12 relay goes to the other MEMBERS, never to the joiner: it is how
 * they confirm connectivity WITH the joiner (spec SS4(O.31), book p. 7-39). */
static void coord_fanout_relay(struct cnxman_coord *c)
{
	uint32_t i;

	for (i = 0; i < c->cl->club.n_csb; i++) {
		if (!coord_is_participant(c, i))
			continue;
		if ((int32_t)i == c->subject_csb)
			continue;
		coord_send_relay(c, i);
	}
}

static void coord_begin_add(struct cnxman_coord *c, int32_t subject_csb)
{
	uint32_t slot = coord_next_slot(c);

	if (slot == 0u) {
		(void)coord_refuse(c, CNXMAN_COORD_REF_NO_SLOT,
			"%CNXMAN, no cluster system vector slot this protocol "
			"can name; membership request not proposed");
		return;
	}
	if (coord_open_transition(c, VMS_CM_CLASS_ADD, subject_csb, slot) != 0)
		return;

	coord_log(c, "%CNXMAN, proposing addition of a system to the cluster");
	c->state = (uint8_t)CNXMAN_COORD_RELAY;
	coord_fanout_relay(c);
	/*
	 * op 0x05 (lock-rebuild burst) and op 0x06 (MEMBERSHIP burst) belong
	 * between the commit and the open in the reference sequence. Neither is
	 * built here: the membership record's {SCSSYSTEMID, incarnation, CSID}
	 * triple has no isolated offset, so a burst would assert an empty
	 * cluster. The joiner therefore is not told the CSID assigned above.
	 * Counted, said on the console once per transition, never faked.
	 */
	c->membership_burst_omitted++;
	coord_log(c, "%CNXMAN, membership records omitted: their format is not "
		     "established");

	coord_try_commit(c);
}

static void coord_begin_remove(struct cnxman_coord *c, int32_t subject_csb)
{
	if (coord_open_transition(c, VMS_CM_CLASS_REMOVE, subject_csb, 0u) != 0)
		return;
	/*
	 * A class-0x03 removal has no op 0x02, no relay and no commit: it opens
	 * directly (spec SS4(r): op 0x08, tag 0x0340) and then runs the SAME
	 * twelve steps and the same 12 x (M-1) law (spec SS4(p)).
	 */
	coord_log(c, "%CNXMAN, proposing removal of a system from the cluster");
	coord_enter_open(c);
}

/* ==========================================================================
 * The handlers -- one per edge
 * ========================================================================== */

/*
 * [IDLE|COMPLETE|ABANDONED][RX_TR_REQUEST] -- a system's op 0x02.
 *
 * THIS IS THE SELECTION. Spec SS4(p): the joiner "sends its op 0x02 to EXACTLY
 * ONE peer, which relays the new node to the rest", and "a NON-COORDINATOR peer
 * SILENTLY DISCARDS op 0x02". Being asked is what makes this node the
 * coordinator (book pp. 7-37/7-38); there is nothing further to decide.
 */
static void coord_h_request(struct cnxman_coord *c, const struct coord_msg *m)
{
	enum cnxman_coord_verdict v;

	if (m->from_csb < 0) {
		/* We cannot admit a system we hold no block for, and we will
		 * not invent one. */
		c->unknown_peer++;
		return;
	}
	v = cnxman_coord_select(c, CNXMAN_COORD_TRIG_ASKED, m->from_csb);
	if (v == CNXMAN_COORD_BACKOFF) {
		coord_enter_backoff(c, CNXMAN_COORD_TRIG_ASKED, m->from_csb);
		return;
	}
	if (v != CNXMAN_COORD_DRIVE)
		return;   /* refused, logged and counted inside select() */
	coord_begin_add(c, m->from_csb);
}

/* Which participant slot did this frame come from? -1 when we cannot tell, and
 * then nothing is credited to anybody. */
static int32_t coord_participant_of(struct cnxman_coord *c,
				    const struct coord_msg *m)
{
	if (m->from_csb < 0 || !coord_is_participant(c, (uint32_t)m->from_csb)) {
		c->unknown_peer++;
		return -1;
	}
	return m->from_csb;
}

/* [RELAY][RX_TR_ACK] -- a member confirms connectivity with the subject. */
static void coord_h_relay_ack(struct cnxman_coord *c, const struct coord_msg *m)
{
	int32_t i = coord_participant_of(c, m);

	if (m->env.opcode != VMS_CM_OP_RELAY) {
		c->ignored_events++;
		return;
	}
	if (i < 0)
		return;
	c->part_flags[i] |= CNXMAN_COORD_P_RELAY_ACK;
	c->relay_acks++;
	coord_try_commit(c);
}

/* [COMMIT][RX_TR_ACK] -- the subject echoed its membership commit. */
static void coord_h_commit_ack(struct cnxman_coord *c, const struct coord_msg *m)
{
	if (m->env.opcode != VMS_CM_OP_COMMIT) {
		c->ignored_events++;
		return;
	}
	if (m->from_csb != c->subject_csb) {
		c->unknown_peer++;
		return;
	}
	c->commit_acks++;
	coord_enter_open(c);
}

/*
 * [OPEN][RX_TR_ACK] -- PHASE 1 acknowledged. p. 7-41: "each system normally
 * acknowledges to VAX_A that it has received and processed the information",
 * and the GO does not go out until every one of them has.
 */
static void coord_h_open_ack(struct cnxman_coord *c, const struct coord_msg *m)
{
	int32_t i;

	if (m->env.opcode != VMS_CM_OP_XITION_ADD &&
	    m->env.opcode != VMS_CM_OP_XITION_REM) {
		c->ignored_events++;
		return;
	}
	i = coord_participant_of(c, m);
	if (i < 0)
		return;
	c->part_flags[i] |= CNXMAN_COORD_P_PHASE1_ACK;
	c->open_acks++;
	coord_try_go(c);
}

/*
 * [BARRIER][RX_TR_ACK] -- a member answered an interleaved lock-rebuild record.
 * The count itself lives in the DLM (cnxman_coord_rebuild_ops); this edge only
 * re-tries the release the record may have been holding.
 */
static void coord_h_rebuild_ack(struct cnxman_coord *c,
				const struct coord_msg *m)
{
	(void)m;
	coord_try_release(c);
}

/* Answer one member's step with the coordinator's 0x81/0x0b (spec SS4(p): the
 * ack, NOT the release). A step never acknowledged is retransmitted forever. */
static void coord_ack_step(struct cnxman_coord *c, const struct coord_msg *m)
{
	struct vms_cm_link link;
	struct vms_cm_envelope env;
	struct vms_csb *csb = coord_csb_at(c, m->from_csb);
	uint32_t written = 0;

	if (csb == NULL || !csb->csid_valid)
		return;
	if (coord_link_out(c, csb->csid, &link, &env) != 0) {
		coord_note_send_failure(c,
			"%CNXMAN, no connection to acknowledge a barrier step");
		return;
	}
	if (vms_cm_step_ack_build(&link, m->frame, m->len, &env, c->scratch,
				  (uint32_t)sizeof(c->scratch), &written) !=
	    VMS_CODEC_OK) {
		coord_note_send_failure(c,
			"%CNXMAN, barrier step acknowledgement could not be "
			"built");
		return;
	}
	coord_emit_response(c, written);
	c->step_acks_sent++;
}

/*
 * [BARRIER][RX_BARRIER] -- one member's op-0x0b report of step N.
 *
 * THE CENSUS. Record it, acknowledge it, and then release step N to EVERYBODY
 * only once every frozen participant has reported it. A retransmission is
 * acknowledged again and counted separately; a report for a step we are not on,
 * or for another transition, is instrumented and credits nothing -- losing
 * count is how a barrier ends up releasing early or stopping short.
 */
static void coord_h_step(struct cnxman_coord *c, const struct coord_msg *m)
{
	struct vms_cm_barrier rep;
	int32_t i;

	if (vms_cm_barrier_parse(m->frame, m->len, &m->fi, &rep) !=
	    VMS_CODEC_OK) {
		c->ignored_events++;
		return;
	}
	i = coord_participant_of(c, m);
	if (i < 0)
		return;
	if (rep.epoch != c->epoch) {
		c->epoch_mismatch++;
		return;
	}
	if (rep.step != (uint32_t)c->step) {
		if (rep.step < (uint32_t)c->step) {
			/* Behind us: the member did not see our release yet.
			 * Answer it again (that is what a retransmission
			 * wants) and change nothing. */
			c->step_duplicates++;
			coord_ack_step(c, m);
			return;
		}
		c->step_out_of_order++;
		coord_log(c, "%CNXMAN, a system reported a barrier step ahead "
			     "of the one in progress");
		return;
	}

	c->steps_received++;
	if ((uint32_t)c->part_step[i] >= rep.step)
		c->step_duplicates++;
	else
		c->part_step[i] = (uint8_t)rep.step;
	coord_ack_step(c, m);
	coord_try_release(c);
}

/*
 * [RELAY|COMMIT|OPEN][RX_TR_OPEN or RX_TR_GO] -- ANOTHER connection manager
 * opened a transition while we were opening ours. Book p. 7-32: a system
 * already granted to another refuses, and the loser backs off.
 *
 * Before our own GO nothing is committed anywhere, so we drop ours and become a
 * participant in theirs -- and the frame is handed BACK (the dispatcher returns
 * NOT_MINE for it) so the participant FSM answers this very open. After our GO
 * the transition is past the point of no return (p. 7-42) and cannot be
 * abandoned: the collision is counted and we keep driving.
 */
static void coord_h_collision(struct cnxman_coord *c, const struct coord_msg *m)
{
	(void)m;
	c->collisions++;
	coord_abandon_internal(c,
		"%CNXMAN, another system is coordinating a state transition; "
		"abandoning ours");
}

static void coord_h_collision_committed(struct cnxman_coord *c,
					const struct coord_msg *m)
{
	(void)m;
	c->collisions++;
	coord_log(c, "%CNXMAN, a state transition opened while ours is "
		     "committed; ours stands");
}

/*
 * There is deliberately NO cell for a peer's transition ABORT (cat-0x01
 * op 0x04, role 0x50). It aborts THAT peer's transition, not ours -- ours is
 * abandoned through cnxman_coord_abandon() or by a collision edge -- and the
 * participant FSM already owns it as a CONSUME row. So it is never classified
 * as ours and routes straight on, which keeps one frame in exactly one FSM.
 */

/* ==========================================================================
 * The table. [state][event]; NULL = the evidence does not connect that event
 * to that state, so it is ignored and COUNTED rather than guessed.
 * ========================================================================== */
typedef void (*coord_handler_t)(struct cnxman_coord *, const struct coord_msg *);

static const coord_handler_t
coord_table[CNXMAN_COORD_STATE__COUNT][CNXMAN_EV__COUNT] = {
	/* [IDLE] a joiner's op 0x02 is the only thing that starts us. */
	[CNXMAN_COORD_IDLE] = {
		[CNXMAN_EV_RX_TR_REQUEST] = coord_h_request,
	},

	/* [DEFER] we were asked but the coordinator lock is held elsewhere; the
	 * back-off beat retries. A second request while deferring changes
	 * nothing -- it is the same joiner asking again. */
	[CNXMAN_COORD_DEFER] = {
		[CNXMAN_EV_RX_TR_REQUEST] = coord_h_request,
	},

	/* [RELAY] op 0x12 out; the members confirm connectivity with the
	 * subject before anything is committed (spec SS4(O.31), p. 7-39). */
	[CNXMAN_COORD_RELAY] = {
		[CNXMAN_EV_RX_TR_ACK]  = coord_h_relay_ack,
		[CNXMAN_EV_RX_TR_OPEN] = coord_h_collision,
		[CNXMAN_EV_RX_TR_GO]   = coord_h_collision,
	},

	/* [COMMIT] op 0x03 out to the subject. */
	[CNXMAN_COORD_COMMIT] = {
		[CNXMAN_EV_RX_TR_ACK]  = coord_h_commit_ack,
		[CNXMAN_EV_RX_TR_OPEN] = coord_h_collision,
		[CNXMAN_EV_RX_TR_GO]   = coord_h_collision,
	},

	/* [OPEN] PHASE 1: every participant must acknowledge before the GO. */
	[CNXMAN_COORD_OPEN] = {
		[CNXMAN_EV_RX_TR_ACK]  = coord_h_open_ack,
		[CNXMAN_EV_RX_TR_OPEN] = coord_h_collision,
		[CNXMAN_EV_RX_TR_GO]   = coord_h_collision,
	},

	/* [BARRIER] PHASE 2 committed; the 12 x (M-1) census is running. */
	[CNXMAN_COORD_BARRIER] = {
		[CNXMAN_EV_RX_BARRIER] = coord_h_step,
		[CNXMAN_EV_RX_TR_ACK]  = coord_h_rebuild_ack,
		[CNXMAN_EV_RX_TR_OPEN] = coord_h_collision_committed,
		[CNXMAN_EV_RX_TR_GO]   = coord_h_collision_committed,
	},

	/* [COMPLETE] / [ABANDONED] the next transition starts with its own
	 * request, exactly as from IDLE. */
	[CNXMAN_COORD_COMPLETE] = {
		[CNXMAN_EV_RX_TR_REQUEST] = coord_h_request,
	},
	[CNXMAN_COORD_ABANDONED] = {
		[CNXMAN_EV_RX_TR_REQUEST] = coord_h_request,
	},
};

/* ==========================================================================
 * Classification: which shared event is this frame?
 *
 * The table is indexed by MEANING, so an opcode re-assignment after a capture
 * is an edit here and nowhere else. Everything this file does not own returns
 * CNXMAN_EV__COUNT and the caller routes the frame on.
 * ========================================================================== */

static enum cnxman_event coord_event_of_response(const struct coord_msg *m)
{
	if (m->env.category == vms_wire_response_category(VMS_CM_CAT_DLM)) {
		/* A member answered an interleaved rebuild record. */
		if (m->env.opcode == VMS_CM_OP_DLM_REBUILD)
			return CNXMAN_EV_RX_TR_ACK;
		return CNXMAN_EV__COUNT;
	}
	switch (m->env.opcode) {
	case VMS_CM_OP_RELAY:       /* 0x81/0x12: connectivity confirmed  */
	case VMS_CM_OP_COMMIT:      /* 0x81/0x03: the subject committed   */
	case VMS_CM_OP_XITION_ADD:  /* 0x81/0x09: Phase 1 acknowledged    */
	case VMS_CM_OP_XITION_REM:  /* 0x81/0x08: ... of a removal        */
		return CNXMAN_EV_RX_TR_ACK;
	default:
		/* 0x81/0x0b is the COORDINATOR's own ack coming back at a
		 * participant -- FC-P3.5's, never ours. */
		return CNXMAN_EV__COUNT;
	}
}

static enum cnxman_event coord_event_of_config(const struct coord_msg *m,
					       int active)
{
	switch (m->env.opcode) {
	case VMS_CM_OP_CONFIG:      /* 0x02: a system asks to join        */
		return CNXMAN_EV_RX_TR_REQUEST;
	case VMS_CM_OP_BARRIER:     /* 0x0b: a member reports a step      */
		return active ? CNXMAN_EV_RX_BARRIER : CNXMAN_EV__COUNT;
	/*
	 * A peer's transition open / GO / abort is the PARTICIPANT's business
	 * unless we are driving one of our own, in which case it is a
	 * collision. Returning NOT_MINE when idle is what lets the glue route
	 * with one rule and never duplicates a frame into two FSMs.
	 */
	case VMS_CM_OP_XITION_ADD:
	case VMS_CM_OP_XITION_REM:
	case VMS_CM_OP_DEPART_XITION:
		return active ? CNXMAN_EV_RX_TR_OPEN : CNXMAN_EV__COUNT;
	case VMS_CM_OP_XITION_GO:
		return active ? CNXMAN_EV_RX_TR_GO : CNXMAN_EV__COUNT;
	default:
		return CNXMAN_EV__COUNT;
	}
}

static enum cnxman_event coord_event_of(const struct coord_msg *m, int active)
{
	if (vms_wire_is_response(m->env.category))
		return coord_event_of_response(m);
	if (m->env.category == VMS_CM_CAT_CONFIG)
		return coord_event_of_config(m, active);
	return CNXMAN_EV__COUNT;
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static enum cnxman_coord_rx coord_dispatch(struct cnxman_coord *c,
					   const struct coord_msg *m)
{
	int active = coord_is_active(c);
	enum cnxman_event ev = coord_event_of(m, active);
	coord_handler_t h;
	int was_collision;

	if (ev == CNXMAN_EV__COUNT)
		return CNXMAN_COORD_RX_NOT_MINE;
	if ((unsigned)c->state >= (unsigned)CNXMAN_COORD_STATE__COUNT)
		return CNXMAN_COORD_RX_BAD;

	h = coord_table[c->state][ev];
	if (h == NULL) {
		c->ignored_events++;
		return CNXMAN_COORD_RX_CONSUMED;
	}
	was_collision = (h == coord_h_collision) ||
			(h == coord_h_collision_committed);
	h(c, m);
	/*
	 * A collision frame is a PEER's transition message: after handing it to
	 * the collision edge (which drops ours if it can), it still has to be
	 * answered as a participant, so it goes back to the router. That is the
	 * hand-off to FC-P3.5, and it costs no extra round trip.
	 */
	return was_collision ? CNXMAN_COORD_RX_NOT_MINE
			     : CNXMAN_COORD_RX_CONSUMED;
}

enum cnxman_coord_rx cnxman_coord_rx_frame(struct cnxman_coord *c,
					   const uint8_t *frame, uint32_t len,
					   int32_t from_csb)
{
	struct coord_msg m;

	if (c == NULL || c->cl == NULL || frame == NULL)
		return CNXMAN_COORD_RX_BAD;
	if (vms_frame_classify(frame, len, &m.fi) != VMS_CODEC_OK)
		return CNXMAN_COORD_RX_BAD;
	if (vms_cm_envelope_parse(frame, len, &m.fi, &m.env) != VMS_CODEC_OK)
		return CNXMAN_COORD_RX_BAD;

	m.frame = frame;
	m.len = len;
	m.from_csb = from_csb;
	return coord_dispatch(c, &m);
}

/* ==========================================================================
 * The FC-P3.6 hand-off, the beat, connectivity loss, lifecycle, readback
 * ========================================================================== */

enum cnxman_coord_verdict cnxman_coord_propose_remove(struct cnxman_coord *c,
						      int32_t subject_csb)
{
	enum cnxman_coord_verdict v;

	if (c == NULL || c->cl == NULL)
		return CNXMAN_COORD_REFUSE;
	v = cnxman_coord_select(c, CNXMAN_COORD_TRIG_DETECTED, subject_csb);
	if (v == CNXMAN_COORD_BACKOFF) {
		coord_enter_backoff(c, CNXMAN_COORD_TRIG_DETECTED, subject_csb);
		return v;
	}
	if (v == CNXMAN_COORD_DRIVE)
		coord_begin_remove(c, subject_csb);
	return v;
}

static void coord_retry_deferred(struct cnxman_coord *c)
{
	int32_t subject = c->pending_subject_csb;
	uint8_t trig = c->pending_trigger;

	c->state = (uint8_t)CNXMAN_COORD_IDLE;
	c->pending_subject_csb = -1;
	if (trig == (uint8_t)CNXMAN_COORD_TRIG_DETECTED) {
		(void)cnxman_coord_propose_remove(c, subject);
		return;
	}
	/*
	 * An ADD that was deferred is NOT re-driven from here: the joiner
	 * retransmits its op 0x02 (spec SS4(o)) and being asked again is what
	 * re-selects this node. Manufacturing a join request for a system that
	 * has stopped asking would admit a node on our own say-so.
	 */
	coord_log(c, "%CNXMAN, ready to consider a membership request again");
}

void cnxman_coord_timer(struct cnxman_coord *c)
{
	if (c == NULL || c->cl == NULL)
		return;

	if (c->state == (uint8_t)CNXMAN_COORD_DEFER) {
		if ((int32_t)(coord_now(c) - c->backoff_due_ms) >= 0)
			coord_retry_deferred(c);
		return;
	}

	if (c->state == (uint8_t)CNXMAN_COORD_RELAY ||
	    c->state == (uint8_t)CNXMAN_COORD_COMMIT ||
	    c->state == (uint8_t)CNXMAN_COORD_OPEN) {
		/*
		 * A proposal nobody has answered. Book p. 7-41 lets the
		 * coordinator abandon only on a REJECTION or a connectivity
		 * loss, and FC-P3.6's reconnect loop is what turns a silent
		 * member into that loss -- so this counts and says so, and
		 * invents no timeout of its own.
		 */
		c->slow_phase1++;
		if (c->slow_phase1 == 1u)
			coord_log(c, "%CNXMAN, waiting for the systems in the "
				     "cluster to acknowledge a state transition");
	}

	if (c->state == (uint8_t)CNXMAN_COORD_BARRIER) {
		/* Spec SS4(p): a slow step is NOT a failure. Counting is the
		 * whole action -- plus one re-try, in case the release was
		 * being held for a rebuild record that has since drained. */
		c->slow_steps++;
		coord_try_release(c);
	}
	if (c->ops != NULL && c->ops->arm_timer != NULL && coord_is_active(c))
		c->ops->arm_timer(c->ops->ctx, CNXMAN_TIMER_BARRIER,
				  (uint32_t)c->step, CNXMAN_COORD_WATCH_MS);
}

void cnxman_coord_participant_lost(struct cnxman_coord *c, int32_t csb_index)
{
	if (c == NULL || c->cl == NULL || !coord_is_active(c))
		return;
	if (csb_index < 0 || (uint32_t)csb_index >= c->cl->club.n_csb)
		return;
	if (!coord_is_participant(c, (uint32_t)csb_index))
		return;

	if (c->state != (uint8_t)CNXMAN_COORD_BARRIER) {
		/* p. 7-41: the coordinator abandons on connectivity loss. */
		coord_abandon_internal(c,
			"%CNXMAN, lost connection to a system in the state "
			"transition; abandoning it");
		return;
	}
	/*
	 * p. 7-42: past the GO the transition "cannot be abandoned". Drop the
	 * departed system from the census instead, so the remaining members are
	 * released rather than held forever waiting on a node that is gone --
	 * which is the exact failure that times a transition out and drops the
	 * healthy members. Its own removal is a SEPARATE transition.
	 */
	c->part_flags[csb_index] &= (uint8_t)~CNXMAN_COORD_P_PARTICIPANT;
	if (c->n_participants > 0u)
		c->n_participants--;
	coord_log(c, "%CNXMAN, lost connection to a system during the barrier; "
		     "continuing without it");
	coord_try_release(c);
}

void cnxman_coord_abandon(struct cnxman_coord *c, const char *why)
{
	if (c == NULL || c->cl == NULL)
		return;
	coord_abandon_internal(c, why != NULL ? why :
		"%CNXMAN, aborting VAXcluster state transition");
}

void cnxman_coord_init(struct cnxman_coord *c, struct vms_cluster *cl,
		       const struct cnxman_ops *ops,
		       const struct cnxman_coord_link_ops *link)
{
	if (c == NULL)
		return;
	coord_bzero(c, (uint32_t)sizeof(*c));
	c->cl = cl;
	c->ops = ops;
	c->link = link;
	c->state = (uint8_t)CNXMAN_COORD_IDLE;
	c->subject_csb = -1;
	c->pending_subject_csb = -1;
}

void cnxman_coord_set_dlm(struct cnxman_coord *c,
			  const struct dlm_scs_role_ops *dlm)
{
	if (c != NULL)
		c->dlm = dlm;
}

void cnxman_coord_set_rebuild(struct cnxman_coord *c,
			      const struct cnxman_coord_rebuild_ops *rb)
{
	if (c != NULL)
		c->rebuild = rb;
}

int cnxman_coord_transition(const struct cnxman_coord *c,
			    struct cnxman_transition *out)
{
	if (c == NULL || out == NULL || !coord_is_active(c))
		return -1;
	coord_fill_transition(c, out);
	return 0;
}

const char *cnxman_coord_state_name(enum cnxman_coord_state s)
{
	switch (s) {
	case CNXMAN_COORD_IDLE:      return "idle";
	case CNXMAN_COORD_DEFER:     return "defer";
	case CNXMAN_COORD_RELAY:     return "relay";
	case CNXMAN_COORD_COMMIT:    return "commit";
	case CNXMAN_COORD_OPEN:      return "open";
	case CNXMAN_COORD_BARRIER:   return "barrier";
	case CNXMAN_COORD_COMPLETE:  return "complete";
	case CNXMAN_COORD_ABANDONED: return "abandoned";
	default:                     return "?";
	}
}

const char *cnxman_coord_verdict_name(enum cnxman_coord_verdict v)
{
	switch (v) {
	case CNXMAN_COORD_DRIVE:   return "drive";
	case CNXMAN_COORD_BACKOFF: return "backoff";
	case CNXMAN_COORD_REFUSE:  return "refuse";
	default:                   return "?";
	}
}
