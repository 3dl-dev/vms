// SPDX-License-Identifier: GPL-2.0
/*
 * vms_scs_dir.c - the SCS Directory Service (`SCS$DIRECTORY`) and the SCS
 * Process Poller (`SCS$DIR_LOOKUP`), FC-P2.3.
 *
 * Read vms_scs_dir.h first: it carries the model, the grounding, and the two
 * rules that keep this file honest -- a HIT is a read of the ONE SYSAP
 * registry, and a timed-out inquiry is never reported as a "No".
 *
 * PURITY. No substrate include, no seam call, no allocation, no libc; the
 * clock is ops->now_ms; every wire byte goes through the FC-P2.1 codec's
 * body-level directory entries, so there is not one raw offset in this file
 * (design SS3.9 rules 2 and 3).
 *
 * PAGE CITES are to *VAXcluster Principles* (Davis 1993) SS2.11, pp. 2-48..2-51.
 * The transcript is copyrighted and host-only: cited, never quoted at length.
 */

#include "vms_scs_dir.h"

/* ==========================================================================
 * A. Small helpers
 * ========================================================================== */

static void dir_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		b[i] = 0u;
}

static void dir_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static int dir_name_eq(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < VMS_SCS_PROCNAME_LEN; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

static void dir_log(const struct scs_dir *d, const char *msg)
{
	if (d->ops != (const struct scs_dir_ops *)0 &&
	    d->ops->log != (void (*)(void *, const char *))0)
		d->ops->log(d->ops->ctx, msg);
}

static uint32_t dir_now(const struct scs_dir *d)
{
	if (d->ops != (const struct scs_dir_ops *)0 &&
	    d->ops->now_ms != (uint32_t (*)(void *))0)
		return d->ops->now_ms(d->ops->ctx);
	return 0u;
}

/* When an inquiry stops being worth waiting for. Wrap-safe: the comparison at
 * the far end is a SIGNED difference, so a clock that rolls over does not turn
 * a fresh inquiry into an expired one. */
static uint32_t dir_deadline(const struct scs_dir *d)
{
	return dir_now(d) + d->cfg.lookup_timeout_ms;
}

/* ==========================================================================
 * B. The two published SYSAP names (p. 2-51), as 16-byte BLANK-padded fields
 * ========================================================================== */

const uint8_t scs_dir_name_directory[VMS_SCS_PROCNAME_LEN] = {
	'S', 'C', 'S', '$', 'D', 'I', 'R', 'E',
	'C', 'T', 'O', 'R', 'Y', ' ', ' ', ' '
};

const uint8_t scs_dir_name_lookup[VMS_SCS_PROCNAME_LEN] = {
	'S', 'C', 'S', '$', 'D', 'I', 'R', '_',
	'L', 'O', 'O', 'K', 'U', 'P', ' ', ' '
};

int scs_dir_name_pad(uint8_t *out, const char *ascii)
{
	uint32_t i = 0u;

	if (out == (uint8_t *)0 || ascii == (const char *)0)
		return SCS_ERR_INVAL;
	while (i < VMS_SCS_PROCNAME_LEN && ascii[i] != '\0') {
		out[i] = (uint8_t)ascii[i];
		i++;
	}
	/* A name that does not fit is REFUSED, not silently shortened: a
	 * truncated SYSAP name is a different name to a real VAX. */
	if (i == VMS_SCS_PROCNAME_LEN && ascii[i] != '\0')
		return SCS_ERR_INVAL;
	for (; i < VMS_SCS_PROCNAME_LEN; i++)
		out[i] = (uint8_t)' ';
	return SCS_OK;
}

/* ==========================================================================
 * C. The peer table -- one row per remote system's transient connection
 * ========================================================================== */

struct scs_dir_peer *scs_dir_peer_by_sysid(struct scs_dir *d,
					   vms_scs_sysid_t sysid)
{
	uint32_t i;

	if (d == (struct scs_dir *)0 || d->peers == (struct scs_dir_peer *)0)
		return (struct scs_dir_peer *)0;
	for (i = 0; i < d->n_peers; i++) {
		if (d->peers[i].in_use && d->peers[i].sysid == sysid)
			return &d->peers[i];
	}
	return (struct scs_dir_peer *)0;
}

static struct scs_dir_peer *peer_by_conid(struct scs_dir *d, vms_conid_t conid)
{
	uint32_t i;

	if (d->peers == (struct scs_dir_peer *)0 || conid == 0u)
		return (struct scs_dir_peer *)0;
	for (i = 0; i < d->n_peers; i++) {
		if (d->peers[i].in_use && d->peers[i].conid == conid)
			return &d->peers[i];
	}
	return (struct scs_dir_peer *)0;
}

static struct scs_dir_peer *peer_alloc(struct scs_dir *d, vms_scs_sysid_t sysid)
{
	uint32_t i;

	for (i = 0; d->peers != (struct scs_dir_peer *)0 && i < d->n_peers; i++) {
		if (d->peers[i].in_use)
			continue;
		dir_bzero(&d->peers[i], (uint32_t)sizeof(d->peers[i]));
		d->peers[i].in_use = 1u;
		d->peers[i].state = (uint8_t)SCS_DIR_IDLE;
		d->peers[i].sysid = sysid;
		return &d->peers[i];
	}
	d->no_peer_slot++;
	return (struct scs_dir_peer *)0;
}

static uint32_t peer_index(const struct scs_dir *d,
			   const struct scs_dir_peer *p)
{
	return (uint32_t)(p - d->peers);
}

static void peer_set_state(struct scs_dir_peer *p, enum scs_dir_state s)
{
	p->state = (uint8_t)s;
}

/* ==========================================================================
 * D. The inquiry table -- what this node has asked, and who to tell
 * ========================================================================== */

static struct scs_dir_inquiry *inq_alloc(struct scs_dir *d,
					 const struct scs_dir_peer *p,
					 const uint8_t *name,
					 scs_dir_result_cb cb, void *cb_ctx)
{
	uint32_t i;

	for (i = 0; d->inq != (struct scs_dir_inquiry *)0 && i < d->n_inq; i++) {
		if (d->inq[i].in_use)
			continue;
		dir_bzero(&d->inq[i], (uint32_t)sizeof(d->inq[i]));
		d->inq[i].in_use = 1u;
		d->inq[i].peer_index = peer_index(d, p);
		dir_copy(d->inq[i].name, name, VMS_SCS_PROCNAME_LEN);
		d->inq[i].cb = cb;
		d->inq[i].cb_ctx = cb_ctx;
		/* The clock starts when the SYSAP ASKED, not when the frame
		 * went out. Time spent waiting for a connection to form, or
		 * behind another inquiry on this one-at-a-time connection, is
		 * time the asker has been waiting -- so an inquiry that never
		 * reaches the wire expires on the same terms as one that did,
		 * and nothing can sit in the table forever. */
		d->inq[i].deadline_ms = dir_deadline(d);
		return &d->inq[i];
	}
	d->no_inquiry_slot++;
	return (struct scs_dir_inquiry *)0;
}

/* The first inquiry for `p` that has not gone out yet, or NULL. */
static struct scs_dir_inquiry *inq_next_unsent(struct scs_dir *d,
					       const struct scs_dir_peer *p)
{
	uint32_t i, want = peer_index(d, p);

	for (i = 0; d->inq != (struct scs_dir_inquiry *)0 && i < d->n_inq; i++) {
		if (d->inq[i].in_use && !d->inq[i].sent &&
		    d->inq[i].peer_index == want)
			return &d->inq[i];
	}
	return (struct scs_dir_inquiry *)0;
}

/* The outstanding inquiry `name` answers, or NULL -- matched on the peer AND
 * the queried name the peer echoed back, never on arrival order. */
static struct scs_dir_inquiry *inq_match(struct scs_dir *d,
					 const struct scs_dir_peer *p,
					 const uint8_t *name)
{
	uint32_t i, want = peer_index(d, p);

	for (i = 0; d->inq != (struct scs_dir_inquiry *)0 && i < d->n_inq; i++) {
		if (d->inq[i].in_use && d->inq[i].sent &&
		    d->inq[i].peer_index == want &&
		    dir_name_eq(d->inq[i].name, name))
			return &d->inq[i];
	}
	return (struct scs_dir_inquiry *)0;
}

static void inq_release(struct scs_dir *d, struct scs_dir_inquiry *q)
{
	struct scs_dir_peer *p = &d->peers[q->peer_index];

	if (q->sent) {
		if (p->outstanding > 0u)
			p->outstanding--;
	} else if (p->queued > 0u) {
		p->queued--;
	}
	dir_bzero(q, (uint32_t)sizeof(*q));
}

/* ==========================================================================
 * E. Composing and sending one directory message
 * ========================================================================== */

static int dir_send_body(struct scs_dir *d, vms_conid_t conid,
			 const struct vms_scs_dir_msg *m)
{
	if (vms_scs_dir_msg_build(m, d->bodybuf,
				  (uint32_t)sizeof(d->bodybuf)) != VMS_CODEC_OK)
		return SCS_ERR_CODEC;
	if (d->ops->send == (int (*)(void *, vms_conid_t, const uint8_t *,
				     uint32_t))0)
		return SCS_ERR_TXFAIL;
	return d->ops->send(d->ops->ctx, conid, d->bodybuf, SCS_DIR_BODY_LEN);
}

static void dir_return_credit(struct scs_dir *d, vms_conid_t conid)
{
	if (d->ops->return_credit != (int (*)(void *, vms_conid_t, uint16_t))0)
		(void)d->ops->return_credit(d->ops->ctx, conid, 1u);
}

/* ==========================================================================
 * F. THE SERVER HALF -- `SCS$DIRECTORY` (p. 2-50)
 * ========================================================================== */

/*
 * The one place a directory answer is decided, and the only thing it consults
 * is the live SYSAP registry.
 *
 * A miss is the GROUNDED literal `"NOT PRESENT HERE"` (spec SS4(h)(2)). A hit
 * carries the 16 bytes the owning SYSAP declared, or -- when it declared none
 * -- the REGISTERED NAME itself, which is the affirmative shape a real member
 * returned for `MSCP$DISK` and is still a read of the registry entry that was
 * found. No captured descriptor is ever baked in (vms_scs_dir.h, "what a HIT
 * carries").
 */
static void dir_answer_for(struct scs_dir *d, const uint8_t *name,
			   struct vms_scs_dir_lookup *out)
{
	struct scs_sysap_info info;
	int rc = SCS_ERR_NOSYSAP;

	dir_copy(out->queried_name, name, VMS_SCSCTRL_NAME_LEN);
	dir_bzero(&info, (uint32_t)sizeof(info));

	if (d->ops->sysap_lookup != (int (*)(void *, const uint8_t *,
					     struct scs_sysap_info *))0)
		rc = d->ops->sysap_lookup(d->ops->ctx, name, &info);

	if (rc != SCS_OK) {
		out->result_kind = (uint8_t)VMS_SCS_DIR_RESULT_NOT_PRESENT;
		dir_copy(out->result, vms_scs_dir_not_present_here,
			 VMS_SCSCTRL_NAME_LEN);
		d->srv_misses++;
		return;
	}

	out->result_kind = (uint8_t)VMS_SCS_DIR_RESULT_AFFIRMATIVE;
	if (info.dir_data_valid)
		dir_copy(out->result, info.dir_data, VMS_SCSCTRL_NAME_LEN);
	else
		dir_copy(out->result, info.name, VMS_SCSCTRL_NAME_LEN);
	d->srv_hits++;
}

/*
 * p. 2-50: the Directory Service accepts the connection. It has no admission
 * policy of its own -- refusing here would deny a peer the one service that
 * tells it what this node offers.
 */
static int dir_srv_connect_req(void *ctx, vms_conid_t local_conid,
			       vms_scs_sysid_t peer, vms_conid_t peer_conid,
			       const uint8_t *conndata, uint32_t conndata_len)
{
	struct scs_dir *d = (struct scs_dir *)ctx;

	(void)local_conid;
	(void)peer;
	(void)peer_conid;
	(void)conndata;
	(void)conndata_len;
	d->srv_connects++;
	return 0;
}

static void dir_srv_opened(void *ctx, vms_conid_t local_conid)
{
	(void)ctx;
	(void)local_conid;
}

/* One inquiry arrived. Release the buffer FIRST so the answer piggybacks it
 * (p. 2-43's own mechanism), then answer. */
static int dir_srv_message(void *ctx, vms_conid_t local_conid,
			   const uint8_t *body, uint32_t len)
{
	struct scs_dir *d = (struct scs_dir *)ctx;
	struct vms_scs_dir_msg in, out;

	if (vms_scs_dir_msg_parse(body, len, &in) != VMS_CODEC_OK) {
		d->rx_malformed++;
		dir_return_credit(d, local_conid);
		return -1;
	}
	if (in.marker != VMS_SCS_DIR_MARKER_REQUEST) {
		/* An ANSWER delivered to the answering half. Counted, never
		 * acted on -- the server has no inquiry it could match. */
		d->rx_wrong_role++;
		dir_return_credit(d, local_conid);
		return -1;
	}

	dir_bzero(&out, (uint32_t)sizeof(out));
	out.marker = VMS_SCS_DIR_MARKER_RESPONSE;
	dir_answer_for(d, in.lookup.queried_name, &out.lookup);

	dir_return_credit(d, local_conid);
	if (dir_send_body(d, local_conid, &out) != SCS_OK) {
		d->srv_send_failed++;
		dir_log(d, "%SCS-W-DIRNOANS, directory answer could not be sent");
	}
	return 0;
}

static void dir_srv_closed(void *ctx, vms_conid_t local_conid, uint32_t reason)
{
	(void)ctx;
	(void)local_conid;
	(void)reason;
	/* p. 2-51: the round ends with a disconnect. The server holds no
	 * per-connection state to unwind -- every answer it gave was a read of
	 * the registry at the moment it gave it. */
}

/* ==========================================================================
 * G. THE CLIENT HALF -- `SCS$DIR_LOOKUP`, the Process Poller (p. 2-51)
 * ========================================================================== */

/* The event record a client handler is given. */
struct dir_ev {
	vms_scs_sysid_t                 dst;
	const uint8_t                  *name;
	scs_dir_result_cb               cb;
	void                           *cb_ctx;
	const struct vms_scs_dir_msg   *msg;
	uint32_t                        now_ms;
};

typedef int (*dir_handler_t)(struct scs_dir *d, struct scs_dir_peer *p,
			     struct dir_ev *ev);

/*
 * ONE INQUIRY AT A TIME on a directory connection. GROUNDED: spec SS4(h)(2a)'s
 * census of the reference capture's two directory connections lists the
 * REQUEST and RESPONSE frames strictly alternating (37, 39, 41, 42, 43, 44,
 * 45, 46) -- never two requests before an answer. The credit ledger says the
 * same thing from the other side: at the extended grant this connection
 * carries, an answer released by the previous exchange is what pays for the
 * next inquiry, which is why the reference wire needs no special credit
 * message on these connections at all.
 */
static int dir_send_next(struct scs_dir *d, struct scs_dir_peer *p)
{
	struct scs_dir_inquiry *q;
	struct vms_scs_dir_msg m;
	int rc;

	if (p->outstanding > 0u)
		return SCS_OK;
	q = inq_next_unsent(d, p);
	if (q == (struct scs_dir_inquiry *)0)
		return SCS_OK;

	dir_bzero(&m, (uint32_t)sizeof(m));
	m.marker = VMS_SCS_DIR_MARKER_REQUEST;
	dir_copy(m.lookup.queried_name, q->name, VMS_SCSCTRL_NAME_LEN);
	/* A REQUEST's result field is all-zero on the reference wire
	 * (SS4(h)(2), "all-zero in the request") -- the asking node has no
	 * result to state. */
	m.lookup.result_kind = (uint8_t)VMS_SCS_DIR_RESULT_EMPTY;

	rc = dir_send_body(d, p->conid, &m);
	if (rc != SCS_OK)
		return rc;

	q->sent = 1u;
	if (p->queued > 0u)
		p->queued--;
	p->outstanding++;
	return SCS_OK;
}

/* The round is over when nothing is queued and nothing is outstanding: p. 2-51
 * "After the Process Poller has received replies to all of its inquiries, the
 * Process Poller and Directory Service disconnect from each other." */
static void dir_close_if_done(struct scs_dir *d, struct scs_dir_peer *p)
{
	if (p->outstanding > 0u || p->queued > 0u)
		return;
	if (d->ops->disconnect == (int (*)(void *, vms_conid_t))0)
		return;

	peer_set_state(p, SCS_DIR_CLOSING);
	if (d->ops->disconnect(d->ops->ctx, p->conid) == SCS_OK)
		return;

	/* SCS would not take the teardown, so the connection is still up and
	 * this row must say so: parking it in CLOSING would make every later
	 * inquiry to this system time out on a connection that was working. */
	peer_set_state(p, SCS_DIR_OPEN);
	dir_log(d, "%SCS-W-DIRNOTERM, directory connection would not close");
}

static int dir_open_connection(struct scs_dir *d, struct scs_dir_peer *p)
{
	vms_conid_t conid = 0u;
	int rc;

	if (d->ops->connect == (int (*)(void *, vms_scs_sysid_t,
					const struct scs_sysap_ops *, uint16_t,
					vms_conid_t *))0)
		return SCS_ERR_TXFAIL;
	rc = d->ops->connect(d->ops->ctx, p->sysid, &d->client_ops,
			     d->cfg.credits, &conid);
	if (rc != SCS_OK) {
		d->cli_connect_failed++;
		return rc;
	}
	p->conid = conid;
	peer_set_state(p, SCS_DIR_CONNECTING);
	return SCS_OK;
}

/* ---- handlers ----------------------------------------------------------- */

/* Record the inquiry; every state does this and then differs only in what it
 * does next, so the queueing itself is one function. */
static struct scs_dir_inquiry *dir_queue(struct scs_dir *d,
					 struct scs_dir_peer *p,
					 struct dir_ev *ev)
{
	struct scs_dir_inquiry *q = inq_alloc(d, p, ev->name, ev->cb,
					      ev->cb_ctx);

	if (q != (struct scs_dir_inquiry *)0)
		p->queued++;
	return q;
}

/* [IDLE] LOOKUP -- open the transient connection this round needs. */
static int h_idle_lookup(struct scs_dir *d, struct scs_dir_peer *p,
			 struct dir_ev *ev)
{
	struct scs_dir_inquiry *q = dir_queue(d, p, ev);
	int rc;

	if (q == (struct scs_dir_inquiry *)0)
		return SCS_ERR_NOCDT;
	rc = dir_open_connection(d, p);
	if (rc != SCS_OK) {
		/* Nothing went on the wire, so nothing is pending: drop THIS
		 * inquiry rather than leave one waiting on a connection that
		 * was never opened. The poll repeats -- that is the retry. */
		inq_release(d, q);
		return rc;
	}
	return SCS_OK;
}

/* [CONNECTING]/[CLOSING] LOOKUP -- queue it; the round in flight picks it up
 * when it opens, or the next one does. */
static int h_queue_only(struct scs_dir *d, struct scs_dir_peer *p,
			struct dir_ev *ev)
{
	return dir_queue(d, p, ev) != (struct scs_dir_inquiry *)0
		       ? SCS_OK : SCS_ERR_NOCDT;
}

/* [OPEN] LOOKUP. */
static int h_open_lookup(struct scs_dir *d, struct scs_dir_peer *p,
			 struct dir_ev *ev)
{
	if (dir_queue(d, p, ev) == (struct scs_dir_inquiry *)0)
		return SCS_ERR_NOCDT;
	return dir_send_next(d, p);
}

/* [CONNECTING] OPENED -- the round may start. */
static int h_opened(struct scs_dir *d, struct scs_dir_peer *p,
		    struct dir_ev *ev)
{
	(void)ev;
	peer_set_state(p, SCS_DIR_OPEN);
	if (p->queued == 0u) {
		/* Every inquiry that asked for this connection went away while
		 * it was forming. Do not hold it open. */
		dir_close_if_done(d, p);
		return SCS_OK;
	}
	return dir_send_next(d, p);
}

/*
 * [OPEN]/[CLOSING] RESPONSE. `present` is read off the wire and nowhere else:
 * SS4(h)(2)'s literal negative means No and anything else means Yes. The
 * queried name the peer echoed is what matches the inquiry -- never the
 * arrival order, and never the result (an affirmative result is NOT the name
 * in general).
 */
static int h_response(struct scs_dir *d, struct scs_dir_peer *p,
		      struct dir_ev *ev)
{
	struct scs_dir_inquiry *q;
	scs_dir_result_cb cb;
	void *cb_ctx;
	uint8_t name[VMS_SCS_PROCNAME_LEN];
	int present;

	q = inq_match(d, p, ev->msg->lookup.queried_name);
	if (q == (struct scs_dir_inquiry *)0) {
		d->rx_unmatched++;
		return SCS_OK;
	}

	present = ev->msg->lookup.result_kind !=
		  (uint8_t)VMS_SCS_DIR_RESULT_NOT_PRESENT;
	if (present)
		d->cli_hits++;
	else
		d->cli_misses++;

	cb = q->cb;
	cb_ctx = q->cb_ctx;
	dir_copy(name, q->name, VMS_SCS_PROCNAME_LEN);
	inq_release(d, q);

	if (cb != (scs_dir_result_cb)0)
		cb(cb_ctx, p->sysid, name, present);

	if (p->state == (uint8_t)SCS_DIR_OPEN) {
		int rc = dir_send_next(d, p);

		if (rc != SCS_OK)
			return rc;
		dir_close_if_done(d, p);
	}
	return SCS_OK;
}

/* Any state, CLOSED. SCS has torn the connection down (round finished, peer
 * disconnected, or the circuit was lost). Inquiries that were on the wire are
 * ABANDONED -- counted, no callback: silence is not an answer. */
static int h_closed(struct scs_dir *d, struct scs_dir_peer *p,
		    struct dir_ev *ev)
{
	uint32_t i, self = peer_index(d, p);

	(void)ev;
	for (i = 0; d->inq != (struct scs_dir_inquiry *)0 && i < d->n_inq; i++) {
		if (!d->inq[i].in_use || d->inq[i].peer_index != self ||
		    !d->inq[i].sent)
			continue;
		d->cli_abandoned++;
		inq_release(d, &d->inq[i]);
	}

	p->conid = 0u;
	p->outstanding = 0u;
	p->rounds++;
	d->cli_rounds++;
	peer_set_state(p, SCS_DIR_IDLE);

	/* Anything asked for while the round was ending starts the next one --
	 * which is what "periodically" looks like from inside (p. 2-51). */
	if (p->queued == 0u)
		return SCS_OK;
	if (dir_open_connection(d, p) == SCS_OK)
		return SCS_OK;

	/* The next round could not be opened. Release what was waiting on it
	 * rather than leave inquiries parked on a peer row with no connection
	 * and no timer under it; the asker polls again. */
	for (i = 0; d->inq != (struct scs_dir_inquiry *)0 && i < d->n_inq; i++) {
		if (d->inq[i].in_use && d->inq[i].peer_index == self) {
			d->cli_abandoned++;
			inq_release(d, &d->inq[i]);
		}
	}
	return SCS_ERR_TXFAIL;
}

/*
 * [CONNECTING]/[OPEN]/[CLOSING] TICK. Two jobs: expire every inquiry for this
 * peer whose deadline has passed -- SENT OR NOT, because the asker has been
 * waiting either way -- and then make progress, which also RETRIES an inquiry
 * whose send the port refused earlier. A connection with nothing left on it
 * closes here as it would after a normal answer.
 */
static int h_tick(struct scs_dir *d, struct scs_dir_peer *p, struct dir_ev *ev)
{
	uint32_t i, self = peer_index(d, p);
	int expired = 0;

	for (i = 0; d->inq != (struct scs_dir_inquiry *)0 && i < d->n_inq; i++) {
		if (!d->inq[i].in_use || d->inq[i].peer_index != self)
			continue;
		if ((int32_t)(ev->now_ms - d->inq[i].deadline_ms) < 0)
			continue;
		/* NO CALLBACK. vms_scs_dir.h: a timed-out inquiry is not a
		 * "No" -- reporting it as one would fabricate a peer's answer. */
		d->cli_timeouts++;
		inq_release(d, &d->inq[i]);
		expired = 1;
	}
	if (expired)
		dir_log(d, "%SCS-W-DIRNOANSWER, directory inquiry unanswered");

	if (p->state == (uint8_t)SCS_DIR_OPEN) {
		(void)dir_send_next(d, p);
		dir_close_if_done(d, p);
	}
	return SCS_OK;
}

/* ==========================================================================
 * H. THE TABLE -- [peer state][event]
 *
 * A NULL cell is counted in d->ignored_events, never silently dropped. The
 * empty cells are facts, not omissions:
 *   [IDLE][RESPONSE]/[OPENED]/[CLOSED] -- there is no connection under an IDLE
 *                        peer; the CDL already refuses any frame for a
 *                        released Con.ID, so reaching here would be an SCS bug.
 *   [IDLE][TICK]      -- an IDLE peer holds no inquiry (h_idle_lookup and
 *                        h_closed both guarantee it), so nothing can expire.
 *   [CONNECTING][RESPONSE] -- SCS delivers no application message before the
 *                        connection reaches OPEN.
 *   [OPEN][OPENED], [CLOSING][OPENED] -- a connection opens once.
 * ========================================================================== */
static const dir_handler_t
dir_table[SCS_DIR_STATE__COUNT][SCS_DIR_EV__COUNT] = {
	[SCS_DIR_IDLE] = {
		[SCS_DIR_EV_LOOKUP]   = h_idle_lookup,
	},
	[SCS_DIR_CONNECTING] = {
		[SCS_DIR_EV_LOOKUP]   = h_queue_only,
		[SCS_DIR_EV_OPENED]   = h_opened,
		[SCS_DIR_EV_CLOSED]   = h_closed,
		[SCS_DIR_EV_TICK]     = h_tick,
	},
	[SCS_DIR_OPEN] = {
		[SCS_DIR_EV_LOOKUP]   = h_open_lookup,
		[SCS_DIR_EV_RESPONSE] = h_response,
		[SCS_DIR_EV_CLOSED]   = h_closed,
		[SCS_DIR_EV_TICK]     = h_tick,
	},
	[SCS_DIR_CLOSING] = {
		[SCS_DIR_EV_LOOKUP]   = h_queue_only,
		[SCS_DIR_EV_RESPONSE] = h_response,
		[SCS_DIR_EV_CLOSED]   = h_closed,
		[SCS_DIR_EV_TICK]     = h_tick,
	},
};

static int dir_dispatch(struct scs_dir *d, struct scs_dir_peer *p,
			enum scs_dir_event ev, struct dir_ev *rec)
{
	dir_handler_t h;

	if (d == (struct scs_dir *)0 || p == (struct scs_dir_peer *)0)
		return SCS_ERR_INVAL;
	if ((unsigned)ev >= (unsigned)SCS_DIR_EV__COUNT ||
	    (unsigned)p->state >= (unsigned)SCS_DIR_STATE__COUNT)
		return SCS_ERR_INVAL;

	h = dir_table[p->state][ev];
	if (h == (dir_handler_t)0) {
		d->ignored_events++;
		return SCS_ERR_INVAL;
	}
	return h(d, p, rec);
}

/* ==========================================================================
 * I. The client SYSAP callbacks -- SCS's side of the same connection
 * ========================================================================== */

static void dir_cli_opened(void *ctx, vms_conid_t local_conid)
{
	struct scs_dir *d = (struct scs_dir *)ctx;
	struct scs_dir_peer *p = peer_by_conid(d, local_conid);
	struct dir_ev ev;

	if (p == (struct scs_dir_peer *)0)
		return;
	dir_bzero(&ev, (uint32_t)sizeof(ev));
	ev.now_ms = dir_now(d);
	(void)dir_dispatch(d, p, SCS_DIR_EV_OPENED, &ev);
}

static int dir_cli_message(void *ctx, vms_conid_t local_conid,
			   const uint8_t *body, uint32_t len)
{
	struct scs_dir *d = (struct scs_dir *)ctx;
	struct scs_dir_peer *p = peer_by_conid(d, local_conid);
	struct vms_scs_dir_msg m;
	struct dir_ev ev;

	if (p == (struct scs_dir_peer *)0) {
		d->rx_unmatched++;
		dir_return_credit(d, local_conid);
		return -1;
	}
	if (vms_scs_dir_msg_parse(body, len, &m) != VMS_CODEC_OK) {
		d->rx_malformed++;
		dir_return_credit(d, local_conid);
		return -1;
	}
	if (m.marker != VMS_SCS_DIR_MARKER_RESPONSE) {
		/* A REQUEST arriving on the asking half. The answering half is
		 * SCS$DIRECTORY and has its own connection; this one is not it. */
		d->rx_wrong_role++;
		dir_return_credit(d, local_conid);
		return -1;
	}

	/* Release the buffer before anything else goes out, so the next
	 * inquiry piggybacks it (p. 2-43). */
	dir_return_credit(d, local_conid);

	dir_bzero(&ev, (uint32_t)sizeof(ev));
	ev.msg = &m;
	ev.now_ms = dir_now(d);
	(void)dir_dispatch(d, p, SCS_DIR_EV_RESPONSE, &ev);
	return 0;
}

static void dir_cli_closed(void *ctx, vms_conid_t local_conid, uint32_t reason)
{
	struct scs_dir *d = (struct scs_dir *)ctx;
	struct scs_dir_peer *p = peer_by_conid(d, local_conid);
	struct dir_ev ev;

	(void)reason;
	if (p == (struct scs_dir_peer *)0)
		return;
	dir_bzero(&ev, (uint32_t)sizeof(ev));
	ev.now_ms = dir_now(d);
	(void)dir_dispatch(d, p, SCS_DIR_EV_CLOSED, &ev);
}

static void dir_cli_send_failed(void *ctx, vms_conid_t local_conid,
				uint32_t reason)
{
	struct scs_dir *d = (struct scs_dir *)ctx;

	(void)local_conid;
	(void)reason;
	/* An inquiry that sat in Credit Wait until the path died. The CLOSED
	 * that follows abandons it; this is the log line that says why. */
	dir_log(d, "%SCS-W-DIRSENDFAIL, directory inquiry lost with the path");
}

/* ==========================================================================
 * J. Lifecycle, binding, and the public services
 * ========================================================================== */

static void dir_cfg_defaults(struct scs_dir_cfg *c)
{
	c->lookup_timeout_ms = SCS_DIR_LOOKUP_TIMEOUT_MS_DEFAULT;
	c->credits = SCS_DIR_CREDITS_DEFAULT;
	c->pad0 = 0u;
}

static void dir_build_sysap_tables(struct scs_dir *d)
{
	d->server_ops.connect_req = dir_srv_connect_req;
	d->server_ops.opened = dir_srv_opened;
	d->server_ops.message = dir_srv_message;
	d->server_ops.closed = dir_srv_closed;
	d->server_ops.send_failed = (void (*)(void *, vms_conid_t, uint32_t))0;
	d->server_ops.ctx = d;

	/* The poller never LISTENs -- nothing connects to SCS$DIR_LOOKUP -- so
	 * it has no connect-request routine, and that absence is the truth
	 * rather than a stub that would accept one. */
	d->client_ops.connect_req = (int (*)(void *, vms_conid_t,
					     vms_scs_sysid_t, vms_conid_t,
					     const uint8_t *, uint32_t))0;
	d->client_ops.opened = dir_cli_opened;
	d->client_ops.message = dir_cli_message;
	d->client_ops.closed = dir_cli_closed;
	d->client_ops.send_failed = dir_cli_send_failed;
	d->client_ops.ctx = d;
}

int scs_dir_init(struct scs_dir *d, const struct scs_dir_ops *ops)
{
	if (d == (struct scs_dir *)0 || ops == (const struct scs_dir_ops *)0)
		return SCS_ERR_INVAL;
	dir_bzero(d, (uint32_t)sizeof(*d));
	d->ops = ops;
	dir_cfg_defaults(&d->cfg);
	dir_build_sysap_tables(d);
	return SCS_OK;
}

int scs_dir_bind_peers(struct scs_dir *d, struct scs_dir_peer *p, uint32_t n)
{
	uint32_t i;

	if (d == (struct scs_dir *)0 || p == (struct scs_dir_peer *)0 || n == 0u)
		return SCS_ERR_INVAL;
	for (i = 0; i < n; i++)
		dir_bzero(&p[i], (uint32_t)sizeof(p[i]));
	d->peers = p;
	d->n_peers = n;
	return SCS_OK;
}

int scs_dir_bind_inquiries(struct scs_dir *d, struct scs_dir_inquiry *q,
			   uint32_t n)
{
	uint32_t i;

	if (d == (struct scs_dir *)0 || q == (struct scs_dir_inquiry *)0 ||
	    n == 0u)
		return SCS_ERR_INVAL;
	for (i = 0; i < n; i++)
		dir_bzero(&q[i], (uint32_t)sizeof(q[i]));
	d->inq = q;
	d->n_inq = n;
	return SCS_OK;
}

void scs_dir_set_cfg(struct scs_dir *d, const struct scs_dir_cfg *cfg)
{
	if (d == (struct scs_dir *)0)
		return;
	if (cfg == (const struct scs_dir_cfg *)0)
		dir_cfg_defaults(&d->cfg);
	else
		d->cfg = *cfg;
}

const struct scs_sysap_ops *scs_dir_server_ops(struct scs_dir *d)
{
	return d == (struct scs_dir *)0 ? (const struct scs_sysap_ops *)0
					: &d->server_ops;
}

const struct scs_sysap_ops *scs_dir_client_ops(struct scs_dir *d)
{
	return d == (struct scs_dir *)0 ? (const struct scs_sysap_ops *)0
					: &d->client_ops;
}

int scs_dir_inquire(struct scs_dir *d, vms_scs_sysid_t dst, const uint8_t *name,
		   scs_dir_result_cb cb, void *cb_ctx)
{
	struct scs_dir_peer *p;
	struct dir_ev ev;

	if (d == (struct scs_dir *)0 || name == (const uint8_t *)0)
		return SCS_ERR_INVAL;
	p = scs_dir_peer_by_sysid(d, dst);
	if (p == (struct scs_dir_peer *)0)
		p = peer_alloc(d, dst);
	if (p == (struct scs_dir_peer *)0)
		return SCS_ERR_NOSB;

	dir_bzero(&ev, (uint32_t)sizeof(ev));
	ev.dst = dst;
	ev.name = name;
	ev.cb = cb;
	ev.cb_ctx = cb_ctx;
	ev.now_ms = dir_now(d);
	return dir_dispatch(d, p, SCS_DIR_EV_LOOKUP, &ev);
}

void scs_dir_tick(struct scs_dir *d)
{
	struct dir_ev ev;
	uint32_t i;

	if (d == (struct scs_dir *)0 || d->peers == (struct scs_dir_peer *)0)
		return;
	dir_bzero(&ev, (uint32_t)sizeof(ev));
	ev.now_ms = dir_now(d);
	for (i = 0; i < d->n_peers; i++) {
		if (!d->peers[i].in_use)
			continue;
		/* An IDLE peer holds no inquiry -- h_idle_lookup and h_closed
		 * both guarantee it -- so there is nothing here to expire. */
		if (d->peers[i].state == (uint8_t)SCS_DIR_IDLE)
			continue;
		(void)dir_dispatch(d, &d->peers[i], SCS_DIR_EV_TICK, &ev);
	}
}

static const char *const dir_state_names[SCS_DIR_STATE__COUNT] = {
	"idle", "connecting", "open", "closing"
};

const char *scs_dir_state_name(enum scs_dir_state s)
{
	if ((unsigned)s >= (unsigned)SCS_DIR_STATE__COUNT)
		return "?";
	return dir_state_names[s];
}
