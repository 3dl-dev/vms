// SPDX-License-Identifier: GPL-2.0
/*
 * vms_dlm_scs_fsm.c - the DLM's REQUESTER arm (FC-P4.6).
 *
 * Read vms_dlm_scs_fsm.h first: it carries the contract, the INV-6 argument
 * (why this object has no field to plumb), and the GROUNDED / NOT-GROUNDED line
 * this file stops at.
 *
 * Pure: no seam call, no allocation, no clock but ops->now_ms, no wire offset
 * of its own -- every cat-0x02 byte goes through vms_cluster_codec_dlm.h.
 */

#include "vms_dlm_scs_fsm.h"

/* ==========================================================================
 * 0. The two libc-free primitives this file needs
 * ========================================================================== */

static void dq_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0u; i < n; i++)
		b[i] = 0u;
}

static void dq_memcpy(void *dst, const void *src, uint32_t n)
{
	uint8_t *d = (uint8_t *)dst;
	const uint8_t *s = (const uint8_t *)src;
	uint32_t i;

	for (i = 0u; i < n; i++)
		d[i] = s[i];
}

/* Copy a NUL-terminated resource name out of a post into a codec name field.
 * Returns the length placed. The post's `resnam` came from the RSB. */
static uint8_t dq_name_from_post(const struct vms_dlm_proxy_post *p,
				 uint8_t *out)
{
	uint8_t n = 0u;

	while (n < (uint8_t)VMS_DLM_NAME_MAX && p->resnam[n] != '\0') {
		out[n] = (uint8_t)p->resnam[n];
		n++;
	}
	return n;
}

/* The mirror: a codec name field into a NUL-terminated buffer the engine's
 * `learn_dir_hash` / `assume_mastery` take. `out` is VMS_DLM_NAME_MAX + 1. */
static void dq_name_to_cstr(const uint8_t *name, uint8_t len, char *out)
{
	uint8_t i;

	if (len > (uint8_t)VMS_DLM_NAME_MAX)
		len = (uint8_t)VMS_DLM_NAME_MAX;
	for (i = 0u; i < len; i++)
		out[i] = (char)name[i];
	out[len] = '\0';
}

static void dq_log(struct dlm_req_fsm *f, const char *msg)
{
	if (f->ops != (const struct dlm_req_ops *)0 &&
	    f->ops->log != (void (*)(void *, const char *))0)
		f->ops->log(f->ops->ctx, msg);
}

static uint32_t dq_now(const struct dlm_req_fsm *f)
{
	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->now_ms == (uint32_t (*)(void *))0)
		return 0u;
	return f->ops->now_ms(f->ops->ctx);
}

static uint32_t dq_generation(const struct dlm_req_fsm *f)
{
	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->dir_generation == (uint32_t (*)(void *))0)
		return 0u;
	return f->ops->dir_generation(f->ops->ctx);
}

/* ==========================================================================
 * 1. The request table
 *
 * Keyed by req_lkid -- THIS node's own lock id, the value the executive minted
 * and the half of (req_csid, req_lkid) that D-DLM-5 makes every retransmit
 * idempotent on. One key, one block: a duplicate post or a duplicate reply
 * finds the SAME block and can never mint a second.
 * ========================================================================== */

static struct dlm_req *dq_find(struct dlm_req_fsm *f, uint32_t req_lkid)
{
	uint32_t i;

	if (req_lkid == VMS_DLM_LKID_UNSET)
		return (struct dlm_req *)0;
	for (i = 0u; i < DLM_REQ_MAX; i++) {
		if (f->req[i].state != (uint8_t)DLM_REQ_ST_IDLE &&
		    f->req[i].req_lkid == req_lkid)
			return &f->req[i];
	}
	return (struct dlm_req *)0;
}

static struct dlm_req *dq_alloc(struct dlm_req_fsm *f, uint32_t req_lkid)
{
	uint32_t i;

	for (i = 0u; i < DLM_REQ_MAX; i++) {
		if (f->req[i].state == (uint8_t)DLM_REQ_ST_IDLE) {
			dq_bzero(&f->req[i], (uint32_t)sizeof(f->req[i]));
			f->req[i].req_lkid = req_lkid;
			return &f->req[i];
		}
	}
	f->no_slot++;
	return (struct dlm_req *)0;
}

static void dq_free(struct dlm_req *r)
{
	dq_bzero(r, (uint32_t)sizeof(*r));   /* state becomes ST_IDLE */
}

/* ==========================================================================
 * 2. Transmission
 *
 * ONE function builds a request frame, ONE sends it, and both take a POST --
 * which is the only shape a wire field ever arrives in here.
 * ========================================================================== */

/*
 * VMS_DLM_POST_* -> the wire opcode THIS ARM TRANSMITS, or 0 for "none".
 *
 * A RELEASE NOW MAPS TO op 0x03 (rd vms-d7a3). It used to map to 0 for two
 * successive reasons, and both are spent: first "there is no grounded opcode
 * for a $DEQ", which vms-c03 ended by grounding 0x03 from a real 2-node
 * OpenVMS VAX 7.3 capture; then "the opcode is grounded but this arm has not
 * been cleared to emit it", which is what this item does -- behind the two
 * gates dq_may_transmit applies (the all-OVMX gate here, RULE C in the
 * connection manager under ops->send). Nothing here guesses: the mapping is a
 * table, and every FIELD of the frame it selects comes out of the post.
 *
 * 0 is therefore reachable only for an operation this seam does not have.
 */
static uint8_t dq_wireop(uint32_t post_op)
{
	if (post_op == VMS_DLM_POST_ENQ)
		return (uint8_t)VMS_DLM_WIREOP_ENQ;
	if (post_op == VMS_DLM_POST_CONVERT)
		return (uint8_t)VMS_DLM_WIREOP_CONVERT;
	if (post_op == VMS_DLM_POST_DEQ)
		return (uint8_t)VMS_DLM_WIREOP_DEQ;
	return 0u;
}

/*
 * THE NEW-SHAPE GATE (memory ovmx-never-crashes-a-peer; header §"WHAT IS
 * GROUNDED").
 *
 * GROUNDED IS NOT CLEARED. op 0x03 is a shape this tree has read off a real
 * cluster's wire and has never yet WATCHED a real peer accept from OVMX, so it
 * is addressed only where every member is proven to run this implementation --
 * a fact the connection manager holds (vms_ldwv_all_ovmx) and this object only
 * asks for. An ABSENT op reads CLOSED: "nobody told us" and "every member is
 * ours" are different facts and only one may put a new shape on a wire.
 *
 * RULE C is the OTHER half and it is deliberately not here: the connection
 * manager refuses per DESTINATION on `csb->peer_is_ours` inside ops->send.
 */
static int dq_new_shape_ok(const struct dlm_req_fsm *f)
{
	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->all_ovmx == (int (*)(void *))0)
		return 0;
	return f->ops->all_ovmx(f->ops->ctx) != 0;
}

/* Hand the built body to the connection manager. The codec wrote FRAME-absolute
 * offsets into txframe; a SYSAP is handed a body, so the body is the span from
 * VMS_OFF_SYSAP_BODY on (header §7, the splice). */
static enum dlm_req_status dq_emit(struct dlm_req_fsm *f, vms_csid_t dst)
{
	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->send == (int (*)(void *, vms_csid_t, const uint8_t *,
				     uint32_t))0) {
		f->send_failures++;
		return DLM_REQ_E_SEND;
	}
	if (f->ops->send(f->ops->ctx, dst, f->txframe + VMS_OFF_SYSAP_BODY,
			 DLM_REQ_BODY_LEN) != 0) {
		f->send_failures++;
		return DLM_REQ_E_SEND;
	}
	return DLM_REQ_OK;
}

/*
 * Build ONE ENQ/CONVERT request frame from ONE post.
 *
 * EVERY FIELD COMES OUT OF `p`, and `p` is what dlm_proxy_fill_post() read from
 * the LKB and the RSB. There is no other source in this function and there is
 * nowhere for one to come from.
 *
 * The directory hash rides ONLY when the executive holds a wire-learned one
 * (`dir_hash_known`); the codec then writes body[10:12], and writes nothing
 * there otherwise. A zero written there is the grant storm.
 */
static enum dlm_req_status dq_build_request(struct dlm_req_fsm *f,
					    const struct vms_dlm_proxy_post *p,
					    uint8_t wireop)
{
	struct vms_dlm_enq_request req;
	uint32_t written = 0u;

	/*
	 * THE VALUE-BLOCK WRITE CROSSING (op-0x06). When the engine marked this
	 * post as a value-block write (a convert that demotes a PW/EX holder with
	 * LCK$M_VALBLK -- wire-confirmed, vms-727), it goes out as the grounded
	 * op-0x06 CONVERT-with-VALBLK, carrying the LKB's real block, instead of a
	 * plain op-0x07 that would silently drop the write and leave a real-VAX
	 * reader with a stale value. body[32]/[52] (the sender-private per-lock
	 * serial) is left 0, exactly as the ENQ builder omits body[32:36]: a peer
	 * routes by master_lkid and provably accepts a zero there.
	 */
	if (p->write_valblk) {
		struct vms_dlm_valblk_convert c;
		uint32_t i;

		dq_bzero(&c, (uint32_t)sizeof(c));
		c.req_lkid    = p->req_lkid;
		c.master_lkid = p->master_lkid;
		c.mode        = (uint8_t)p->lkmode;   /* the mode converted TO */
		c.serial      = 0u;                   /* honest omission, as the ENQ */
		for (i = 0u; i < VMS_DLM_VALBLK_WIRE_LEN; i++)
			c.valblk[i] = p->valblk[i];

		dq_bzero(f->txframe, (uint32_t)sizeof(f->txframe));
		if (vms_dlm_valblk_convert_build(&c, f->txframe,
						 (uint32_t)sizeof(f->txframe),
						 &written) != VMS_CODEC_OK) {
			/* A write crossing the arm could NOT put on the wire. */
			f->lvb_write_no_wire_field++;
			f->codec_failures++;
			return DLM_REQ_E_CODEC;
		}
		f->lvb_writes_sent++;
		return DLM_REQ_OK;
	}

	dq_bzero(&req, (uint32_t)sizeof(req));
	req.mode            = (uint8_t)p->lkmode;
	req.req_pid_or_lkid = p->req_lkid;      /* our own executive handle */
	req.master_lkid     = p->master_lkid;   /* 0 until the master named it */
	req.dir_hash        = p->dir_hash;
	req.dir_hash_valid  = p->dir_hash_known;
	req.name_len        = dq_name_from_post(p, req.name);

	dq_bzero(f->txframe, (uint32_t)sizeof(f->txframe));
	if (vms_dlm_enq_request_build(&req, wireop, f->txframe,
				      (uint32_t)sizeof(f->txframe),
				      &written) != VMS_CODEC_OK) {
		f->codec_failures++;
		return DLM_REQ_E_CODEC;
	}
	return DLM_REQ_OK;
}

/*
 * Build ONE op-0x03 $DEQ request frame from ONE post (rd vms-d7a3).
 *
 * THREE FIELDS, THREE EXECUTIVE READS, AND NOTHING ELSE. `req_lkid` is the
 * proxy LKB's own handle; `master_lkid` is the master's handle for that lock,
 * as the master's OWN grant reply put it in the lock database (0 until then,
 * which the codec refuses); the mode is the mode the LKB holds as it is
 * released, which is what vms-c03 grounds body[30] of a real DEQ as. All three
 * come out of the post `dlm_proxy_fill_post()` just filled.
 *
 * WHAT IS *NOT* ON IT, and why each omission is the protocol's own shape:
 *   - NO RESOURCE NAME. A real DEQ names its lock by lock-id. The reference
 *     frame's body[46] holds uninitialised bytes that read 0x9a on one
 *     specimen and 0x00 on a second DEQ in the SAME capture -- the proof it is
 *     not a field, so writing the name there would be inventing one.
 *   - NO DIRECTORY HASH. A release is addressed to the MASTER, which the lock
 *     database names; a directory index is what you carry when you do not know
 *     who to ask.
 *   - NO VALUE BLOCK, and this one is a GAP rather than a shape: a release
 *     that wrote the LVB has no grounded field to carry it here (the value
 *     block is grounded at body[36:52] of an op-0x06 only, and that opcode has
 *     no builder because the four bytes ahead of the block are unpinned).
 *     Unchanged by this item, and stated rather than hidden.
 */
static enum dlm_req_status dq_build_deq(struct dlm_req_fsm *f,
					const struct vms_dlm_proxy_post *p)
{
	struct vms_dlm_deq d;
	uint32_t written = 0u;

	dq_bzero(&d, (uint32_t)sizeof(d));
	d.req_lkid    = p->req_lkid;      /* our own executive handle          */
	d.master_lkid = p->master_lkid;   /* what the master's grant recorded  */
	d.mode        = (uint8_t)p->lkmode;

	dq_bzero(f->txframe, (uint32_t)sizeof(f->txframe));
	if (vms_dlm_deq_build(&d, f->txframe, (uint32_t)sizeof(f->txframe),
			      &written) != VMS_CODEC_OK) {
		/* The codec refuses an unset lock id in EITHER field -- the
		 * fc8540ae INVLOCKID lesson, applied to a lock-id-only message. */
		f->codec_failures++;
		return DLM_REQ_E_CODEC;
	}
	return DLM_REQ_OK;
}

/* One frame, from one post, to one destination. The opcode selects which
 * builder; no request block is touched, so both the tracked and the untracked
 * release path go through exactly this. */
static enum dlm_req_status dq_build_and_send(struct dlm_req_fsm *f,
					     const struct vms_dlm_proxy_post *p,
					     uint8_t wireop, vms_csid_t dst)
{
	enum dlm_req_status st;

	if (wireop == (uint8_t)VMS_DLM_WIREOP_DEQ)
		st = dq_build_deq(f, p);
	else
		st = dq_build_request(f, p, wireop);
	if (st != DLM_REQ_OK)
		return st;
	return dq_emit(f, dst);
}

/*
 * MAY this frame be transmitted at all? Every refusal is counted where it is
 * decided, and NOTHING is built before they have all passed -- a frame that
 * exists is a frame a later edit can send.
 */
static enum dlm_req_status dq_may_transmit(struct dlm_req_fsm *f, uint8_t wireop,
					   vms_csid_t dst, uint8_t to_directory,
					   const struct vms_dlm_proxy_post *p)
{
	if (p == (const struct vms_dlm_proxy_post *)0)
		return DLM_REQ_E_INVAL;
	if (wireop == 0u) {
		f->posts_no_wireop++;
		return DLM_REQ_E_NOWIREOP;
	}
	if (wireop == (uint8_t)VMS_DLM_WIREOP_DEQ && !dq_new_shape_ok(f)) {
		/* The all-OVMX gate is closed: a shape no real peer has been
		 * watched to take may not be addressed at a mixed cluster. */
		dq_log(f, "%CNXMAN, cross-node lock release not sent: the "
			  "cluster is not all-OVMX and op-03 is not cleared "
			  "for a system that has not proved it runs this "
			  "implementation");
		return DLM_REQ_E_NOWIREOP;
	}
	if (dst == 0u) {
		f->dir_unresolved++;
		return DLM_REQ_E_NODIR;
	}
	if (to_directory && !p->dir_hash_known) {
		/* The refusal that cures the grant storm: never a lookup with a
		 * hash this node did not receive from the cluster. */
		f->hash_unknown_refused++;
		dq_log(f, "%CNXMAN, directory lookup refused: no wire-learned "
			  "hash for this resource name");
		return DLM_REQ_E_NOHASH;
	}
	return DLM_REQ_OK;
}

/* The bookkeeping a transmission leaves on its request block. */
static void dq_note_transmitted(struct dlm_req_fsm *f, struct dlm_req *r)
{
	r->sent_ms = dq_now(f);
	r->frames_tx++;
	if (r->tries < 0xffu)
		r->tries++;
	if (r->post_op == VMS_DLM_POST_DEQ)
		f->releases_sent++;
	else if (r->to_directory)
		f->lookups_sent++;
	else
		f->requests_sent++;
}

/*
 * Transmit `r`'s current operation from the post `p`.
 *
 * The caller is responsible for `p` being a FRESH executive read -- either the
 * post the engine just handed us, or one that came back from ops->refill_post a
 * moment ago. Nothing in this function remembers anything.
 */
static enum dlm_req_status dq_transmit(struct dlm_req_fsm *f, struct dlm_req *r,
				       const struct vms_dlm_proxy_post *p)
{
	uint8_t wireop = dq_wireop(r->post_op);
	enum dlm_req_status st;

	st = dq_may_transmit(f, wireop, r->dst_csid, r->to_directory, p);
	if (st != DLM_REQ_OK)
		return st;
	st = dq_build_and_send(f, p, wireop, r->dst_csid);
	if (st != DLM_REQ_OK)
		return st;
	dq_note_transmitted(f, r);
	return DLM_REQ_OK;
}

/*
 * Transmit a RELEASE for which this arm holds no request block.
 *
 * The block is only this object's WIRE RECORD; the lock is the engine's, and a
 * $DEQ post is a fresh read of it. So a release whose record was already
 * dropped -- a spent ladder, a departed master -- is still a real release of a
 * real lock, and the same three executive-read fields go on the same frame.
 * The gates are identical because they are applied by the same function.
 */
static enum dlm_req_status dq_transmit_release(struct dlm_req_fsm *f,
					       const struct vms_dlm_proxy_post *p)
{
	enum dlm_req_status st;
	vms_csid_t dst;

	if (p == (const struct vms_dlm_proxy_post *)0)
		return DLM_REQ_E_INVAL;
	dst = (vms_csid_t)p->dst_csid;
	st = dq_may_transmit(f, (uint8_t)VMS_DLM_WIREOP_DEQ, dst, 0u, p);
	if (st != DLM_REQ_OK)
		return st;
	st = dq_build_and_send(f, p, (uint8_t)VMS_DLM_WIREOP_DEQ, dst);
	if (st == DLM_REQ_OK)
		f->releases_sent++;
	return st;
}

/*
 * Re-read the proxy LKB and transmit from THAT -- the retransmit / retry path.
 *
 * This is where the anti-LARP rule is actually enforced: a retransmit is not a
 * re-send of remembered bytes, it is a fresh executive read that happens to
 * carry the same lock id. If the lock is gone, the transmission is ABANDONED,
 * because a frame about a lock that no longer exists is a frame with no object
 * behind it.
 */
static enum dlm_req_status dq_refill_transmit(struct dlm_req_fsm *f,
					      struct dlm_req *r)
{
	struct vms_dlm_proxy_post p;

	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->refill_post == (int (*)(void *, uint32_t, uint32_t,
					    vms_csid_t,
					    struct vms_dlm_proxy_post *))0)
		return DLM_REQ_E_INVAL;

	dq_bzero(&p, (uint32_t)sizeof(p));
	if (f->ops->refill_post(f->ops->ctx, r->req_lkid, r->post_op,
				r->dst_csid, &p) != 0) {
		f->lock_gone++;
		return DLM_REQ_E_NOLOCK;
	}
	return dq_transmit(f, r, &p);
}

/* ==========================================================================
 * 3. SETTLE ON GRANT -- the terminal state of a successful request
 *
 * THE GRANT IS THE ANSWER AND NOTHING FOLLOWS IT. An earlier build sent a
 * "completion + commit" pair here, built from a PROVISIONAL op-0x04/0x03
 * table. The vms-c03 capture of a real 2-node OpenVMS VAX 7.3 cluster showed
 * that pair was a phantom: 0x03 is $DEQ, 0x04 is BLKAST, and a real requester
 * answers a grant with NO frame at all (vms_cluster_codec_dlm.h's supersession
 * note). So the frames are gone, and with them the only post-grant state this
 * FSM ever had to wait on.
 *
 * WHAT REPLACES THEM IS NOT A GAP. Reaching ST_GRANTED with `settled` set IS
 * the terminal state: the beat skips a settled block (§12), so there is
 * nothing retransmitting, nothing counting down a ladder, and nothing holding
 * a request slot open for an acknowledgement that was never going to come.
 * The one thing that MUST still happen -- the master's handle landing in the
 * executive's own lock record -- happened in `ops->grant_recv` before this
 * runs, which is where it always belonged; it never needed a frame to carry
 * it back to the node that sent it.
 *
 * NOTHING IS STRANDED AT THE OTHER END EITHER: the DLM's inbound arm
 * (vms_dlm_scs.c) serves only ENQ/CONVERT/REBUILD and DECLINES everything
 * else, so no OVMX master ever consumed the pair, and no real VMS master ever
 * expected it.
 * ========================================================================== */
static void dq_settle(struct dlm_req_fsm *f, struct dlm_req *r)
{
	r->sent_ms = dq_now(f);   /* the beat's clock, left coherent          */
	r->tries   = 0u;          /* no ladder is running on a settled block  */
	r->settled = 1u;          /* nothing outstanding: the beat skips it   */
	r->state   = (uint8_t)DLM_REQ_ST_GRANTED;
	f->grants_settled++;
}

/* ==========================================================================
 * 4. Failing a request honestly
 * ========================================================================== */
static void dq_fail(struct dlm_req_fsm *f, struct dlm_req *r,
		    enum dlm_req_fail_reason why)
{
	if (f->ops != (const struct dlm_req_ops *)0 &&
	    f->ops->fail != (void (*)(void *, uint32_t,
				      enum dlm_req_fail_reason))0)
		f->ops->fail(f->ops->ctx, r->req_lkid, why);
	dq_free(r);
}

/* ==========================================================================
 * 5. Re-resolving a routing decision through the CURRENT vector
 *
 * A DECLINE is an ANSWER: the target is not the directory (or the master) for
 * this name. The correct response is to ask the vector again -- it may have
 * changed under us, which is exactly what Phase 1 of a transition does -- and
 * to STOP when it keeps naming the same node. Retrying the same target after
 * the same decline IS the 35/s grant storm.
 * ========================================================================== */
static enum dlm_req_status dq_reresolve(struct dlm_req_fsm *f,
					struct dlm_req *r)
{
	struct vms_dlm_proxy_post p;
	vms_csid_t csid = 0u;
	uint32_t gen;

	if (r->redirects >= (uint8_t)DLM_REQ_MAX_REDIRECTS)
		return DLM_REQ_E_NODIR;
	if (f->ops->dir_resolve == (int (*)(void *, uint16_t, vms_csid_t *))0)
		return DLM_REQ_E_NODIR;

	/* The hash comes out of the EXECUTIVE, on this read, and is passed to a
	 * resolver that cannot take a name. Nothing here derives one. */
	dq_bzero(&p, (uint32_t)sizeof(p));
	if (f->ops->refill_post(f->ops->ctx, r->req_lkid, r->post_op,
				r->dst_csid, &p) != 0) {
		f->lock_gone++;
		return DLM_REQ_E_NOLOCK;
	}
	if (!p.dir_hash_known) {
		f->hash_unknown_refused++;
		return DLM_REQ_E_NOHASH;
	}
	if (f->ops->dir_resolve(f->ops->ctx, p.dir_hash, &csid) != 0) {
		f->dir_unresolved++;
		return DLM_REQ_E_NODIR;
	}

	gen = dq_generation(f);
	if (csid == r->dst_csid && gen == r->dir_gen) {
		/* The same vector naming the same node that just declined. One
		 * more frame would be the first of the storm. */
		f->dir_unresolved++;
		return DLM_REQ_E_NODIR;
	}
	if (csid == 0u) {
		/* The vector says THIS node is the directory -- outcome 3's
		 * ground, and the engine, not this file, decides mastery. */
		return DLM_REQ_E_NODIR;
	}

	r->dst_csid     = csid;
	r->to_directory = 1u;
	r->dir_gen      = gen;
	r->tries        = 0u;
	r->redirects++;
	r->state        = (uint8_t)DLM_REQ_ST_LOOKUP;
	return dq_transmit(f, r, &p);
}

/* ==========================================================================
 * 6. The event context and the [state][event] handlers
 * ========================================================================== */
struct dq_ev {
	const struct vms_dlm_proxy_post   *post;   /* the three POST events   */
	const struct vms_dlm_enq_response *rsp;    /* GRANT / DENY            */
	vms_csid_t                         from_csid;
	vms_csid_t                         master_csid;  /* REDIRECT          */
	struct dlm_req                    *r;
	enum dlm_req_status                st;
};

typedef void (*dq_handler_t)(struct dlm_req_fsm *f, struct dq_ev *e);

/* ---- the POST events -------------------------------------------------- */

/* A brand-new request: take the engine's routing decision, then send. */
static void h_post_new(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;
	const struct vms_dlm_proxy_post *p = e->post;

	r->post_op      = p->op;
	r->dst_csid     = p->dst_csid;
	r->to_directory = p->to_directory;
	r->dir_gen      = dq_generation(f);
	r->state        = p->to_directory ? (uint8_t)DLM_REQ_ST_LOOKUP
					  : (uint8_t)DLM_REQ_ST_ENQ;

	e->st = dq_transmit(f, r, p);
	if (e->st != DLM_REQ_OK) {
		dq_free(r);        /* nothing went out: hold no phantom request */
		return;
	}
	if (p->op == VMS_DLM_POST_CONVERT)
		f->converts_posted++;
	else
		f->enqs_posted++;
}

/*
 * A POST for a request already outstanding: the SAME key, so the SAME block.
 * This is the outbound half of D-DLM-5's idempotency -- a re-post can never
 * mint a second request, and the frame that goes out is built from the post the
 * engine just filled, not from anything this object remembered.
 */
static void h_post_again(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;

	/*
	 * The engine RE-RESOLVED when it filled this post, so its routing
	 * decision supersedes the one this block was carrying -- the master may
	 * have been learned since the first transmission, which is precisely
	 * the case where re-sending to the old directory node would be sending
	 * a lookup for a tree whose master the executive already knows.
	 */
	r->post_op      = e->post->op;
	r->dst_csid     = e->post->dst_csid;
	r->to_directory = e->post->to_directory;
	r->state        = e->post->to_directory ? (uint8_t)DLM_REQ_ST_LOOKUP
						: (uint8_t)DLM_REQ_ST_ENQ;
	e->st = dq_transmit(f, r, e->post);
	if (e->st == DLM_REQ_OK)
		f->retransmits++;
}

/* A CONVERT on a lock we already hold: it goes to the MASTER, and the block
 * moves back to "outstanding at the master" until the reply lands. */
static void h_post_convert_granted(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;

	r->post_op      = VMS_DLM_POST_CONVERT;
	r->dst_csid     = e->post->dst_csid;
	r->to_directory = e->post->to_directory;
	r->tries        = 0u;
	r->settled      = 0u;
	r->state        = (uint8_t)DLM_REQ_ST_ENQ;

	e->st = dq_transmit(f, r, e->post);
	if (e->st == DLM_REQ_OK)
		f->converts_posted++;
	else
		r->state = (uint8_t)DLM_REQ_ST_GRANTED;  /* the lock still is */
}

/*
 * A RELEASE (rd vms-d7a3). The op-0x03 $DEQ is built from THIS post and sent to
 * the master, behind the two gates dq_may_transmit applies.
 *
 * THE SLOT IS FREED ON EVERY PATH THROUGH HERE, sent or not. That is what makes
 * a granted lock RELEASABLE without a completion round trip (the block settled
 * on the grant, and a $DEQ post takes it straight back to ST_IDLE, with no
 * intermediate "completing" state to get stuck behind) -- and it is also the
 * honest answer when the frame did NOT go out: the requester really is
 * releasing the lock, so a wire record of a lock we no longer hold would be its
 * own kind of fabrication. What is lost when a gate refuses is not tracked
 * silently: `releases_no_wire_op` rises and the reason is logged.
 *
 * A release is addressed to the MASTER, never to a directory node -- which is
 * why `to_directory` is cleared here rather than taken from the post.
 */
static void h_post_deq(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;

	if (r != (struct dlm_req *)0) {
		r->post_op      = VMS_DLM_POST_DEQ;
		r->to_directory = 0u;
		/* The engine RE-RESOLVED when it filled this post, so its
		 * routing decision supersedes the one this block carried. */
		if (e->post != (const struct vms_dlm_proxy_post *)0)
			r->dst_csid = (vms_csid_t)e->post->dst_csid;
		e->st = dq_transmit(f, r, e->post);
		dq_free(r);
	} else {
		e->st = dq_transmit_release(f, e->post);
	}

	if (e->st != DLM_REQ_OK) {
		f->releases_no_wire_op++;
		dq_log(f, "%CNXMAN, cross-node lock release NOT transmitted: "
			  "the wire record is dropped and the gap is counted");
	}
}

/* ---- the REPLY events ------------------------------------------------- */

/*
 * THE GRANT. Hand what the master genuinely said to the ENGINE -- which is what
 * records the master's handle on the proxy LKB and wakes the $ENQW -- and then
 * complete from a FRESH read of that same LKB.
 */
static void h_grant(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;
	struct vms_dlm_proxy_grant g;

	dq_bzero(&g, (uint32_t)sizeof(g));
	g.req_lkid     = r->req_lkid;          /* OUR key, not the frame's    */
	g.master_lkid  = e->rsp->master_lkid;  /* codec body[24:28]           */
	g.master_csid  = e->from_csid;         /* the frame's SCA source      */
	g.granted_mode = e->rsp->granted_mode; /* codec body[30]              */
	/*
	 * THE LVB READ CROSSING (vms-727). When the grant reply carried the
	 * master's value block (the codec recognised the grounded
	 * grant-with-valblk record and set `valblk_present`), hand it to the
	 * engine, which records it on the proxy LKB so a later $GETLKI reads the
	 * master's block back. When it did NOT (a plain grant, or a stale-buffer
	 * grant the codec refused), valblk_present stays 0 and the engine leaves
	 * the proxy's own block ALONE -- never sixteen zeros read as data.
	 */
	if (e->rsp->valblk_present) {
		g.valblk_present = 1u;
		dq_memcpy(g.valblk, e->rsp->valblk, VMS_DLM_VALBLK_WIRE_LEN);
	}

	if (f->ops->grant_recv(f->ops->ctx, &g) != 0) {
		/* The engine holds no proxy this grant can belong to. Nothing
		 * is recorded and nothing is answered. */
		f->replies_unmatched++;
		dq_free(r);
		e->st = DLM_REQ_E_NOLOCK;
		return;
	}

	f->grants_rx++;
	r->dst_csid     = e->from_csid;   /* the master, as the frame said     */
	r->to_directory = 0u;
	dq_settle(f, r);                  /* the grant IS the terminal settle  */
	e->st = DLM_REQ_OK;
}

/*
 * A grant for a lock we are already holding: the master retransmitted, most
 * likely because its own reply ladder had not been quiesced yet. RE-APPLY it
 * (the engine is idempotent on the key, D-DLM-5) and settle again. The
 * re-apply is the whole point and it is not a formality: it is a fresh
 * `grant_recv` with the handle THIS frame carried, so a master that reassigned
 * the lock id is followed, in the lock database, without this object having
 * remembered the old one.
 *
 * NOTHING IS SENT BACK. A duplicate grant needs no answer because a grant
 * needs no answer (see §3) -- so a retransmitting master cannot pump this node
 * into emitting a frame per received frame, which is the shape every storm in
 * this campaign has had.
 */
static void h_grant_dup(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct vms_dlm_proxy_grant g;

	dq_bzero(&g, (uint32_t)sizeof(g));
	g.req_lkid     = e->r->req_lkid;
	g.master_lkid  = e->rsp->master_lkid;
	g.master_csid  = e->from_csid;
	g.granted_mode = e->rsp->granted_mode;

	f->grants_duplicate++;
	if (f->ops->grant_recv(f->ops->ctx, &g) != 0) {
		e->st = DLM_REQ_E_NOLOCK;
		return;
	}
	e->r->dst_csid = e->from_csid;
	dq_settle(f, e->r);
	e->st = DLM_REQ_OK;
}

/*
 * A DENY at the DIRECTORY: it did not answer with a master. Re-resolve through
 * the current vector and retry; when the vector keeps naming the same node the
 * request is UNROUTABLE and the waiter is told so.
 */
static void h_deny_lookup(struct dlm_req_fsm *f, struct dq_ev *e)
{
	f->denies_rx++;
	e->st = dq_reresolve(f, e->r);
	if (e->st == DLM_REQ_OK) {
		f->declines_reresolved++;
		return;
	}
	dq_fail(f, e->r, DLM_REQ_FAIL_UNROUTABLE);
}

/*
 * A DENY at the MASTER: a real SS$_NOTQUEUED (the requester asked for NOQUEUE
 * and the lock was not free). That is an ANSWER, not a routing problem, so it
 * is not retried -- it is delivered.
 */
static void h_deny_master(struct dlm_req_fsm *f, struct dq_ev *e)
{
	f->denies_rx++;
	if (e->r->post_op == VMS_DLM_POST_CONVERT) {
		/*
		 * A refused CONVERT leaves the lock at its old mode: the lock
		 * is still real, so the wire record stays with it -- SETTLED,
		 * because the refusal is the answer and nothing is outstanding
		 * any more. Leaving it unsettled would hand the beat a block
		 * with no frame owed, which is the "dangling state waiting on
		 * an ack" shape the supersession removed everywhere else.
		 */
		e->r->settled = 1u;
		e->r->tries   = 0u;
		e->r->state   = (uint8_t)DLM_REQ_ST_GRANTED;
		if (f->ops->fail != (void (*)(void *, uint32_t,
					      enum dlm_req_fail_reason))0)
			f->ops->fail(f->ops->ctx, e->r->req_lkid,
				     DLM_REQ_FAIL_NOTQUEUED);
		e->st = DLM_REQ_OK;
		return;
	}
	dq_fail(f, e->r, DLM_REQ_FAIL_NOTQUEUED);
	e->st = DLM_REQ_OK;
}

/* ---- the DIRECTORY outcomes ------------------------------------------- */

/*
 * OUTCOME 2. Put the answer in the LOCK DATABASE first, then re-read it: the
 * retry's destination, its master_csid field and its to_directory flag all come
 * back out of the executive. The CSID is never carried from the reply frame
 * into the request frame inside this object.
 */
static void h_redirect(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;
	struct vms_dlm_proxy_post p;

	if (r->redirects >= (uint8_t)DLM_REQ_MAX_REDIRECTS) {
		dq_fail(f, r, DLM_REQ_FAIL_UNROUTABLE);
		e->st = DLM_REQ_E_NODIR;
		return;
	}

	/* The name the engine is told about comes from the engine too. */
	dq_bzero(&p, (uint32_t)sizeof(p));
	if (f->ops->refill_post(f->ops->ctx, r->req_lkid, r->post_op,
				r->dst_csid, &p) != 0) {
		f->lock_gone++;
		dq_free(r);
		e->st = DLM_REQ_E_NOLOCK;
		return;
	}
	if (f->ops->record_master(f->ops->ctx, p.resnam, r->req_lkid,
				  e->master_csid) != 0) {
		e->st = DLM_REQ_E_INVAL;
		return;
	}

	/* THE RE-READ. Everything the retry needs now comes from the LKB the
	 * record above just updated. */
	dq_bzero(&p, (uint32_t)sizeof(p));
	if (f->ops->refill_post(f->ops->ctx, r->req_lkid, r->post_op, 0u,
				&p) != 0) {
		f->lock_gone++;
		dq_free(r);
		e->st = DLM_REQ_E_NOLOCK;
		return;
	}

	r->dst_csid     = p.master_csid;
	r->to_directory = p.to_directory;
	r->tries        = 0u;
	r->redirects++;
	r->state        = (uint8_t)DLM_REQ_ST_ENQ;

	e->st = dq_transmit(f, r, &p);
	if (e->st == DLM_REQ_OK)
		f->redirects_followed++;
	else
		dq_fail(f, r, DLM_REQ_FAIL_UNROUTABLE);
}

/*
 * OUTCOME 3: "no master -- you master it". The engine promotes the proxy onto
 * res->waiting and runs the LOCAL granting algorithm, so the $ENQW completes
 * from a genuine grant this node's own queues produced. Nothing is sent; there
 * is nobody to send to.
 */
static void h_assume(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;
	struct vms_dlm_proxy_post p;

	dq_bzero(&p, (uint32_t)sizeof(p));
	if (f->ops->refill_post(f->ops->ctx, r->req_lkid, r->post_op,
				r->dst_csid, &p) != 0) {
		f->lock_gone++;
		dq_free(r);
		e->st = DLM_REQ_E_NOLOCK;
		return;
	}
	if (f->ops->assume_mastery(f->ops->ctx, p.resnam, r->req_lkid) != 0) {
		e->st = DLM_REQ_E_INVAL;
		return;
	}
	f->masteries_assumed++;
	dq_free(r);       /* it is a LOCAL lock now: no wire record belongs */
	e->st = DLM_REQ_OK;
}

/* ---- BLKAST ----------------------------------------------------------- */

/* The master is blocked behind our lock. Fire the holder's REAL user-mode AST
 * through the engine, which is the only thing that knows whether a holder
 * registered one. A refusal is honest and counted; nothing is faked. */
static void h_blkast(struct dlm_req_fsm *f, struct dq_ev *e)
{
	f->blkasts_rx++;
	if (f->ops->blkast_deliver(f->ops->ctx, e->r->req_lkid) != 0) {
		f->blkasts_undeliverable++;
		e->st = DLM_REQ_E_NOLOCK;
		return;
	}
	f->blkasts_delivered++;
	e->st = DLM_REQ_OK;
}

/* ---- the deadline ------------------------------------------------------ */

/* A request that has been transmitted its whole ladder without an answer is
 * FAILED with a real terminal status, so the $ENQW returns to its caller. */
static void h_timeout_request(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;

	if (r->tries >= (uint8_t)DLM_REQ_MAX_TRIES) {
		f->timeouts_failed++;
		dq_fail(f, r, DLM_REQ_FAIL_TIMEOUT);
		e->st = DLM_REQ_OK;
		return;
	}
	e->st = dq_refill_transmit(f, r);
	if (e->st == DLM_REQ_OK) {
		f->retransmits++;
		return;
	}
	if (e->st == DLM_REQ_E_NOLOCK) {
		dq_free(r);   /* the lock went away under us: abandon quietly */
		return;
	}
	/*
	 * The attempt was refused before a frame was built (no wire opcode, no
	 * hash, no destination), so dq_transmit did not count a try. Count one
	 * here anyway: a request that can never be transmitted must still walk
	 * off the end of the ladder and be FAILED, not retried forever on every
	 * beat. A bounded wrong answer beats an unbounded silent one.
	 */
	if (r->tries < 0xffu)
		r->tries++;
}

/*
 * A deadline on a lock this node HOLDS.
 *
 * Since the supersession (§3) a grant settles immediately, and the beat skips
 * a settled block -- so the only way to arrive here is the one path that
 * leaves ST_GRANTED with `settled` clear: a CONVERT on a held lock whose frame
 * could not be transmitted (h_post_convert_granted's failure branch). The
 * convert is still owed, so it is retried from a FRESH executive read, exactly
 * like any other outstanding request.
 *
 * WHAT IS DIFFERENT FROM h_timeout_request IS THE END OF THE LADDER: a GRANTED
 * lock is REAL, so a spent ladder may NOT fail it. The wire record is dropped
 * and the fact is counted; the lock itself is the engine's, and the engine
 * holds the same rule. A timeout may not take away a lock the master granted.
 */
static void h_timeout_granted(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;

	if (r->tries >= (uint8_t)DLM_REQ_MAX_TRIES) {
		f->timeouts_failed++;
		dq_free(r);
		e->st = DLM_REQ_OK;
		return;
	}
	e->st = dq_refill_transmit(f, r);
	if (e->st == DLM_REQ_OK) {
		f->retransmits++;
		return;
	}
	if (e->st == DLM_REQ_E_NOLOCK) {
		dq_free(r);   /* the lock went away under us: abandon quietly */
		return;
	}
	/* Refused before a frame was built, so dq_transmit counted no try.
	 * Count one here so the ladder still terminates (h_timeout_request
	 * carries the same argument at length). */
	if (r->tries < 0xffu)
		r->tries++;
}

/* ---- a member left ----------------------------------------------------- */

static void h_peer_gone_pending(struct dlm_req_fsm *f, struct dq_ev *e)
{
	f->peers_gone++;
	dq_fail(f, e->r, DLM_REQ_FAIL_PATHLOST);
	e->st = DLM_REQ_OK;
}

/* A GRANTED lock whose master left is the ENGINE's business (its own
 * member-departure path owns what happens to the LKB). All this layer does is
 * drop a wire record that can no longer be true. */
static void h_peer_gone_granted(struct dlm_req_fsm *f, struct dq_ev *e)
{
	f->peers_gone++;
	dq_free(e->r);
	e->st = DLM_REQ_OK;
}

/* ==========================================================================
 * 7. The table
 *
 * An EMPTY CELL IS COUNTED, NOT GUESSED (`ignored_events`).
 * ========================================================================== */
static const dq_handler_t dq_table[DLM_REQ_ST__COUNT][DLM_REQ_EV__COUNT] = {
	/* ST_IDLE: only a fresh post can start a request. */
	[DLM_REQ_ST_IDLE] = {
		[DLM_REQ_EV_ENQ]     = h_post_new,
		[DLM_REQ_EV_CONVERT] = h_post_new,
		[DLM_REQ_EV_DEQ]     = h_post_deq
	},
	/* ST_LOOKUP: outstanding at the DIRECTORY node. A grant can arrive
	 * here -- p. 6-31 outcome 1, where the directory node IS the master. */
	[DLM_REQ_ST_LOOKUP] = {
		[DLM_REQ_EV_ENQ]       = h_post_again,
		[DLM_REQ_EV_CONVERT]   = h_post_again,
		[DLM_REQ_EV_DEQ]       = h_post_deq,
		[DLM_REQ_EV_GRANT]     = h_grant,
		[DLM_REQ_EV_DENY]      = h_deny_lookup,
		[DLM_REQ_EV_REDIRECT]  = h_redirect,
		[DLM_REQ_EV_ASSUME]    = h_assume,
		[DLM_REQ_EV_TIMEOUT]   = h_timeout_request,
		[DLM_REQ_EV_PEER_GONE] = h_peer_gone_pending
	},
	/* ST_ENQ: outstanding at the MASTER. */
	[DLM_REQ_ST_ENQ] = {
		[DLM_REQ_EV_ENQ]       = h_post_again,
		[DLM_REQ_EV_CONVERT]   = h_post_again,
		[DLM_REQ_EV_DEQ]       = h_post_deq,
		[DLM_REQ_EV_GRANT]     = h_grant,
		[DLM_REQ_EV_DENY]      = h_deny_master,
		[DLM_REQ_EV_REDIRECT]  = h_redirect,
		[DLM_REQ_EV_ASSUME]    = h_assume,
		[DLM_REQ_EV_TIMEOUT]   = h_timeout_request,
		[DLM_REQ_EV_PEER_GONE] = h_peer_gone_pending
	},
	/* ST_GRANTED: the lock is real and this node holds it. */
	[DLM_REQ_ST_GRANTED] = {
		[DLM_REQ_EV_CONVERT]   = h_post_convert_granted,
		[DLM_REQ_EV_DEQ]       = h_post_deq,
		[DLM_REQ_EV_GRANT]     = h_grant_dup,
		[DLM_REQ_EV_BLKAST]    = h_blkast,
		[DLM_REQ_EV_TIMEOUT]   = h_timeout_granted,
		[DLM_REQ_EV_PEER_GONE] = h_peer_gone_granted
	}
};

static enum dlm_req_status dq_dispatch(struct dlm_req_fsm *f,
				       struct dlm_req *r,
				       enum dlm_req_event ev,
				       struct dq_ev *e)
{
	dq_handler_t h;

	if ((unsigned)r->state >= (unsigned)DLM_REQ_ST__COUNT ||
	    (unsigned)ev >= (unsigned)DLM_REQ_EV__COUNT) {
		f->ignored_events++;
		return DLM_REQ_E_STATE;
	}
	h = dq_table[r->state][ev];
	if (h == (dq_handler_t)0) {
		f->ignored_events++;   /* an empty cell is COUNTED, not guessed */
		return DLM_REQ_E_STATE;
	}
	e->r = r;
	e->st = DLM_REQ_OK;
	h(f, e);
	return e->st;
}

/* ==========================================================================
 * 8. Lifecycle
 * ========================================================================== */
void dlm_req_fsm_init(struct dlm_req_fsm *f, const struct dlm_req_ops *ops)
{
	if (f == (struct dlm_req_fsm *)0)
		return;
	dq_bzero(f, (uint32_t)sizeof(*f));
	f->ops = ops;
}

/* Are the ops complete enough to act at all? A missing door is a refusal, never
 * a step this file takes on its own. */
static int dq_ops_ok(const struct dlm_req_fsm *f)
{
	const struct dlm_req_ops *o = f->ops;

	return o != (const struct dlm_req_ops *)0 &&
	       o->send != (int (*)(void *, vms_csid_t, const uint8_t *,
				   uint32_t))0 &&
	       o->refill_post != (int (*)(void *, uint32_t, uint32_t,
					  vms_csid_t,
					  struct vms_dlm_proxy_post *))0 &&
	       o->grant_recv != (int (*)(void *,
					 const struct vms_dlm_proxy_grant *))0 &&
	       o->record_master != (int (*)(void *, const char *, uint32_t,
					    vms_csid_t))0 &&
	       o->assume_mastery != (int (*)(void *, const char *, uint32_t))0 &&
	       o->blkast_deliver != (int (*)(void *, uint32_t))0;
}

/* ==========================================================================
 * 9. The OUTBOUND event
 * ========================================================================== */
static enum dlm_req_event dq_event_for_post(uint32_t op)
{
	if (op == VMS_DLM_POST_CONVERT)
		return DLM_REQ_EV_CONVERT;
	if (op == VMS_DLM_POST_DEQ)
		return DLM_REQ_EV_DEQ;
	return DLM_REQ_EV_ENQ;
}

enum dlm_req_status dlm_req_fsm_post(struct dlm_req_fsm *f,
				     const struct vms_dlm_proxy_post *p)
{
	struct dlm_req *r;
	struct dq_ev e;

	if (f == (struct dlm_req_fsm *)0 || p == (const struct vms_dlm_proxy_post *)0)
		return DLM_REQ_E_INVAL;
	if (!dq_ops_ok(f))
		return DLM_REQ_E_INVAL;
	if (p->req_lkid == VMS_DLM_LKID_UNSET)
		return DLM_REQ_E_INVAL;   /* the engine's own refusal, mirrored */

	dq_bzero(&e, (uint32_t)sizeof(e));
	e.post = p;

	r = dq_find(f, p->req_lkid);
	if (r == (struct dlm_req *)0) {
		if (p->op == VMS_DLM_POST_DEQ) {
			/* No wire RECORD to release -- but the lock is the
			 * engine's, not this object's, so the release is still
			 * real and still transmitted (h_post_deq's second arm).
			 * A block is never allocated for one: a $DEQ waits for
			 * no reply. */
			e.r = (struct dlm_req *)0;
			h_post_deq(f, &e);
			return e.st;
		}
		r = dq_alloc(f, p->req_lkid);
		if (r == (struct dlm_req *)0)
			return DLM_REQ_E_NOSLOT;
	}
	return dq_dispatch(f, r, dq_event_for_post(p->op), &e);
}

/* ==========================================================================
 * 10. The INBOUND events
 * ========================================================================== */

/*
 * Which request does this reply belong to?
 *
 * TWO CORRELATORS, AND THE GROUNDED ONE WINS. `correlated_lkid` is what the
 * CONNECTION MANAGER matched through its own transaction envelope (spec §4(j)
 * -- send/ack counters and a transaction token), which is the correlation a
 * real VAX requires and the only one this tree has grounded for a reply.
 * body[20] is the other candidate: §4(f).1 reads a GRANT's body[20] as "the
 * requester's real assigned lock-id", which is the value THIS node put on the
 * request -- true for OVMX's own requests, and not something to rely on when a
 * foreign master rewrites the field. So the envelope's answer is used when
 * there is one, and body[20] only when there is not.
 */
static struct dlm_req *dq_match_reply(struct dlm_req_fsm *f,
				      uint32_t correlated_lkid,
				      const struct vms_dlm_enq_response *rsp)
{
	struct dlm_req *r;

	if (correlated_lkid != VMS_DLM_LKID_UNSET) {
		r = dq_find(f, correlated_lkid);
		if (r != (struct dlm_req *)0)
			return r;
	}
	return dq_find(f, rsp->req_lkid);
}

enum dlm_req_status dlm_req_fsm_reply(struct dlm_req_fsm *f,
				      vms_csid_t from_csid,
				      uint32_t correlated_lkid,
				      const uint8_t *frame, uint32_t len)
{
	struct vms_frame_info fi;
	struct vms_dlm_enq_response rsp;
	struct dlm_req *r;
	struct dq_ev e;

	if (f == (struct dlm_req_fsm *)0 || frame == (const uint8_t *)0)
		return DLM_REQ_E_INVAL;
	if (!dq_ops_ok(f))
		return DLM_REQ_E_INVAL;

	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK ||
	    vms_dlm_enq_response_parse(frame, len, &fi, &rsp) != VMS_CODEC_OK) {
		f->replies_unparsed++;
		return DLM_REQ_E_CODEC;
	}

	/* Every cat-0x02 frame is a chance to learn a hash (E49). */
	(void)dlm_req_fsm_observe(f, frame, len);

	r = dq_match_reply(f, correlated_lkid, &rsp);
	if (r == (struct dlm_req *)0) {
		f->replies_unmatched++;
		return DLM_REQ_E_NOLOCK;
	}

	dq_bzero(&e, (uint32_t)sizeof(e));
	e.rsp = &rsp;
	e.from_csid = from_csid;
	return dq_dispatch(f, r,
			   rsp.outcome == VMS_DLM_ENQ_GRANTED
				   ? DLM_REQ_EV_GRANT
				   : DLM_REQ_EV_DENY,
			   &e);
}

/* One small entry per event that has no grounded frame shape to parse. */
static enum dlm_req_status dq_entry(struct dlm_req_fsm *f, uint32_t req_lkid,
				    enum dlm_req_event ev, struct dq_ev *e)
{
	struct dlm_req *r;

	if (f == (struct dlm_req_fsm *)0 || !dq_ops_ok(f))
		return DLM_REQ_E_INVAL;
	r = dq_find(f, req_lkid);
	if (r == (struct dlm_req *)0) {
		f->replies_unmatched++;
		return DLM_REQ_E_NOLOCK;
	}
	return dq_dispatch(f, r, ev, e);
}

enum dlm_req_status dlm_req_fsm_redirect(struct dlm_req_fsm *f,
					 uint32_t req_lkid,
					 vms_csid_t master_csid)
{
	struct dq_ev e;

	if (master_csid == 0u)
		return DLM_REQ_E_INVAL;   /* 0 means "unmastered": no answer */
	dq_bzero(&e, (uint32_t)sizeof(e));
	e.master_csid = master_csid;
	return dq_entry(f, req_lkid, DLM_REQ_EV_REDIRECT, &e);
}

enum dlm_req_status dlm_req_fsm_assume_mastery(struct dlm_req_fsm *f,
					       uint32_t req_lkid)
{
	struct dq_ev e;

	dq_bzero(&e, (uint32_t)sizeof(e));
	return dq_entry(f, req_lkid, DLM_REQ_EV_ASSUME, &e);
}

enum dlm_req_status dlm_req_fsm_decline(struct dlm_req_fsm *f,
					uint32_t req_lkid)
{
	struct dq_ev e;
	struct dlm_req *r;

	if (f == (struct dlm_req_fsm *)0 || !dq_ops_ok(f))
		return DLM_REQ_E_INVAL;
	r = dq_find(f, req_lkid);
	if (r == (struct dlm_req *)0) {
		f->replies_unmatched++;
		return DLM_REQ_E_NOLOCK;
	}
	/* A decline is the DENY event: the two arrive differently on the wire
	 * (a deny is a parsed cat-0x82 shape, a decline is whatever the
	 * classifier recognised) and mean the same thing to this FSM -- "that
	 * target did not serve it". One handler, one behaviour. */
	dq_bzero(&e, (uint32_t)sizeof(e));
	return dq_dispatch(f, r, DLM_REQ_EV_DENY, &e);
}

enum dlm_req_status dlm_req_fsm_blkast(struct dlm_req_fsm *f, uint32_t req_lkid)
{
	struct dq_ev e;

	dq_bzero(&e, (uint32_t)sizeof(e));
	return dq_entry(f, req_lkid, DLM_REQ_EV_BLKAST, &e);
}

uint32_t dlm_req_fsm_peer_gone(struct dlm_req_fsm *f, vms_csid_t csid)
{
	struct dq_ev e;
	uint32_t i, n = 0u;

	if (f == (struct dlm_req_fsm *)0 || csid == 0u)
		return 0u;
	for (i = 0u; i < DLM_REQ_MAX; i++) {
		if (f->req[i].state == (uint8_t)DLM_REQ_ST_IDLE ||
		    f->req[i].dst_csid != csid)
			continue;
		dq_bzero(&e, (uint32_t)sizeof(e));
		if (dq_dispatch(f, &f->req[i], DLM_REQ_EV_PEER_GONE, &e) ==
		    DLM_REQ_OK)
			n++;
	}
	return n;
}

/* ==========================================================================
 * 11. THE HASH LEARNER (integration note E49)
 *
 * Davis p. 6-50: a lookup carries the SENDER's own 16-bit hash and the
 * receiving system uses the RECEIVED value. This is where OVMX receives it.
 * Both halves must come from the SAME frame -- a hash is a property of a name,
 * so a hash learned against a name from somewhere else would be worse than no
 * hash at all.
 * ========================================================================== */

/* The root name a cat-0x02 frame carries, if it carries one. Requests and the
 * DENY reply shape echo it; a GRANT does not (spec §4(f).1). */
static int dq_frame_name(const uint8_t *frame, uint32_t len,
			 const struct vms_frame_info *fi, char *out)
{
	struct vms_dlm_enq_request req;
	struct vms_dlm_enq_response rsp;
	struct vms_dlm_rebuild_record rec;
	uint8_t opcode = 0u;

	if (vms_dlm_enq_request_parse(frame, len, fi, &opcode, &req) ==
	    VMS_CODEC_OK) {
		if (req.name_len == 0u)
			return -1;
		dq_name_to_cstr(req.name, req.name_len, out);
		return 0;
	}
	if (vms_dlm_rebuild_parse(frame, len, fi, &rec) == VMS_CODEC_OK) {
		if (rec.name_len == 0u)
			return -1;
		dq_name_to_cstr(rec.name, rec.name_len, out);
		return 0;
	}
	if (vms_dlm_enq_response_parse(frame, len, fi, &rsp) == VMS_CODEC_OK) {
		if (rsp.name_len == 0u)
			return -1;   /* a GRANT echoes no name */
		dq_name_to_cstr(rsp.name, rsp.name_len, out);
		return 0;
	}
	return -1;
}

/* The body twin of dq_frame_name (rd vms-1ee): the same three shapes, read out
 * of the 132 bytes SCS delivers instead of a captured frame. */
static int dq_body_name(const uint8_t *body, uint32_t len, char *out)
{
	struct vms_dlm_enq_request req;
	struct vms_dlm_enq_response rsp;
	struct vms_dlm_rebuild_record rec;
	uint8_t opcode = 0u;

	if (vms_dlm_enq_request_parse_body(body, len, &opcode, &req) ==
	    VMS_CODEC_OK) {
		if (req.name_len == 0u)
			return -1;
		dq_name_to_cstr(req.name, req.name_len, out);
		return 0;
	}
	if (vms_dlm_rebuild_parse_body(body, len, &rec) == VMS_CODEC_OK) {
		if (rec.name_len == 0u)
			return -1;
		dq_name_to_cstr(rec.name, rec.name_len, out);
		return 0;
	}
	if (vms_dlm_enq_response_parse_body(body, len, &rsp) == VMS_CODEC_OK) {
		if (rsp.name_len == 0u)
			return -1;
		dq_name_to_cstr(rsp.name, rsp.name_len, out);
		return 0;
	}
	return -1;
}

/*
 * THE INBOUND ENTRIES SCS ACTUALLY REACHES (rd vms-1ee).
 *
 * cnxman_vc_message() hands a SYSAP its own 132 bytes and nothing below them
 * (design sec 3.2.4), so the frame-taking entries above cannot be called from
 * the live receive path at all -- handing them a body is integration note E73,
 * which does not fail loudly, it silently refuses every real message. These
 * take what SCS delivers. No frame is synthesised around the body to make a
 * classifier pass: a body that arrived on the VMS$VAXcluster connection is
 * already known to be one, and the codec's own category/opcode checks are what
 * reject a body that is not what it claims.
 */
uint32_t dlm_req_fsm_observe_body(struct dlm_req_fsm *f, const uint8_t *body,
				  uint32_t len)
{
	char name[VMS_DLM_NAME_MAX + 1u];
	uint16_t hash = 0u;

	if (f == (struct dlm_req_fsm *)0 || body == (const uint8_t *)0)
		return 0u;
	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->learn_dir_hash == (int (*)(void *, const char *,
					       uint16_t))0)
		return 0u;
	if (vms_dlm_dir_hash_parse_body(body, len, &hash) != VMS_CODEC_OK)
		return 0u;
	if (dq_body_name(body, len, name) != 0)
		return 0u;
	if (f->ops->learn_dir_hash(f->ops->ctx, name, hash) != 0)
		return 0u;
	f->hashes_learned++;
	return 1u;
}

enum dlm_req_status dlm_req_fsm_reply_body(struct dlm_req_fsm *f,
					   vms_csid_t from_csid,
					   uint32_t correlated_lkid,
					   const uint8_t *body, uint32_t len)
{
	struct vms_dlm_enq_response rsp;
	struct dlm_req *r;
	struct dq_ev e;

	if (f == (struct dlm_req_fsm *)0 || body == (const uint8_t *)0)
		return DLM_REQ_E_INVAL;
	if (!dq_ops_ok(f))
		return DLM_REQ_E_INVAL;

	if (vms_dlm_enq_response_parse_body(body, len, &rsp) != VMS_CODEC_OK) {
		f->replies_unparsed++;
		return DLM_REQ_E_CODEC;
	}

	/* Every cat-0x02 body is a chance to learn a hash (E49). */
	(void)dlm_req_fsm_observe_body(f, body, len);

	r = dq_match_reply(f, correlated_lkid, &rsp);
	if (r == (struct dlm_req *)0) {
		f->replies_unmatched++;
		return DLM_REQ_E_NOLOCK;
	}

	dq_bzero(&e, (uint32_t)sizeof(e));
	e.rsp = &rsp;
	e.from_csid = from_csid;
	return dq_dispatch(f, r,
			   rsp.outcome == VMS_DLM_ENQ_GRANTED
				   ? DLM_REQ_EV_GRANT
				   : DLM_REQ_EV_DENY,
			   &e);
}

/*
 * THE BLOCKING AST's RECEIVE HALF (rd vms-c72), as SCS delivers it: 132 bytes,
 * category 0x02, opcode 0x04. It is the body twin of dlm_req_fsm_blkast() and
 * exists for the same reason dlm_req_fsm_reply_body() does -- the frame-taking
 * entry cannot be reached from the live receive path (integration note E73).
 *
 * THE LOCK IS FOUND BY A HANDLE THIS EXECUTIVE MINTED. body[20:24] is the
 * HOLDER's own lock id -- the value this node put on its own op-0x01 request
 * and that the master stamped on the LKB it granted -- so `dq_find` looks up a
 * request of OURS by OUR OWN key. The master's handle at body[24:28] is read by
 * the codec and asserted about nothing: this arm has no table keyed by it.
 *
 * THREE REFUSALS, EACH COUNTED AND NONE OF THEM A FABRICATION:
 *   - a body the codec will not parse (wrong category, wrong opcode, or EITHER
 *     lock id zero -- the fc8540ae refusal lives in the codec and this entry
 *     inherits it) raises `blkasts_unparsed` and delivers nothing;
 *   - a parsed BLKAST naming no request of ours raises `replies_unmatched`
 *     (dq_entry) and delivers nothing -- an AST is never fired at a lock this
 *     node cannot name;
 *   - a holder that registered no blocking AST is the ENGINE's answer, not
 *     this layer's: h_blkast counts `blkasts_undeliverable`.
 */
enum dlm_req_status dlm_req_fsm_blkast_body(struct dlm_req_fsm *f,
					    vms_csid_t from_csid,
					    const uint8_t *body, uint32_t len)
{
	struct vms_dlm_blkast b;

	(void)from_csid;   /* the sender is the master; the LOCK is the key */
	if (f == (struct dlm_req_fsm *)0 || body == (const uint8_t *)0)
		return DLM_REQ_E_INVAL;
	if (!dq_ops_ok(f))
		return DLM_REQ_E_INVAL;

	if (vms_dlm_blkast_parse_body(body, len, &b) != VMS_CODEC_OK) {
		f->blkasts_unparsed++;
		return DLM_REQ_E_CODEC;
	}
	return dlm_req_fsm_blkast(f, b.req_lkid);
}

uint32_t dlm_req_fsm_observe(struct dlm_req_fsm *f, const uint8_t *frame,
			     uint32_t len)
{
	struct vms_frame_info fi;
	char name[VMS_DLM_NAME_MAX + 1u];
	uint16_t hash = 0u;

	if (f == (struct dlm_req_fsm *)0 || frame == (const uint8_t *)0)
		return 0u;
	if (f->ops == (const struct dlm_req_ops *)0 ||
	    f->ops->learn_dir_hash == (int (*)(void *, const char *,
					       uint16_t))0)
		return 0u;
	if (vms_frame_classify(frame, len, &fi) != VMS_CODEC_OK)
		return 0u;
	if (vms_dlm_dir_hash_parse(frame, len, &fi, &hash) != VMS_CODEC_OK)
		return 0u;
	if (dq_frame_name(frame, len, &fi, name) != 0)
		return 0u;

	if (f->ops->learn_dir_hash(f->ops->ctx, name, hash) != 0)
		return 0u;   /* refused (a conflicting value) -- the engine
			      * counts it; the first value stands */
	f->hashes_learned++;
	return 1u;
}

/* ==========================================================================
 * 12. The beat
 * ========================================================================== */
uint32_t dlm_req_fsm_tick(struct dlm_req_fsm *f)
{
	struct dq_ev e;
	uint32_t i, now, sent = 0u;

	if (f == (struct dlm_req_fsm *)0 || !dq_ops_ok(f))
		return 0u;
	now = dq_now(f);

	for (i = 0u; i < DLM_REQ_MAX; i++) {
		struct dlm_req *r = &f->req[i];
		uint32_t before;

		if (r->state == (uint8_t)DLM_REQ_ST_IDLE || r->settled)
			continue;   /* settled: the answer arrived and was sent */
		if ((uint32_t)(now - r->sent_ms) < DLM_REQ_RETRY_MS)
			continue;
		before = f->retransmits;
		dq_bzero(&e, (uint32_t)sizeof(e));
		(void)dq_dispatch(f, r, DLM_REQ_EV_TIMEOUT, &e);
		if (f->retransmits != before)
			sent++;
	}
	return sent;
}

/* ==========================================================================
 * 13. Readback
 * ========================================================================== */
const struct dlm_req *dlm_req_fsm_at(const struct dlm_req_fsm *f,
				     uint32_t index)
{
	if (f == (const struct dlm_req_fsm *)0 || index >= DLM_REQ_MAX)
		return (const struct dlm_req *)0;
	return &f->req[index];
}

const struct dlm_req *dlm_req_fsm_find(const struct dlm_req_fsm *f,
				       uint32_t req_lkid)
{
	uint32_t i;

	if (f == (const struct dlm_req_fsm *)0 ||
	    req_lkid == VMS_DLM_LKID_UNSET)
		return (const struct dlm_req *)0;
	for (i = 0u; i < DLM_REQ_MAX; i++) {
		if (f->req[i].state != (uint8_t)DLM_REQ_ST_IDLE &&
		    f->req[i].req_lkid == req_lkid)
			return &f->req[i];
	}
	return (const struct dlm_req *)0;
}

uint32_t dlm_req_fsm_outstanding(const struct dlm_req_fsm *f)
{
	uint32_t i, n = 0u;

	if (f == (const struct dlm_req_fsm *)0)
		return 0u;
	for (i = 0u; i < DLM_REQ_MAX; i++) {
		if (f->req[i].state != (uint8_t)DLM_REQ_ST_IDLE)
			n++;
	}
	return n;
}

const char *dlm_req_state_name(enum dlm_req_state s)
{
	switch (s) {
	case DLM_REQ_ST_IDLE:    return "IDLE";
	case DLM_REQ_ST_LOOKUP:  return "LOOKUP";
	case DLM_REQ_ST_ENQ:     return "ENQ";
	case DLM_REQ_ST_GRANTED: return "GRANTED";
	default:                 return "?";
	}
}

/* ==========================================================================
 * 13. THE RELEASE QUEUE (rd vms-49f8)
 *
 * The thread crossing a $DEQ needs and no other operation does: the release
 * DESTROYS the lock block a later refill would read, so the fork thread emits
 * from what the engine really read at release time. The header's §12 carries
 * the full rationale -- above all why a snapshot of a completed release is not
 * a cache, and why the record holds four fields and not a post's worth.
 *
 * Pure: no lock, no clock, no call out. The glue owns the instance and the
 * serialisation.
 * ========================================================================== */

void dlm_relq_init(struct dlm_relq *q)
{
	if (q == (struct dlm_relq *)0)
		return;
	dq_bzero(q, (uint32_t)sizeof(*q));
	q->next_seq = 1u;   /* 0 is "no staging": never handed out */
}

/* The next generation stamp. Wraps past 0, because 0 is the free marker and a
 * slot stamped 0 would be claimable by a work item that named nothing. */
static uint32_t relq_next_seq(struct dlm_relq *q)
{
	uint32_t s = q->next_seq++;

	if (q->next_seq == 0u)
		q->next_seq = 1u;
	return s == 0u ? 1u : s;
}

/* The first free slot, or DLM_RELQ_SLOTS when the queue is full. */
static uint32_t relq_free_slot(const struct dlm_relq *q)
{
	uint32_t i;

	for (i = 0u; i < DLM_RELQ_SLOTS; i++) {
		if (q->slot[i].busy == 0u)
			return i;
	}
	return DLM_RELQ_SLOTS;
}

/*
 * Is this post a RELEASE this queue may stage? A post of another operation does
 * not belong here (those refill), and a release with no handle of our own is
 * the engine's own refusal mirrored -- the value that is never a lock id.
 */
static int relq_post_is_release(const struct vms_dlm_proxy_post *p)
{
	return p != (const struct vms_dlm_proxy_post *)0 &&
	       p->op == VMS_DLM_POST_DEQ &&
	       p->req_lkid != VMS_DLM_LKID_UNSET;
}

/* The snapshot itself: four executive reads out of the post, and nothing else
 * is copied -- see the header's "FOUR FIELDS" note. */
static void relq_record_from_post(struct dlm_relq_rec *rec,
				  const struct vms_dlm_proxy_post *p)
{
	rec->req_lkid    = p->req_lkid;
	rec->master_lkid = p->master_lkid;
	rec->dst_csid    = p->dst_csid;
	rec->mode        = (uint8_t)p->lkmode;
}

enum dlm_req_status dlm_relq_stage(struct dlm_relq *q,
				   const struct vms_dlm_proxy_post *p,
				   uint32_t *out_slot, uint32_t *out_seq)
{
	uint32_t i;

	if (q == (struct dlm_relq *)0 || out_slot == (uint32_t *)0 ||
	    out_seq == (uint32_t *)0)
		return DLM_REQ_E_INVAL;
	if (!relq_post_is_release(p))
		return DLM_REQ_E_INVAL;

	i = relq_free_slot(q);
	if (i >= DLM_RELQ_SLOTS) {
		q->full_refused++;
		return DLM_REQ_E_NOSLOT;
	}

	dq_bzero(&q->slot[i], (uint32_t)sizeof(q->slot[i]));
	relq_record_from_post(&q->slot[i].rec, p);
	q->slot[i].seq  = relq_next_seq(q);
	q->slot[i].busy = 1u;
	q->staged++;

	*out_slot = i;
	*out_seq  = q->slot[i].seq;
	return DLM_REQ_OK;
}

/* Does (slot, seq) name a staged release? The whole anti-double-emit rule. */
static int relq_slot_matches(const struct dlm_relq *q, uint32_t slot,
			     uint32_t seq)
{
	return slot < DLM_RELQ_SLOTS && seq != 0u &&
	       q->slot[slot].busy != 0u && q->slot[slot].seq == seq;
}

static void relq_slot_free(struct dlm_relq *q, uint32_t slot)
{
	dq_bzero(&q->slot[slot], (uint32_t)sizeof(q->slot[slot]));
}

/*
 * The post the FSM takes, built from the record ALONE. Zeroed first, so every
 * field a release does not carry is a zero this function wrote rather than a
 * value some earlier request left behind: `to_directory` 0 (a release is
 * addressed to the master), `dir_hash_known` 0 (a release carries no directory
 * index), no resource name and no value block.
 */
static void relq_post_from_record(struct vms_dlm_proxy_post *out,
				  const struct dlm_relq_rec *rec)
{
	dq_bzero(out, (uint32_t)sizeof(*out));
	out->op          = VMS_DLM_POST_DEQ;
	out->dst_csid    = rec->dst_csid;
	out->req_lkid    = rec->req_lkid;
	out->master_lkid = rec->master_lkid;
	out->lkmode      = rec->mode;
}

enum dlm_req_status dlm_relq_claim(struct dlm_relq *q, uint32_t slot,
				   uint32_t seq, struct vms_dlm_proxy_post *out)
{
	if (q == (struct dlm_relq *)0 || out == (struct vms_dlm_proxy_post *)0)
		return DLM_REQ_E_INVAL;
	if (!relq_slot_matches(q, slot, seq)) {
		q->stale_refused++;
		return DLM_REQ_E_NOLOCK;
	}

	relq_post_from_record(out, &q->slot[slot].rec);
	relq_slot_free(q, slot);
	q->claimed++;
	return DLM_REQ_OK;
}

void dlm_relq_abandon(struct dlm_relq *q, uint32_t slot, uint32_t seq)
{
	if (q == (struct dlm_relq *)0)
		return;
	if (!relq_slot_matches(q, slot, seq)) {
		q->stale_refused++;
		return;
	}
	relq_slot_free(q, slot);
	q->abandoned++;
}

uint32_t dlm_relq_pending(const struct dlm_relq *q)
{
	uint32_t i, n = 0u;

	if (q == (const struct dlm_relq *)0)
		return 0u;
	for (i = 0u; i < DLM_RELQ_SLOTS; i++) {
		if (q->slot[i].busy != 0u)
			n++;
	}
	return n;
}
