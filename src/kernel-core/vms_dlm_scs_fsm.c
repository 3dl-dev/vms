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

/* VMS_DLM_POST_* -> the GROUNDED cat-0x02 wire opcode, or 0 for "none".
 * A release has NO grounded opcode (header §"WHAT IS GROUNDED"), and 0 is how
 * that is said -- not a guess at 0x03, which is the PROVISIONAL commit. */
static uint8_t dq_wireop(uint32_t post_op)
{
	if (post_op == VMS_DLM_POST_ENQ)
		return (uint8_t)VMS_DLM_WIREOP_ENQ;
	if (post_op == VMS_DLM_POST_CONVERT)
		return (uint8_t)VMS_DLM_WIREOP_CONVERT;
	return 0u;
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

	dq_bzero(&req, (uint32_t)sizeof(req));
	req.mode            = (uint8_t)p->lkmode;
	req.req_pid_or_lkid = p->req_lkid;      /* our own executive handle */
	req.master_lkid     = p->master_lkid;   /* 0 until the master named it */
	req.dir_hash        = p->dir_hash;
	req.dir_hash_valid  = p->dir_hash_known;
	req.name_len        = dq_name_from_post(p, req.name);

	/*
	 * THE LOCK VALUE BLOCK IS NOT HERE, and that is deliberate. `p->valblk`
	 * holds the LKB's real bytes, but no cat-0x02 LVB field is grounded
	 * (vms_cluster_codec_dlm.h), so there is nowhere honest to put them.
	 * Counted, so the omission is a number in a diagnostic rather than a
	 * silence.
	 */
	{
		uint32_t i;

		for (i = 0u; i < VMS_DLM_VALBLK_LEN; i++) {
			if (p->valblk[i] != 0u) {
				f->lvb_write_no_wire_field++;
				break;
			}
		}
	}

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

	if (wireop == 0u) {
		f->releases_no_wire_op++;
		dq_log(f, "%CNXMAN, cross-node lock release not sent: no "
			  "grounded cat-02 opcode for a DLM dequeue");
		return DLM_REQ_E_NOWIREOP;
	}
	if (r->dst_csid == 0u) {
		f->dir_unresolved++;
		return DLM_REQ_E_NODIR;
	}
	if (r->to_directory && !p->dir_hash_known) {
		/* The refusal that cures the grant storm: never a lookup with a
		 * hash this node did not receive from the cluster. */
		f->hash_unknown_refused++;
		dq_log(f, "%CNXMAN, directory lookup refused: no wire-learned "
			  "hash for this resource name");
		return DLM_REQ_E_NOHASH;
	}

	st = dq_build_request(f, p, wireop);
	if (st != DLM_REQ_OK)
		return st;
	st = dq_emit(f, r->dst_csid);
	if (st != DLM_REQ_OK)
		return st;

	r->sent_ms = dq_now(f);
	r->frames_tx++;
	if (r->tries < 0xffu)
		r->tries++;
	if (r->to_directory)
		f->lookups_sent++;
	else
		f->requests_sent++;
	return DLM_REQ_OK;
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
 * 3. The completion / commit pair (op 0x04 + op 0x03, PROVISIONAL)
 *
 * TWO FRAMES, TWO FRESH READS. The one field that killed a cluster is
 * master_lkid, so it is read out of the lock database immediately before each
 * frame is built -- never taken off the grant that arrived a microsecond
 * earlier. The codec refuses a zero in either lock-id field (fc8540ae), which
 * is the second gate behind this one.
 * ========================================================================== */
static enum dlm_req_status dq_send_one_completion(struct dlm_req_fsm *f,
						  struct dlm_req *r,
						  uint8_t wireop)
{
	struct vms_dlm_proxy_post p;
	struct vms_dlm_completion c;
	uint32_t written = 0u;

	dq_bzero(&p, (uint32_t)sizeof(p));
	if (f->ops->refill_post(f->ops->ctx, r->req_lkid, r->post_op,
				r->dst_csid, &p) != 0) {
		f->lock_gone++;
		return DLM_REQ_E_NOLOCK;
	}

	dq_bzero(&c, (uint32_t)sizeof(c));
	c.master_lkid = p.master_lkid;   /* the MASTER's handle, from the LKB */
	c.req_lkid    = p.req_lkid;      /* ours, from the same read          */
	c.name_len    = dq_name_from_post(&p, c.name);

	dq_bzero(f->txframe, (uint32_t)sizeof(f->txframe));
	if (vms_dlm_completion_build(&c, wireop, f->txframe,
				     (uint32_t)sizeof(f->txframe),
				     &written) != VMS_CODEC_OK) {
		/* The codec refused -- which for this builder means a lock id
		 * the executive has not (yet) got. Never patched around. */
		f->codec_failures++;
		return DLM_REQ_E_CODEC;
	}
	return dq_emit(f, r->dst_csid);
}

static enum dlm_req_status dq_send_completion(struct dlm_req_fsm *f,
					      struct dlm_req *r)
{
	enum dlm_req_status st;

	st = dq_send_one_completion(f, r,
				    (uint8_t)VMS_DLM_WIREOP_COMPLETE_PROVISIONAL);
	if (st != DLM_REQ_OK)
		return st;
	st = dq_send_one_completion(f, r,
				    (uint8_t)VMS_DLM_WIREOP_COMMIT_PROVISIONAL);
	if (st != DLM_REQ_OK)
		return st;

	r->sent_ms  = dq_now(f);
	r->tries    = 0u;
	r->settled  = 1u;   /* nothing outstanding: the beat leaves it alone */
	r->frames_tx += 2u;
	f->completions_sent++;
	return DLM_REQ_OK;
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

	r->post_op = e->post->op;
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
 * A RELEASE. There is no grounded cat-0x02 opcode for one (header §"WHAT IS
 * GROUNDED"), so nothing goes on the wire -- but the WIRE RECORD goes away,
 * because the requester really is releasing the lock and a stale record of a
 * lock we no longer hold is its own kind of fabrication.
 */
static void h_post_deq(struct dlm_req_fsm *f, struct dq_ev *e)
{
	f->releases_no_wire_op++;
	dq_log(f, "%CNXMAN, cross-node lock release not sent: no grounded "
		  "cat-02 opcode for a DLM dequeue");
	if (e->r != (struct dlm_req *)0)
		dq_free(e->r);
	e->st = DLM_REQ_E_NOWIREOP;
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
	 * valblk_present stays 0: no grounded cat-0x02 LVB field exists, so
	 * the engine leaves the proxy's own value block alone instead of
	 * overwriting it with sixteen zeros that would read exactly like data.
	 */

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
	r->tries        = 0u;
	r->settled      = 0u;
	r->state        = (uint8_t)DLM_REQ_ST_GRANTED;
	e->st = dq_send_completion(f, r);
}

/* A grant for a lock we have already completed: the master did not see our
 * completion. Re-apply it (the engine is idempotent on the key) and answer
 * again -- one reply per received frame, which is what makes it not a storm. */
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
	e->r->settled = 0u;
	e->st = dq_send_completion(f, e->r);
	if (e->st == DLM_REQ_OK)
		f->completions_resent++;
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
		/* A refused CONVERT leaves the lock at its old mode: the lock
		 * is still real, so the wire record stays with it. */
		e->r->state = (uint8_t)DLM_REQ_ST_GRANTED;
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
	if (e->st == DLM_REQ_OK)
		f->retransmits++;
	else if (e->st == DLM_REQ_E_NOLOCK)
		dq_free(r);   /* the lock went away under us: abandon quietly */
}

/*
 * The completion did not go out. Retry it -- but a GRANTED lock is REAL, so
 * when the ladder is spent the request is NOT failed: the wire record is
 * dropped and the fact is counted. A timeout may not take away a lock the
 * master granted (the engine holds the same rule).
 */
static void h_timeout_completion(struct dlm_req_fsm *f, struct dq_ev *e)
{
	struct dlm_req *r = e->r;

	if (r->tries >= (uint8_t)DLM_REQ_MAX_TRIES) {
		f->timeouts_failed++;
		dq_free(r);
		e->st = DLM_REQ_OK;
		return;
	}
	if (r->tries < 0xffu)
		r->tries++;
	e->st = dq_send_completion(f, r);
	if (e->st == DLM_REQ_OK)
		f->completions_resent++;
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
		[DLM_REQ_EV_TIMEOUT]   = h_timeout_completion,
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
			/* Nothing to release a record for; still the honest
			 * "no grounded opcode" answer. */
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
		before = f->retransmits + f->completions_resent;
		dq_bzero(&e, (uint32_t)sizeof(e));
		(void)dq_dispatch(f, r, DLM_REQ_EV_TIMEOUT, &e);
		if (f->retransmits + f->completions_resent != before)
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
