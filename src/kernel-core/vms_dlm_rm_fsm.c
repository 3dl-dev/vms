// SPDX-License-Identifier: GPL-2.0
/*
 * vms_dlm_rm_fsm.c - THE NEW MASTER'S SIDE OF A LOCK-TREE REMASTER, as a table
 * (rd vms-ed5). The contract, the measured sequence, rules W1/W2 and the INV-6
 * argument are in vms_dlm_rm_fsm.h; this file is the machine.
 *
 * PURE on exactly the terms vms_dlm_scs_fsm.c is: no global, no clock, no
 * allocation, no substrate idiom, and the lock engine reached only through the
 * injected ops. Every cat-0x02 byte goes through the FC-P4.5 codec.
 *
 * INCLUDES: kernel-core headers only (CI gate
 * tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_dlm_rm_fsm.h"

/* ==========================================================================
 * 0. Byte-wise zero. This TU calls no library and no seam primitive -- the
 * substrate spells memset differently and a pure TU must build on the host too
 * (the same note vms_cnxman_csb.c's csb_bzero carries). Nothing here clears
 * more than one tree slot.
 * ========================================================================== */
static void rm_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0u; i < n; i++)
		b[i] = 0u;
}

/* ==========================================================================
 * 1. Small reads of the machine's own state
 * ========================================================================== */

/* Are all the doors there? A missing one makes every step REFUSED -- the
 * honest floor for a node that cannot reach its engine. */
static int rm_ops_ok(const struct vms_dlm_rm *f)
{
	const struct vms_dlm_rm_ops *o;

	if (f == (const struct vms_dlm_rm *)0)
		return 0;
	o = f->ops;
	return o != (const struct vms_dlm_rm_ops *)0 &&
	       o->other_members != 0 && o->adopt_open != 0 &&
	       o->adopt_lock != 0 && o->adopt_end != 0 && o->adopt_abort != 0 &&
	       o->adopt_tree_of_lkid != 0 && o->adopt_reported != 0 &&
	       o->name_mastered_here != 0 && o->dir_hash != 0 &&
	       o->stage_reply != 0 && o->send != 0 && o->residue_fill != 0 &&
	       o->log != 0;
}

static void rm_say(const struct vms_dlm_rm *f, const char *msg)
{
	if (f != (const struct vms_dlm_rm *)0 &&
	    f->ops != (const struct vms_dlm_rm_ops *)0 && f->ops->log != 0)
		f->ops->log(f->ops->ctx, msg);
}

/* Do two resource identities name the same resource? Group, access mode and
 * the name bytes -- the full identity, because "which resource of that name"
 * has no honest default (rd vms-b5b0). */
static int rm_ident_eq(const struct vms_dlm_res_ident *a,
		       const struct vms_dlm_res_ident *b)
{
	uint32_t i;

	if (a->group != b->group || a->mode != b->mode ||
	    a->name_len != b->name_len)
		return 0;
	for (i = 0u; i < a->name_len; i++) {
		if (a->name[i] != b->name[i])
			return 0;
	}
	return 1;
}

/* The identity, as the engine is asked about it: a C string plus group/mode. */
static void rm_ident_to_name(const struct vms_dlm_res_ident *id, char *out,
			     uint32_t cap)
{
	uint32_t n = id->name_len;
	uint32_t i;

	if (cap == 0u)
		return;
	if (n > cap - 1u)
		n = cap - 1u;
	for (i = 0u; i < n; i++)
		out[i] = (char)id->name[i];
	out[n] = '\0';
}

static struct vms_dlm_rm_tree *rm_slot_of(struct vms_dlm_rm *f,
					  const struct vms_dlm_res_ident *id,
					  uint8_t state)
{
	uint32_t i;

	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		if (f->tree[i].state == state &&
		    rm_ident_eq(&f->tree[i].id, id))
			return &f->tree[i];
	}
	return (struct vms_dlm_rm_tree *)0;
}

/* The slot for this identity in ANY non-free state. */
static struct vms_dlm_rm_tree *rm_slot_any(struct vms_dlm_rm *f,
					   const struct vms_dlm_res_ident *id)
{
	struct vms_dlm_rm_tree *t = rm_slot_of(f, id, VMS_DLM_RM_ST_OPEN);

	return t != (struct vms_dlm_rm_tree *)0 ?
		t : rm_slot_of(f, id, VMS_DLM_RM_ST_HELD);
}

/*
 * A SLOT TO OPEN A TREE IN.
 *
 *   1. THE SLOT THAT ALREADY NAMES THIS TREE, if it is HELD. A fresh 0x0e for a
 *      tree this node already holds is that tree MOVING AWAY AND COMING BACK,
 *      and it must reuse that slot: two slots naming one identity would make
 *      the op-0x0f role readback (vms_dlm_rm_is_new_master) answer off whichever
 *      one it reached first, and answering "I am not the new master" for a tree
 *      that IS open here is a wrong answer on the wire.
 *   2. a FREE slot.
 *   3. failing that, any HELD one -- a tree whose 0x14s have gone out has
 *      nothing outstanding, so reusing its slot loses only the ability to
 *      recognise a late answer to it (counted as an unmatched reply, never as
 *      an answer).
 *
 * A slot in OPEN state is never taken: that tree's holders are still sending
 * records.
 */
static struct vms_dlm_rm_tree *rm_slot_free(struct vms_dlm_rm *f,
					    const struct vms_dlm_res_ident *id)
{
	struct vms_dlm_rm_tree *t = rm_slot_of(f, id, VMS_DLM_RM_ST_HELD);
	uint32_t i;

	if (t != (struct vms_dlm_rm_tree *)0)
		return t;
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		if (f->tree[i].state == VMS_DLM_RM_ST_FREE)
			return &f->tree[i];
	}
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		if (f->tree[i].state == VMS_DLM_RM_ST_HELD)
			return &f->tree[i];
	}
	return (struct vms_dlm_rm_tree *)0;
}

/* ==========================================================================
 * 2. The owed set -- who still has an end-of-records to send (rule W1/W2)
 * ========================================================================== */

/*
 * SNAPSHOT THE OTHER MEMBERS into the tree's owed set. The CLUB is the only
 * honest source: the 0x0f and 0x11 that tell the other members are
 * point-to-point and this node never sees them (vms_dlm_rm_fsm.h).
 *
 * Returns 0 on a real snapshot. Non-zero means this node could not name the
 * membership it would have to wait for -- no member at all, or more than the
 * owed set is wide -- and then the tree is refused rather than half-waited for.
 */
static int rm_owed_snapshot(struct vms_dlm_rm *f, struct vms_dlm_rm_tree *t)
{
	uint32_t n = f->ops->other_members(f->ops->ctx, t->owed,
					   VMS_DLM_RM_MEMBERS);
	uint32_t i;

	if (n == 0u || n > VMS_DLM_RM_MEMBERS)
		return -1;
	for (i = 0u; i < n; i++) {
		if (t->owed[i] == 0u)
			return -1;   /* a member this node cannot address */
		t->done[i] = 0u;
	}
	t->n_owed = (uint8_t)n;
	t->n_done = 0u;
	return 0;
}

/* Mark `csid` as having sent its end-of-records. Returns 1 when it was owed
 * one and had not sent it, 0 otherwise (an unexpected or repeated 0x13). */
static int rm_owed_mark(struct vms_dlm_rm_tree *t, uint32_t csid)
{
	uint32_t i;

	for (i = 0u; i < t->n_owed; i++) {
		if (t->owed[i] != csid || t->done[i])
			continue;
		t->done[i] = VMS_DLM_RM_OWED_REPORTED;
		t->n_done++;
		return 1;
	}
	return 0;
}

/* Every member this tree was waiting for has either reported or left. */
static int rm_owed_complete(const struct vms_dlm_rm_tree *t)
{
	return t->n_owed != 0u && t->n_done == t->n_owed;
}

/* Is `csid` in this tree's owed set at all? (Who may be sent an op-0x14.) */
static int rm_owed_has(const struct vms_dlm_rm_tree *t, uint32_t csid)
{
	uint32_t i;

	for (i = 0u; i < t->n_owed; i++) {
		if (t->owed[i] == csid)
			return 1;
	}
	return 0;
}

/*
 * A DEPARTED member stops being waited for -- it will not be sending an
 * end-of-records -- and stops being a recipient, but it is NOT removed from the
 * set: `owed` is the membership the CLUB named when the tree was handed over,
 * and that record is what makes "this tree was announced to everyone who
 * reported, and to nobody else" checkable afterwards. Returns 1 only when the
 * member really was still owing one.
 */
static int rm_owed_strike(struct vms_dlm_rm_tree *t, uint32_t csid)
{
	uint32_t i;

	for (i = 0u; i < t->n_owed; i++) {
		if (t->owed[i] != csid || t->done[i])
			continue;
		t->done[i] = VMS_DLM_RM_OWED_DEPARTED;
		t->n_done++;
		return 1;
	}
	return 0;
}

/* ==========================================================================
 * 3. The two answers this node stages, both built by the codec
 * ========================================================================== */

/* RULE 1's 82/15 echo. `new_master` is a ROLE and only an op-0x0f answer uses
 * it; the caller passes 1 only when this node really holds an open remaster
 * for that tree. */
static int rm_stage_ack(struct vms_dlm_rm *f, const uint8_t *body,
			uint32_t len, int new_master)
{
	if (vms_dlm_rm_ack_build(body, len, new_master, f->txframe,
				 (uint32_t)sizeof(f->txframe),
				 (uint32_t *)0) != VMS_CODEC_OK) {
		f->codec_failures++;
		return -1;
	}
	if (f->ops->stage_reply(f->ops->ctx, f->txframe + VMS_OFF_SYSAP_BODY,
				VMS_CM_BODY_LEN) != 0)
		return -1;
	f->acks_sent++;
	return 0;
}

/* RULE 2's 82/01 grant. Every field comes out of `res` -- the engine's
 * read-back of the LKB and RSB it just created. */
static int rm_stage_lock_answer(struct vms_dlm_rm *f, const uint8_t *body,
				uint32_t len,
				const struct vms_dlm_adopt_lock_result *res)
{
	struct vms_dlm_rm_lock_answer a;
	uint32_t i;

	rm_bzero(&a, (uint32_t)sizeof(a));
	a.master_lkid = res->master_lkid;
	a.valblk_present = res->valblk_present;
	a.valblk_seq = res->valblk_seq;
	for (i = 0u; i < VMS_DLM_VALBLK_WIRE_LEN; i++)
		a.valblk[i] = res->valblk[i];
	if (vms_dlm_rm_lock_answer_build(body, len, &a, f->txframe,
					 (uint32_t)sizeof(f->txframe),
					 (uint32_t *)0) != VMS_CODEC_OK) {
		f->codec_failures++;
		return -1;
	}
	return f->ops->stage_reply(f->ops->ctx,
				   f->txframe + VMS_OFF_SYSAP_BODY,
				   VMS_CM_BODY_LEN);
}

/* ==========================================================================
 * 4. The op-0x14: "the tree is held here"
 * ========================================================================== */

/*
 * THE TWO EXECUTIVE READS BEHIND THE STATEMENT, and the hash.
 *
 * `adopt_end` has already returned SS$_NORMAL for this exact tree, which it
 * does only when an adoption of that root resource really was open here. This
 * is the SECOND, independent read: does this executive master the name now? A
 * no means something released the tree between the two calls, and then nothing
 * is sent -- `held_not_mastered`, counted.
 */
static int rm_held_body(struct vms_dlm_rm *f, const struct vms_dlm_rm_tree *t)
{
	struct vms_dlm_rm_held h;
	char nm[VMS_DLM_NAME_MAX + 1u];
	uint32_t hash = 0u;

	rm_ident_to_name(&t->id, nm, (uint32_t)sizeof(nm));
	/* The COUNT comes off the identity, not from the C string (rd
	 * vms-326): `nm` is NUL-terminated for a diagnostic's sake, and a
	 * resource name may contain NULs of its own. */
	if (!f->ops->name_mastered_here(f->ops->ctx, nm, t->id.name_len,
					t->id.group, t->id.mode))
		return -1;

	rm_bzero(&h, (uint32_t)sizeof(h));
	h.id = t->id;
	h.id.hash = 0u;
	if (f->ops->dir_hash(f->ops->ctx, nm, t->id.name_len, t->id.group,
			     t->id.mode, &hash)) {
		h.id.hash = hash;
		h.hash_known = 1u;
	}
	h.residue_fill = f->ops->residue_fill(f->ops->ctx);
	if (vms_dlm_rm_held_build(&h, f->txframe, (uint32_t)sizeof(f->txframe),
				  (uint32_t *)0) != VMS_CODEC_OK) {
		f->codec_failures++;
		return -1;
	}
	return 0;
}

/*
 * RULE W1/W2: one op-0x14 per op-0x13, to its sender, after the last one is in.
 * Returns how many really went out.
 */
static uint32_t rm_send_held(struct vms_dlm_rm *f, struct vms_dlm_rm_tree *t)
{
	uint32_t i, sent = 0u;

	if (rm_held_body(f, t) != 0) {
		f->held_not_mastered++;
		rm_say(f, "%DLM, a lock tree this node finished adopting is no "
			  "longer mastered here: the old master is NOT told it "
			  "moved (see the DLM counters)");
		return 0u;
	}
	for (i = 0u; i < t->n_owed; i++) {
		/* RULE W1: ONLY a member that really sent an end-of-records.
		 * Not one that is still owing one (it is not part of this
		 * hand-over as far as this node can tell), and not one that
		 * has LEFT the cluster. */
		if (t->done[i] != VMS_DLM_RM_OWED_REPORTED)
			continue;
		if (f->ops->send(f->ops->ctx, t->owed[i],
				 f->txframe + VMS_OFF_SYSAP_BODY,
				 VMS_CM_BODY_LEN) != 0) {
			f->held_unsent++;
			continue;
		}
		sent++;
	}
	return sent;
}

/*
 * THE TREE IS COMPLETE: close the adoption, then announce it. In that order,
 * because the 0x14 says the tree is HELD and `adopt_end` is what makes that
 * true (it is also what turns the adopted locks into ordinary master-side locks
 * the holders' next $ENQ is served from).
 */
static void rm_complete(struct vms_dlm_rm *f, struct vms_dlm_rm_tree *t)
{
	uint32_t n_locks = 0u, sent;

	if (!f->ops->adopt_end(f->ops->ctx, &t->tree, &n_locks)) {
		/* The engine has no such adoption: this node holds nothing to
		 * announce, so it announces nothing. The tree slot goes, and
		 * the holders' records were refused as they arrived. */
		rm_bzero(t, (uint32_t)sizeof(*t));
		rm_say(f, "%DLM, a lock-tree hand-over completed on the wire "
			  "and this node's lock database has no such adoption: "
			  "nothing is announced");
		return;
	}
	t->records = n_locks;
	t->state = (uint8_t)VMS_DLM_RM_ST_HELD;
	f->trees_held++;

	sent = rm_send_held(f, t);
	t->held_sent = sent;
	f->held_sent += sent;
	/*
	 * REPORTED when AT LEAST ONE op-0x14 really went out: the engine's
	 * `unreported` counts completions NOBODY was told about, and this one
	 * was told to somebody. A member that was owed one and did not get it
	 * is named by `held_unsent`, per frame -- the two counters answer
	 * different questions and neither stands in for the other.
	 */
	if (sent != 0u)
		f->ops->adopt_reported(f->ops->ctx);
}

/* ==========================================================================
 * 5. The handlers -- one per (state, event) cell
 * ========================================================================== */

struct rm_in {
	uint32_t from_csid;
	const uint8_t *body;
	uint32_t len;
	const struct vms_dlm_rm_step *step;        /* NULL for an op-0x12 */
	const struct vms_dlm_rm_lock_rec *rec;     /* op-0x12 only        */
};

typedef enum vms_dlm_rm_status (*rm_handler_t)(struct vms_dlm_rm *f,
					       struct vms_dlm_rm_tree *t,
					       const struct rm_in *in);

/* An event this node will not act on in this state. Counted by the caller. */
static enum vms_dlm_rm_status rm_h_refuse(struct vms_dlm_rm *f,
					  struct vms_dlm_rm_tree *t,
					  const struct rm_in *in)
{
	(void)f;
	(void)t;
	(void)in;
	return VMS_DLM_RM_REFUSED;
}

/*
 * op 0x0e -- "you are the new master of this tree". The hand-over OPENS here:
 * the adoption is opened BEFORE the answer is staged, because a 82/15 is this
 * node saying it will take the tree, and a node that cannot adopt it must not
 * say so (the old master would then hand over a tree nobody holds).
 */
static enum vms_dlm_rm_status rm_h_open(struct vms_dlm_rm *f,
					struct vms_dlm_rm_tree *t,
					const struct rm_in *in)
{
	struct vms_dlm_rm_tree *slot = rm_slot_free(f, &in->step->id);

	(void)t;
	if (slot == (struct vms_dlm_rm_tree *)0) {
		f->no_slot++;
		f->opens_refused++;
		rm_say(f, "%DLM, a VMS system is handing this node another lock "
			  "tree and every remaster slot is busy: NOT accepted, "
			  "the tree stays with its old master");
		return VMS_DLM_RM_REFUSED;
	}

	rm_bzero(slot, (uint32_t)sizeof(*slot));
	slot->id = in->step->id;
	slot->tree.old_master_csid = in->from_csid;
	slot->tree.res_group = in->step->id.group;
	slot->tree.res_mode = in->step->id.mode;
	slot->tree.ident_valid = 1u;
	rm_ident_to_name(&in->step->id, slot->tree.resnam,
			 (uint32_t)sizeof(slot->tree.resnam));

	if (rm_owed_snapshot(f, slot) != 0) {
		f->opens_refused++;
		rm_say(f, "%DLM, a lock tree is being handed to this node and "
			  "it cannot name the members it would have to collect "
			  "lock records from: NOT accepted");
		rm_bzero(slot, (uint32_t)sizeof(*slot));
		return VMS_DLM_RM_REFUSED;
	}
	if (!f->ops->adopt_open(f->ops->ctx, &slot->tree)) {
		f->opens_refused++;
		rm_say(f, "%DLM, this node was asked to become the master of a "
			  "lock tree and its lock database would not open the "
			  "adoption: NOT accepted, nothing is answered");
		rm_bzero(slot, (uint32_t)sizeof(*slot));
		return VMS_DLM_RM_REFUSED;
	}
	slot->state = (uint8_t)VMS_DLM_RM_ST_OPEN;
	f->opens++;
	if (rm_stage_ack(f, in->body, in->len, 0) != 0)
		return VMS_DLM_RM_REFUSED;
	return VMS_DLM_RM_HANDLED;
}

/*
 * op 0x0f -- sent to EVERY member. The answer is the same echo either way
 * except for one cell: the NEW MASTER additionally clears body[28:32] (63 of 70
 * real pairs; rd vms-cab grounds the same rule on 3,299). The role is read off
 * this machine's own state -- a tree really open here -- never off the frame.
 */
static enum vms_dlm_rm_status rm_h_role(struct vms_dlm_rm *f,
					struct vms_dlm_rm_tree *t,
					const struct rm_in *in)
{
	int new_master = (t != (struct vms_dlm_rm_tree *)0 &&
			  t->state == (uint8_t)VMS_DLM_RM_ST_OPEN) ? 1 : 0;

	if (rm_stage_ack(f, in->body, in->len, new_master) != 0)
		return VMS_DLM_RM_REFUSED;
	if (new_master)
		f->roles_as_new_master++;
	return VMS_DLM_RM_HANDLED;
}

/* op 0x10 -- the third opening step, answered with the same echo. Only for a
 * tree really open here: answering otherwise claims a role this node has not
 * taken. */
static enum vms_dlm_rm_status rm_h_prep(struct vms_dlm_rm *f,
					struct vms_dlm_rm_tree *t,
					const struct rm_in *in)
{
	(void)t;
	if (rm_stage_ack(f, in->body, in->len, 0) != 0)
		return VMS_DLM_RM_REFUSED;
	return VMS_DLM_RM_HANDLED;
}

/*
 * ABANDON a tree after a refused record (vms_dlm_adopt.h): release exactly what
 * this transaction installed and give the tree back. A HALF adopted tree must
 * never be served -- the locks it is missing are locks a holder still believes
 * it holds.
 */
static void rm_abandon(struct vms_dlm_rm *f, struct vms_dlm_rm_tree *t)
{
	uint32_t undone = 0u;

	(void)f->ops->adopt_abort(f->ops->ctx, &t->tree, &undone);
	f->aborts++;
	rm_say(f, "%DLM, a lock record in a tree being handed to this node "
		  "could not be adopted: the WHOLE tree is given back to its "
		  "old master, and no lock in it is served from here");
	rm_bzero(t, (uint32_t)sizeof(*t));
}

/* Fill the engine's record from the parsed one plus the CONNECTION MANAGER's
 * identification of the sender -- spec SS4(a)'s cluster-logical source address,
 * the one grounded answer to "whose lock is this". The body carries no CSID. */
static void rm_fill_rec(const struct rm_in *in,
			struct vms_dlm_adopt_lock_rec *out)
{
	uint32_t i;

	rm_bzero(out, (uint32_t)sizeof(*out));
	rm_ident_to_name(&in->rec->id, out->resnam,
			 (uint32_t)sizeof(out->resnam));
	out->holder_csid = in->from_csid;
	out->holder_lkid = in->rec->holder_lkid;
	out->parent_master_lkid = in->rec->parent_master_lkid;
	out->res_group = in->rec->id.group;
	out->res_mode = in->rec->id.mode;
	out->ident_valid = 1u;
	out->granted_mode = in->rec->granted_mode;
	out->requested_mode = in->rec->requested_mode;
	out->valblk_present = in->rec->valblk_present;
	out->valblk_seq = in->rec->valblk_seq;
	for (i = 0u; i < VMS_DLM_VALBLK_WIRE_LEN; i++)
		out->valblk[i] = in->rec->valblk[i];
}

/*
 * op 0x12 -- ONE LOCK a holder holds in the tree. The engine creates a real
 * master-side LKB on a real resource block and hands back the handle it minted;
 * the answer is built from that read-back and from nothing else.
 */
static enum vms_dlm_rm_status rm_h_record(struct vms_dlm_rm *f,
					  struct vms_dlm_rm_tree *t,
					  const struct rm_in *in)
{
	struct vms_dlm_adopt_lock_rec rec;
	struct vms_dlm_adopt_lock_result res;

	rm_fill_rec(in, &rec);
	if (!f->ops->adopt_lock(f->ops->ctx, &t->tree, &rec, &res)) {
		f->records_refused++;
		rm_abandon(f, t);
		return VMS_DLM_RM_REFUSED;
	}
	if (res.duplicate)
		f->records_duplicate++;
	else
		f->records_adopted++;
	if (rm_stage_lock_answer(f, in->body, in->len, &res) != 0)
		return VMS_DLM_RM_REFUSED;
	return VMS_DLM_RM_HANDLED;
}

/*
 * op 0x13 -- "that is every record from me". A NOTIFICATION: 68 of 68 real ones
 * open no transaction and are never answered. When the LAST owed member has
 * sent one, the tree is complete and the op-0x14s go out (rule W2).
 */
static enum vms_dlm_rm_status rm_h_end(struct vms_dlm_rm *f,
				       struct vms_dlm_rm_tree *t,
				       const struct rm_in *in)
{
	if (!rm_owed_mark(t, in->from_csid)) {
		f->ends_unexpected++;
		return VMS_DLM_RM_REFUSED;
	}
	f->ends_seen++;
	if (rm_owed_complete(t))
		rm_complete(f, t);
	return VMS_DLM_RM_HANDLED;
}

/* ==========================================================================
 * 6. THE TABLE
 * ========================================================================== */
static const rm_handler_t rm_table[VMS_DLM_RM_ST__COUNT][VMS_DLM_RM_EV__COUNT] = {
	/*
	 * No tree here. Only a 0x0e opens one. A 0x0f for a tree this node is
	 * not adopting is still ANSWERED -- as every member other than the new
	 * master answers it (888 of 888 real pairs, rd vms-cab): left
	 * unanswered it stalls the sender's file system. The other three name a
	 * tree nobody handed this node, so there is nothing to act on.
	 */
	[VMS_DLM_RM_ST_FREE] = {
		[VMS_DLM_RM_EV_OPEN]   = rm_h_open,
		[VMS_DLM_RM_EV_ROLE]   = rm_h_role,
		[VMS_DLM_RM_EV_PREP]   = rm_h_refuse,
		[VMS_DLM_RM_EV_RECORD] = rm_h_refuse,
		[VMS_DLM_RM_EV_END]    = rm_h_refuse,
	},
	/*
	 * The adoption is open. A SECOND 0x0e for the same tree is refused: the
	 * engine refuses a second hand-over of a tree already being adopted,
	 * and taking it would reset the owed set under the holders' feet.
	 */
	[VMS_DLM_RM_ST_OPEN] = {
		[VMS_DLM_RM_EV_OPEN]   = rm_h_refuse,
		[VMS_DLM_RM_EV_ROLE]   = rm_h_role,
		[VMS_DLM_RM_EV_PREP]   = rm_h_prep,
		[VMS_DLM_RM_EV_RECORD] = rm_h_record,
		[VMS_DLM_RM_EV_END]    = rm_h_end,
	},
	/*
	 * The tree is held and announced. A 0x0e is a FRESH hand-over of the
	 * same tree (it moved away and came back) and opens a new slot. A 0x12
	 * or 0x13 now is a record for a transaction that is closed: refused and
	 * counted, never answered with a handle from the closed adoption.
	 */
	[VMS_DLM_RM_ST_HELD] = {
		[VMS_DLM_RM_EV_OPEN]   = rm_h_open,
		[VMS_DLM_RM_EV_ROLE]   = rm_h_role,
		[VMS_DLM_RM_EV_PREP]   = rm_h_refuse,
		[VMS_DLM_RM_EV_RECORD] = rm_h_refuse,
		[VMS_DLM_RM_EV_END]    = rm_h_refuse,
	},
};

/* ==========================================================================
 * 7. The door: one inbound step
 * ========================================================================== */

/* The table axis for a wire opcode. -1 for anything that is not a step. */
static int rm_event_of(uint8_t op)
{
	switch (op) {
	case VMS_DLM_WIREOP_RM_0E:
		return (int)VMS_DLM_RM_EV_OPEN;
	case VMS_DLM_WIREOP_RM_0F:
		return (int)VMS_DLM_RM_EV_ROLE;
	case VMS_DLM_WIREOP_RM_10:
		return (int)VMS_DLM_RM_EV_PREP;
	case VMS_DLM_WIREOP_RM_12:
		return (int)VMS_DLM_RM_EV_RECORD;
	case VMS_DLM_WIREOP_RM_13:
		return (int)VMS_DLM_RM_EV_END;
	default:
		return -1;
	}
}

/*
 * WHICH TREE DOES AN op-0x12 BELONG TO? A ROOT record (no parent) names its
 * tree directly. A SUB record names only its own resource, so the ENGINE is
 * asked which open adoption minted the parent handle at body[40:44]; a handle
 * no open adoption minted resolves to no tree, and the record is refused.
 */
static struct vms_dlm_rm_tree *rm_tree_of_record(struct vms_dlm_rm *f,
						 const struct vms_dlm_rm_lock_rec *r)
{
	struct vms_dlm_adopt_tree parent;
	struct vms_dlm_res_ident id;
	uint32_t i;

	if (r->is_root)
		return rm_slot_of(f, &r->id, VMS_DLM_RM_ST_OPEN);
	if (!f->ops->adopt_tree_of_lkid(f->ops->ctx, r->parent_master_lkid,
					&parent))
		return (struct vms_dlm_rm_tree *)0;
	rm_bzero(&id, (uint32_t)sizeof(id));
	id.group = parent.res_group;
	id.mode = parent.res_mode;
	for (i = 0u; i < VMS_DLM_NAME_MAX && parent.resnam[i] != '\0'; i++)
		id.name[i] = (uint8_t)parent.resnam[i];
	id.name_len = (uint8_t)i;
	if (id.name_len == 0u)
		return (struct vms_dlm_rm_tree *)0;
	return rm_slot_of(f, &id, VMS_DLM_RM_ST_OPEN);
}

/* The slot an event applies to, and the state to dispatch in. */
static struct vms_dlm_rm_tree *rm_dispatch_slot(struct vms_dlm_rm *f,
						const struct rm_in *in,
						int ev, uint8_t *state)
{
	struct vms_dlm_rm_tree *t;

	if (ev == (int)VMS_DLM_RM_EV_RECORD)
		t = rm_tree_of_record(f, in->rec);
	else
		t = rm_slot_any(f, &in->step->id);
	*state = (t != (struct vms_dlm_rm_tree *)0) ? t->state :
						      (uint8_t)VMS_DLM_RM_ST_FREE;
	return t;
}

enum vms_dlm_rm_status vms_dlm_rm_step(struct vms_dlm_rm *f, uint32_t from_csid,
				       uint8_t category, uint8_t opcode,
				       const uint8_t *body, uint32_t len)
{
	struct vms_dlm_rm_step step;
	struct vms_dlm_rm_lock_rec rec;
	struct vms_dlm_rm_tree *t;
	struct rm_in in;
	uint8_t state = (uint8_t)VMS_DLM_RM_ST_FREE;
	int ev;

	if (f == (struct vms_dlm_rm *)0 || body == (const uint8_t *)0 ||
	    len < VMS_CM_BODY_LEN)
		return VMS_DLM_RM_NOT_MINE;
	if (category != (uint8_t)VMS_DLM_CAT_REQUEST)
		return VMS_DLM_RM_NOT_MINE;
	ev = rm_event_of(opcode);
	if (ev < 0)
		return VMS_DLM_RM_NOT_MINE;
	/* A system that cannot be named cannot hand a tree over, hold a lock in
	 * one, or be sent an op-0x14 (spec SS4(a)). */
	if (from_csid == 0u || !rm_ops_ok(f))
		return VMS_DLM_RM_REFUSED;

	rm_bzero(&in, (uint32_t)sizeof(in));
	in.from_csid = from_csid;
	in.body = body;
	in.len = len;
	if (ev == (int)VMS_DLM_RM_EV_RECORD) {
		if (vms_dlm_rm_lock_rec_parse_body(body, len, &rec) !=
		    VMS_CODEC_OK)
			return VMS_DLM_RM_REFUSED;
		in.rec = &rec;
	} else {
		if (vms_dlm_rm_step_parse_body(body, len, &step) !=
		    VMS_CODEC_OK)
			return VMS_DLM_RM_REFUSED;
		in.step = &step;
	}

	t = rm_dispatch_slot(f, &in, ev, &state);
	return rm_table[state][ev](f, t, &in);
}

/* ==========================================================================
 * 8. The rest of the door
 * ========================================================================== */

void vms_dlm_rm_init(struct vms_dlm_rm *f, const struct vms_dlm_rm_ops *ops)
{
	if (f == (struct vms_dlm_rm *)0)
		return;
	rm_bzero(f, (uint32_t)sizeof(*f));
	f->ops = ops;
}

int vms_dlm_rm_held_ack(struct vms_dlm_rm *f, uint32_t from_csid,
			uint8_t category, uint8_t opcode,
			const uint8_t *body, uint32_t len)
{
	struct vms_dlm_res_ident id;
	uint32_t i;

	if (f == (struct vms_dlm_rm *)0 || body == (const uint8_t *)0 ||
	    len < VMS_CM_BODY_LEN || from_csid == 0u)
		return 0;
	if (category != (uint8_t)(VMS_DLM_CAT_REQUEST |
				  VMS_WIRE_RESPONSE_BIT) ||
	    opcode != (uint8_t)VMS_DLM_WIREOP_RM_ACK)
		return 0;
	/*
	 * A real 82/15 is the request echoed, so it still names the tree -- the
	 * only correlation available, because the connection manager's own
	 * transaction token is not surfaced to the DLM today (the same note
	 * dlm_arm_handle_reply carries). An answer that names a tree this node
	 * really announced TO THAT SYSTEM is the answer to that announcement.
	 */
	if (vms_dlm_rm_ack_ident(body, len, &id) != VMS_CODEC_OK)
		return 0;
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		struct vms_dlm_rm_tree *t = &f->tree[i];

		if (t->state != (uint8_t)VMS_DLM_RM_ST_HELD ||
		    t->held_sent == 0u || !rm_owed_has(t, from_csid))
			continue;
		if (!rm_ident_eq(&t->id, &id))
			continue;
		f->acks_received++;
		return 1;
	}
	return 0;
}

void vms_dlm_rm_notified(struct vms_dlm_rm *f)
{
	if (f != (struct vms_dlm_rm *)0)
		f->notifications++;
}

void vms_dlm_rm_member_departed(struct vms_dlm_rm *f, uint32_t csid)
{
	uint32_t i;

	if (f == (struct vms_dlm_rm *)0 || csid == 0u || !rm_ops_ok(f))
		return;
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		struct vms_dlm_rm_tree *t = &f->tree[i];

		if (t->state != (uint8_t)VMS_DLM_RM_ST_OPEN)
			continue;
		if (!rm_owed_strike(t, csid))
			continue;
		f->departures_struck++;
		/* The tree may have been waiting only for the system that just
		 * left. An owed set emptied by departures is NOT completed:
		 * nobody is left to announce it to, and `adopt_end` would
		 * report a tree no member asked for. */
		if (rm_owed_complete(t))
			rm_complete(f, t);
	}
}

int vms_dlm_rm_is_new_master(const struct vms_dlm_rm *f,
			     const struct vms_dlm_res_ident *id)
{
	uint32_t i;

	if (f == (const struct vms_dlm_rm *)0 ||
	    id == (const struct vms_dlm_res_ident *)0 || id->name_len == 0u)
		return 0;
	/* Scans for an OPEN match specifically, and not "the first slot naming
	 * this tree": the role is "an adoption of it is open HERE, now". */
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		if (f->tree[i].state == (uint8_t)VMS_DLM_RM_ST_OPEN &&
		    rm_ident_eq(&f->tree[i].id, id))
			return 1;
	}
	return 0;
}

const struct vms_dlm_rm_tree *
vms_dlm_rm_find(const struct vms_dlm_rm *f, const struct vms_dlm_res_ident *id)
{
	uint32_t i;

	if (f == (const struct vms_dlm_rm *)0 ||
	    id == (const struct vms_dlm_res_ident *)0 || id->name_len == 0u)
		return (const struct vms_dlm_rm_tree *)0;
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		if (f->tree[i].state != (uint8_t)VMS_DLM_RM_ST_FREE &&
		    rm_ident_eq(&f->tree[i].id, id))
			return &f->tree[i];
	}
	return (const struct vms_dlm_rm_tree *)0;
}

uint32_t vms_dlm_rm_trees_in_state(const struct vms_dlm_rm *f,
				   enum vms_dlm_rm_state st)
{
	uint32_t i, n = 0u;

	if (f == (const struct vms_dlm_rm *)0)
		return 0u;
	for (i = 0u; i < VMS_DLM_RM_TREES; i++) {
		if (f->tree[i].state == (uint8_t)st)
			n++;
	}
	return n;
}
