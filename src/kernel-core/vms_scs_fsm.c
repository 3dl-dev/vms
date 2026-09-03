// SPDX-License-Identifier: GPL-2.0
/*
 * vms_scs_fsm.c - the pure SCS state machine (FC-P2.2).
 *
 * Read vms_scs_fsm.h first: it carries the model, the grounding and the three
 * structural rules (only-way-to-send spends a credit, only-way-to-receive is
 * the CDL, every credit byte is a ledger read).
 *
 * PURITY. No substrate include, no seam call, no allocation, no libc; the
 * clock is ops->now_ms and every frame is built and parsed through the
 * FC-P2.1 codec. There is not one raw wire offset in this file
 * (design SS3.9 rules 2 and 3) -- grep for a numeric byte index and you will
 * find none; the codec owns them all.
 *
 * PAGE CITES are to *VAXcluster Principles* (Davis 1993), ch. 2. The
 * transcript is copyrighted and host-only: cited, never quoted at length.
 */

#include "vms_scs_fsm.h"

/* ==========================================================================
 * A. Small helpers
 * ========================================================================== */

static void scs_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		b[i] = 0u;
}

static void scs_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static int scs_name_eq(const uint8_t *a, const uint8_t *b)
{
	uint32_t i;

	for (i = 0; i < VMS_SCS_PROCNAME_LEN; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/*
 * NOTE ON ops->now_ms. This FSM derives NO deadline of its own: every timeout
 * in the ladder is an ARMED timer (SCS_TIMER_CONNECT / SCS_TIMER_DISCONNECT)
 * whose expiry arrives as an event, so a host test drives time by firing the
 * timer rather than by winding a clock forward. `now_ms` stays in the ops
 * because the GLUE arms those timers against the real clock and FC-P2.3's
 * directory-lookup timeout needs it; nothing in this file calls it, which is
 * why there is no wrapper for it here.
 */

static void scs_log(const struct scs_fsm *f, const char *msg)
{
	if (f->ops != (const struct scs_fsm_ops *)0 &&
	    f->ops->log != (void (*)(void *, const char *))0)
		f->ops->log(f->ops->ctx, msg);
}

static void scs_arm(const struct scs_fsm *f, enum scs_timer which,
		    uint32_t key, uint32_t ms)
{
	if (f->ops != (const struct scs_fsm_ops *)0 &&
	    f->ops->arm_timer != (void (*)(void *, enum scs_timer, uint32_t,
					   uint32_t))0)
		f->ops->arm_timer(f->ops->ctx, which, key, ms);
}

static void scs_cancel(const struct scs_fsm *f, enum scs_timer which,
		       uint32_t key)
{
	if (f->ops != (const struct scs_fsm_ops *)0 &&
	    f->ops->cancel_timer != (void (*)(void *, enum scs_timer,
					      uint32_t))0)
		f->ops->cancel_timer(f->ops->ctx, which, key);
}

/* ==========================================================================
 * B. The CDL and the Con.ID allocator (vms_scs_fsm.h SS4)
 * ========================================================================== */

#define SCS_CONID_SLOT_MASK  0xffffu
#define SCS_CONID_UNIQ_SHIFT 16u

static uint32_t conid_slot(vms_conid_t id)
{
	/* ch. 2: "the low order 16 bits of the CONID ... are used as an index
	 * into the CDL". +1 was applied at mint (0 means "unbound" on the
	 * wire), so undo it here. */
	return (uint32_t)(id & SCS_CONID_SLOT_MASK);
}

static vms_conid_t conid_make(uint16_t uniq, uint32_t slot)
{
	return (vms_conid_t)(((uint32_t)uniq << SCS_CONID_UNIQ_SHIFT) |
			     ((slot + 1u) & SCS_CONID_SLOT_MASK));
}

struct scs_cdt *scs_fsm_cdt_at(struct scs_fsm *f, uint32_t index)
{
	if (f == (struct scs_fsm *)0 || f->cdl == (struct scs_cdt *)0)
		return (struct scs_cdt *)0;
	if (index >= f->n_cdl)
		return (struct scs_cdt *)0;
	return &f->cdl[index];
}

/*
 * THE CDL LOOKUP -- the only route from a wire Con.ID to a CDT. Index by the
 * low 16 bits (ch. 2), then verify the WHOLE value: a reused slot carries a
 * different uniquifier, so a stale Con.ID resolves to nothing rather than to
 * the plausible-looking connection that happens to occupy its slot now.
 */
struct scs_cdt *scs_fsm_cdt_by_conid(struct scs_fsm *f, vms_conid_t conid)
{
	struct scs_cdt *cdt;
	uint32_t slot = conid_slot(conid);

	if (f == (struct scs_fsm *)0 || slot == 0u)
		return (struct scs_cdt *)0;
	cdt = scs_fsm_cdt_at(f, slot - 1u);
	if (cdt == (struct scs_cdt *)0 || !cdt->in_use)
		return (struct scs_cdt *)0;
	if (cdt->local_conid != conid) {
		f->rx_conid_mismatch++;
		return (struct scs_cdt *)0;
	}
	return cdt;
}

/* Zero a CDL slot back to "free", preserving only the reuse generation. */
static void cdt_reset(struct scs_cdt *cdt, uint16_t generation)
{
	scs_bzero(cdt, (uint32_t)sizeof(*cdt));
	cdt->generation = generation;
	cdt->state = (uint8_t)VMS_SCS_CDT_CLOSED;
	cdt->sb_index = SCS_NIL;
	cdt->sb_next = SCS_NIL;
	cdt->listen_index = SCS_NIL;
	cdt->sw_head = SCS_NIL;
	cdt->sw_tail = SCS_NIL;
}

/* Take a free CDL slot and mint its Con.ID. Returns NULL and sets *why. */
static struct scs_cdt *cdt_alloc(struct scs_fsm *f, int *why)
{
	uint32_t i, slot;
	struct scs_cdt *cdt;

	if (f->cdl == (struct scs_cdt *)0 || f->n_cdl == 0u) {
		*why = SCS_ERR_NOCDT;
		return (struct scs_cdt *)0;
	}
	if (!f->conid.seeded) {
		/* vms_scs_fsm.h SS4: an unseeded allocator would repeat a
		 * Con.ID across incarnations, which SS4(t) says a real node
		 * cannot do. Refuse rather than mint a placeholder. */
		*why = SCS_ERR_NOCONID;
		return (struct scs_cdt *)0;
	}

	for (i = 0; i < f->n_cdl; i++) {
		slot = (f->conid.next_slot + i) % f->n_cdl;
		if (f->cdl[slot].in_use)
			continue;
		cdt = &f->cdl[slot];
		cdt_reset(cdt, cdt->generation);
		cdt->in_use = 1u;
		cdt->timer_key = slot;
		cdt->local_conid =
			conid_make((uint16_t)(f->conid.seed + cdt->generation),
				   slot);
		f->conid.next_slot = (slot + 1u) % f->n_cdl;
		f->conid.minted++;
		return cdt;
	}
	*why = SCS_ERR_NOCDT;
	return (struct scs_cdt *)0;
}

/* Return a CDT to the CDL, bumping its slot's reuse generation so the NEXT
 * connection in this slot gets a different Con.ID (SS4(t): never a repeat). */
static void cdt_release(struct scs_fsm *f, struct scs_cdt *cdt)
{
	(void)f;
	cdt_reset(cdt, (uint16_t)(cdt->generation + 1u));
}

static uint32_t cdt_index(const struct scs_fsm *f, const struct scs_cdt *cdt)
{
	return (uint32_t)(cdt - f->cdl);
}

/* ==========================================================================
 * C. The SB set and its CDT queue (ch. 2's Path Block queue)
 * ========================================================================== */

struct scs_sb *scs_fsm_sb_by_sysid(struct scs_fsm *f, vms_scs_sysid_t sysid)
{
	uint32_t i;

	if (f == (struct scs_fsm *)0 || f->sbs == (struct scs_sb *)0)
		return (struct scs_sb *)0;
	for (i = 0; i < f->n_sbs; i++) {
		if (f->sbs[i].in_use && f->sbs[i].peer_sysid == sysid)
			return &f->sbs[i];
	}
	return (struct scs_sb *)0;
}

struct scs_sb *scs_fsm_sb_at(struct scs_fsm *f, uint32_t index)
{
	if (f == (struct scs_fsm *)0 || f->sbs == (struct scs_sb *)0)
		return (struct scs_sb *)0;
	if (index >= f->n_sbs)
		return (struct scs_sb *)0;
	return f->sbs[index].in_use ? &f->sbs[index] : (struct scs_sb *)0;
}

static struct scs_sb *sb_find_or_alloc(struct scs_fsm *f,
				       vms_scs_sysid_t sysid)
{
	struct scs_sb *sb = scs_fsm_sb_by_sysid(f, sysid);
	uint32_t i;

	if (sb != (struct scs_sb *)0)
		return sb;
	if (f->sbs == (struct scs_sb *)0)
		return (struct scs_sb *)0;
	for (i = 0; i < f->n_sbs; i++) {
		if (!f->sbs[i].in_use) {
			scs_bzero(&f->sbs[i], (uint32_t)sizeof(f->sbs[i]));
			f->sbs[i].in_use = 1u;
			f->sbs[i].peer_sysid = sysid;
			f->sbs[i].cdt_head = SCS_NIL;
			return &f->sbs[i];
		}
	}
	return (struct scs_sb *)0;
}

static uint32_t sb_index(const struct scs_fsm *f, const struct scs_sb *sb)
{
	return (uint32_t)(sb - f->sbs);
}

/* "SCA specifies that all CDTs corresponding to connections supported by a
 * virtual circuit be queued to the Path Block corresponding to that circuit."
 * This is that queue, and scs_fsm_vc_down() is the scan it exists for. */
static void sb_queue_cdt(struct scs_fsm *f, struct scs_sb *sb,
			 struct scs_cdt *cdt)
{
	cdt->sb_index = sb_index(f, sb);
	cdt->sb_next = sb->cdt_head;
	sb->cdt_head = cdt_index(f, cdt);
	sb->n_cdts++;
}

static void sb_unqueue_cdt(struct scs_fsm *f, struct scs_cdt *cdt)
{
	struct scs_sb *sb;
	uint32_t self = cdt_index(f, cdt);
	uint32_t *link;
	uint32_t cur;

	if (cdt->sb_index == SCS_NIL || f->sbs == (struct scs_sb *)0)
		return;
	sb = &f->sbs[cdt->sb_index];
	link = &sb->cdt_head;
	cur = sb->cdt_head;
	while (cur != SCS_NIL && cur < f->n_cdl) {
		if (cur == self) {
			*link = f->cdl[cur].sb_next;
			if (sb->n_cdts > 0u)
				sb->n_cdts--;
			cdt->sb_index = SCS_NIL;
			cdt->sb_next = SCS_NIL;
			return;
		}
		link = &f->cdl[cur].sb_next;
		cur = f->cdl[cur].sb_next;
	}
	cdt->sb_index = SCS_NIL;
	cdt->sb_next = SCS_NIL;
}

/* ==========================================================================
 * D. The credit ledger (pp. 2-43..2-44)
 *
 * Five counters per CDT and one invariant:
 *     receive + held + pending == grant
 * Every function below preserves it by construction, and the R1 property test
 * asserts it after every step of a message/ack cycle.
 * ========================================================================== */

/* Extend `n` receive buffers to the peer. Done ONCE, when this end emits its
 * CONNECT_REQ or ACCEPT_REQ -- which are the only two frames the corpus shows
 * carrying an initial grant (spec SS4(h)(1c): types 0 and 2 carry 3/6/8/10;
 * types 1/3/4/5/6/7 carry 0 in 100% of frames). */
static void credit_extend(struct scs_cdt *cdt, uint16_t n)
{
	cdt->credit_grant = n;
	cdt->credit_receive = n;
	cdt->credit_held = 0u;
	cdt->credit_pending = 0u;
}

/*
 * The Pending Receive Credit going onto the wire, in TWO steps -- peek, then
 * commit ONLY IF THE FRAME ACTUALLY WENT OUT.
 *
 * p. 2-43 describes it as one action: "local SCS copies the local Pending
 * Receive Credit count into this credit field, and then resets to 0 the local
 * Pending Receive Credit count", after which the buffers count as extended
 * again. But a build or a transmit can fail, and doing both halves up front
 * would tell this node's ledger that N credits were returned to a peer that
 * never received them -- the count would be silently lost and this end would
 * believe the peer may send N messages it has no permission for. So the value
 * is READ for the frame, and the ledger moves only after the port took it.
 *
 * credit_pending_peek() IS THE ONLY PLACE A CREDIT VALUE IS PRODUCED FOR A
 * FRAME. Nothing else in this file writes the credit field of an outbound
 * message, which is what makes "the credit on the wire is a ledger read"
 * checkable by inspection.
 */
static uint16_t credit_pending_peek(const struct scs_cdt *cdt)
{
	return cdt->credit_pending;
}

static void credit_pending_commit(struct scs_cdt *cdt, uint16_t n)
{
	if (n > cdt->credit_pending)
		return;                 /* cannot happen; never go negative */
	cdt->credit_pending = (uint16_t)(cdt->credit_pending - n);
	cdt->credit_receive = (uint16_t)(cdt->credit_receive + n);
}

/* A message in the account arrived: it consumed one of the buffers we
 * extended, and the SYSAP now holds it. Types 8, 9 and 10 -- spec SS4(h)(1c),
 * whose ledger replay agreed 938 of 938 with exactly this rule. */
static void credit_consume_receive(struct scs_fsm *f, struct scs_cdt *cdt)
{
	if (cdt->credit_receive == 0u) {
		/* The peer sent past the window it was granted. Counted and
		 * still delivered: dropping a message the port already
		 * acknowledged would be the worse failure. */
		cdt->credit_overruns++;
		(void)f;
		return;
	}
	cdt->credit_receive--;
	cdt->credit_held++;
}

/* Credit the peer carried to us: "remote SCS adds the content of the credit
 * field to the Send Credit count it associates with the connection". */
static void credit_receive_grant(struct scs_cdt *cdt, uint16_t n)
{
	cdt->credit_send = (uint16_t)(cdt->credit_send + n);
}

/* p. 2-44's "dangerously low" test. The SCA rule is
 *     local Receive Credit < SCSFLOWCUSH + remote Minimum Send Credits
 * and the remote term's WIRE offset is not grounded anywhere in the spec, so
 * it is added only when the peer actually told us (it never has yet -- see
 * scs_cdt.peer_min_send_credits). Running on the cushion alone makes the
 * trigger FIRE EARLIER, never later, and every byte of the message it sends
 * is still a real ledger read; the partial threshold is counted so the
 * difference is visible rather than assumed away. */
static int credit_dangerously_low(struct scs_fsm *f, struct scs_cdt *cdt)
{
	uint32_t threshold = f->cfg.flowcush;

	if (cdt->peer_min_send_credits_valid)
		threshold += cdt->peer_min_send_credits;
	else
		f->credit_msg_partial_threshold++;

	return (uint32_t)cdt->credit_receive < threshold;
}

/* ==========================================================================
 * E. Frame construction -- every byte through the FC-P2.1 codec
 * ========================================================================== */

/* Which SCA content length each verb occupies (spec SS4(m)'s own table). */
static uint16_t ctrl_content_for_op(uint16_t op)
{
	switch (op) {
	case SCS_MTYPE_CON_REQ:   /* op 0, SS4(m) "CONNECT-REQUEST"  110 */
	case SCS_MTYPE_ACCP_REQ:  /* op 2, SS4(m) "CONNECT-RESPONSE" 110 */
		return VMS_SCSCTRL_LEN_CONNECT;
	case SCS_MTYPE_CON_RSP:   /* op 1, SS4(m) "CONNECT-ECHO"      66 */
		return VMS_SCSCTRL_LEN_ECHO;
	case SCS_MTYPE_ACCP_RSP:  /* op 3, SS4(m) "CONNECT-CONFIRM"   62 */
	case SCS_MTYPE_REJ_REQ:   /* op 4, REJECT-REQUEST             62 */
	case SCS_MTYPE_DISC_REQ:  /* op 6, DISCONNECT-REQUEST         62 */
		return VMS_SCSCTRL_LEN_MARKER;
	case SCS_MTYPE_APPL_MSG:
		/*
		 * op 10 has TWO grounded classes and this is the only one SCS
		 * builds WHOLE: the 94-content directory message (SS4(h)(2a),
		 * "the capture holds 12 lookup messages, all of length 94").
		 * The other -- the 190-content class -- is assembled through
		 * the body-level seam by the port and never reaches here.
		 */
		return VMS_SCSCTRL_LEN_LOOKUP;
	default:                  /* ops 5/7/8/9: envelope only       58 */
		return VMS_SCSCTRL_LEN_SHORT;
	}
}

/* Set the has_* flags, the inner length and the SCA length field from ONE
 * content length, so the three cannot disagree. */
static void ctrl_set_shape(struct vms_scs_ctrl_frame *c, uint16_t content)
{
	c->has_marker = (uint8_t)(content >= VMS_SCSCTRL_LEN_MARKER);
	c->has_tail4  = (uint8_t)(content == VMS_SCSCTRL_LEN_ECHO);
	c->has_names  = (uint8_t)(content == VMS_SCSCTRL_LEN_LOOKUP ||
				  content == VMS_SCSCTRL_LEN_CONNECT);
	c->has_blank  = (uint8_t)(content == VMS_SCSCTRL_LEN_CONNECT);
	c->inner_len  = (uint16_t)(content - SCS_INNERLEN_BIAS);
	c->hdr.sca_len_field = (uint16_t)(content - 2u);
}

/*
 * Spec SS4(m)'s msgtype phase rule, applied from the CDT's OWN phase flag.
 * This is SCS knowledge (design SS3.2.4: "the 0x5b/0x4b phase rule is SCS
 * knowledge, never a SYSAP byte").
 */
static uint8_t cdt_msgtype(const struct scs_cdt *cdt)
{
	return cdt->data_phase ? (uint8_t)VMS_SCS_MT_MSG
			       : (uint8_t)VMS_SCS_MT_SETUP;
}

/*
 * Fill the parts of a control frame that are the same for every verb.
 *
 * ABS 32-55 IS LEFT ZERO ON PURPOSE. Design SS3.2.4 gives 32-35 (recv_ack/
 * send_seq) and 36-55 (the incarnation and counter mirrors) to the PORT: the
 * port stamps 32/34/44 at transmit time (vms_scs_seq_stamp) and has no
 * generic builder for the rest yet -- FC-P1.1 built one only for START/STACK/
 * ACK and the credit-return short. So SCS passes zeros there, exactly as
 * pe_vc_send_msg does for the 190-content class, and counts on the honesty of
 * that zero rather than on a template (INV-6). The same reasoning covers the
 * abs-72 marker word: SS4(h)(1a) grounds semantics for op 6's marker[2:4]
 * alone, and no capture isolates its encoding, so it goes out zero rather
 * than carrying an invented flag.
 */
static int ctrl_prepare(struct scs_fsm *f, const struct scs_cdt *cdt,
			uint16_t op, struct vms_scs_ctrl_frame *c)
{
	struct vms_scs_addr addr;

	scs_bzero(c, (uint32_t)sizeof(*c));
	if (f->ops == (const struct scs_fsm_ops *)0 ||
	    f->ops->addr == (int (*)(void *, vms_scs_sysid_t,
				     struct vms_scs_addr *))0)
		return SCS_ERR_ADDR;
	if (f->ops->addr(f->ops->ctx, cdt->peer_sysid, &addr) != 0) {
		/* No circuit, no real addressing, no frame. */
		f->tx_refused_addr++;
		return SCS_ERR_ADDR;
	}

	scs_copy(c->hdr.eth_dst, addr.dst_mac, VMS_ETH_ADDR_LEN);
	scs_copy(c->hdr.eth_src, addr.src_mac, VMS_ETH_ADDR_LEN);
	scs_copy(c->hdr.dst_lavc, addr.dst_logical, VMS_ETH_ADDR_LEN);
	scs_copy(c->hdr.src_lavc, addr.src_logical, VMS_ETH_ADDR_LEN);
	c->hdr.connect_flag = VMS_SCSCTRL_CONNECT_FLAG;
	c->hdr.word30 = (uint16_t)(cdt_msgtype(cdt) |
				   ((uint16_t)VMS_SCS_FORMAT_V13 << 8));

	c->op = op;
	c->conid_remote = cdt->remote_conid_valid ? cdt->remote_conid : 0u;
	c->conid_local = cdt->local_conid;
	ctrl_set_shape(c, ctrl_content_for_op(op));
	return SCS_OK;
}

/* Build and transmit one control verb. Returns SCS_OK or a refusal; on any
 * refusal NOTHING went out. */
static int ctrl_emit(struct scs_fsm *f, struct scs_cdt *cdt,
		     struct vms_scs_ctrl_frame *c)
{
	uint32_t written = 0u;

	if (vms_scs_ctrl_build(c, f->ctrlbuf, (uint32_t)sizeof(f->ctrlbuf),
			       &written) != VMS_CODEC_OK) {
		f->tx_refused_codec++;
		return SCS_ERR_CODEC;
	}
	if (f->ops->send_ctrl == (int (*)(void *, vms_scs_sysid_t,
					  const uint8_t *, uint32_t))0)
		return SCS_ERR_TXFAIL;
	if (f->ops->send_ctrl(f->ops->ctx, cdt->peer_sysid, f->ctrlbuf,
			      written) != 0) {
		f->tx_errors++;
		return SCS_ERR_TXFAIL;
	}
	return SCS_OK;
}

/*
 * A verb that is OUTSIDE the credit account (spec SS4(h)(1c): types 1/3/4/5/
 * 6/7 carry credit 0 in 100% of real-VAX frames -- "connection-control frames
 * neither consume a receive buffer nor return one"). The credit field is
 * therefore a literal 0 here, and that 0 is the measured value, not a
 * placeholder.
 */
static int ctrl_send_plain(struct scs_fsm *f, struct scs_cdt *cdt, uint16_t op)
{
	struct vms_scs_ctrl_frame c;
	int rc = ctrl_prepare(f, cdt, op, &c);

	if (rc != SCS_OK)
		return rc;
	c.credit = 0u;
	return ctrl_emit(f, cdt, &c);
}

/*
 * A verb that IS inside the account and carries the ledger's pending count:
 * the initial grant on ops 0/2, and the special credit message op 8. The
 * value comes from credit_take_pending()/credit_extend() -- never a constant.
 */
static int ctrl_send_grant(struct scs_fsm *f, struct scs_cdt *cdt, uint16_t op,
			   uint16_t grant)
{
	struct vms_scs_ctrl_frame c;
	int rc = ctrl_prepare(f, cdt, op, &c);

	if (rc != SCS_OK)
		return rc;
	c.credit = grant;
	scs_copy(c.name1, cdt->remote_name, VMS_SCSCTRL_NAME_LEN);
	scs_copy(c.name2, cdt->local_name, VMS_SCSCTRL_NAME_LEN);
	return ctrl_emit(f, cdt, &c);
}

/* ==========================================================================
 * F. Application messages -- TWO grounded classes, ONE account
 *
 * MTYPE 10 rides the 190-content class (a 132-byte SYSAP body, split through
 * design SS3.2.4's body-level seam and finished by the port) and the
 * 94-content directory class (a 36-byte body, SS4(h)(2), which no lower layer
 * can pre-build so SCS builds it whole -- the same reason the connect verbs
 * do). The SHAPE differs; the ACCOUNT does not: either way one Send Credit is
 * spent, the piggybacked credit is one ledger read, and the msgtype phase flag
 * turns over. That is why the ledger steps live in one place below.
 * ========================================================================== */

/* Everything that becomes true once the port has TAKEN a message. */
static void msg_accounted(struct scs_cdt *cdt, uint16_t credit_sent)
{
	credit_pending_commit(cdt, credit_sent);
	cdt->credit_send--;
	cdt->msgs_sent++;
	cdt->data_phase = 1u;   /* spec SS4(m): 0x5b -> 0x4b after the first */
}

/*
 * The two directory tallies vms_scs_view reports ("lookups this node ANSWERED"
 * / "lookups this node ISSUED"). They are read off the SYSAP's OWN body, whose
 * marker word SS4(h)(2a) grounds as the request/response discriminator, so
 * each counts a frame this node actually put on the wire. A marker outside
 * {0,1} counts as neither -- it is not forced into one bucket.
 */
static void dir_count_tx(struct scs_fsm *f, const uint8_t *sysap_body)
{
	struct vms_scs_dir_msg m;

	if (vms_scs_dir_msg_parse(sysap_body, SCS_DIR_BODY_LEN, &m) !=
	    VMS_CODEC_OK)
		return;
	if (m.marker == VMS_SCS_DIR_MARKER_RESPONSE)
		f->dir_lookups_served++;
	else if (m.marker == VMS_SCS_DIR_MARKER_REQUEST)
		f->dir_lookups_sent++;
}

/* The 190-content class: 16 bytes of SCS header + the SYSAP's 132, handed to
 * the port at the body-level seam. */
static int msg_transmit_long(struct scs_fsm *f, struct scs_cdt *cdt,
			     const uint8_t *sysap_body)
{
	struct vms_scs_hdr h;
	int rc;

	scs_bzero(&h, (uint32_t)sizeof(h));
	h.inner_len = SCS_MSG_INNER_LEN;
	h.mtype = (uint16_t)SCS_MTYPE_APPL_MSG;
	h.conid_remote = cdt->remote_conid;
	h.conid_local = cdt->local_conid;
	/* THE LEDGER READ. Piggyback whatever the local SYSAP has released
	 * since the last outbound message -- the p. 2-43 rule, executed, not
	 * imitated. The ledger moves only once the port has taken the frame. */
	h.credit = credit_pending_peek(cdt);

	if (vms_scs_msg_body_build(&h, sysap_body, SCS_SYSAP_BODY_LEN,
				   f->msgbuf,
				   (uint32_t)sizeof(f->msgbuf)) != VMS_CODEC_OK) {
		f->tx_refused_codec++;
		return SCS_ERR_CODEC;
	}
	if (f->ops->send_msg == (int (*)(void *, vms_scs_sysid_t, vms_conid_t,
					 const uint8_t *, uint32_t))0)
		return SCS_ERR_TXFAIL;
	rc = f->ops->send_msg(f->ops->ctx, cdt->peer_sysid, cdt->remote_conid,
			      f->msgbuf, (uint32_t)sizeof(f->msgbuf));
	if (rc != 0) {
		f->tx_errors++;
		return SCS_ERR_TXFAIL;
	}

	msg_accounted(cdt, h.credit);
	return SCS_OK;
}

/*
 * The 94-content directory class. The SYSAP's 36 bytes go in through the
 * codec's body-level entry (vms_scs_dir_ctrl_from_body), so this file still
 * holds no wire offset, and the credit field is the same ledger read the
 * 190-content path makes -- SS4(h)(2a) measures a REAL piggybacked credit on
 * this class ("on the 94-byte lookup class the same field is the ordinary
 * piggybacked credit"), and the strawman's constant 0 there is recorded in the
 * spec as a KNOWN DEVIATION (SS4h gap (f)) that this path does not repeat.
 */
static int msg_transmit_short(struct scs_fsm *f, struct scs_cdt *cdt,
			      const uint8_t *sysap_body)
{
	struct vms_scs_ctrl_frame c;
	uint16_t credit;
	int rc = ctrl_prepare(f, cdt, (uint16_t)SCS_MTYPE_APPL_MSG, &c);

	if (rc != SCS_OK)
		return rc;
	credit = credit_pending_peek(cdt);
	c.credit = credit;
	if (vms_scs_dir_ctrl_from_body(sysap_body, SCS_DIR_BODY_LEN, &c) !=
	    VMS_CODEC_OK) {
		f->tx_refused_codec++;
		return SCS_ERR_CODEC;
	}
	rc = ctrl_emit(f, cdt, &c);
	if (rc != SCS_OK)
		return rc;

	msg_accounted(cdt, credit);
	dir_count_tx(f, sysap_body);
	return SCS_OK;
}

/*
 * THE THIRD SHAPE (FC-P6.3, vms_scs_fsm.h SS1). A SYSAP body at a length neither
 * grounded class holds -- the MSCP server's 28/32/44/52-byte end messages -- is
 * assembled from this layer's own 16-byte header plus the SYSAP's bytes and
 * handed to the port's variable-length entry. The credit field is the SAME
 * ledger read the other two make; nothing here is padded and nothing invented.
 */
static int msg_transmit_var(struct scs_fsm *f, struct scs_cdt *cdt,
			    const uint8_t *sysap_body, uint32_t len)
{
	struct vms_scs_hdr h;
	uint32_t body_len = VMS_SCS_HDR_LEN + len;
	int rc;

	if (f->ops->send_msg_var == (int (*)(void *, vms_scs_sysid_t,
					     vms_conid_t, const uint8_t *,
					     uint32_t))0)
		return SCS_ERR_TXFAIL;

	scs_bzero(&h, (uint32_t)sizeof(h));
	h.inner_len = SCS_SYSAP_INNER_LEN(len);
	h.mtype = (uint16_t)SCS_MTYPE_APPL_MSG;
	h.conid_remote = cdt->remote_conid;
	h.conid_local = cdt->local_conid;
	h.credit = credit_pending_peek(cdt);

	if (vms_scs_msg_body_build(&h, sysap_body, len, f->msgbuf,
				   body_len) != VMS_CODEC_OK) {
		f->tx_refused_codec++;
		return SCS_ERR_CODEC;
	}
	rc = f->ops->send_msg_var(f->ops->ctx, cdt->peer_sysid,
				  cdt->remote_conid, f->msgbuf, body_len);
	if (rc != 0) {
		f->tx_errors++;
		return SCS_ERR_TXFAIL;
	}

	msg_accounted(cdt, h.credit);
	return SCS_OK;
}

/* The class is chosen by the SYSAP's own length and by nothing else. */
static int msg_transmit(struct scs_fsm *f, struct scs_cdt *cdt,
			const uint8_t *sysap_body, uint32_t len)
{
	if (len == SCS_SYSAP_BODY_LEN)
		return msg_transmit_long(f, cdt, sysap_body);
	if (len == SCS_DIR_BODY_LEN)
		return msg_transmit_short(f, cdt, sysap_body);
	if (len > 0u && len < SCS_SYSAP_BODY_LEN)
		return msg_transmit_var(f, cdt, sysap_body, len);
	return SCS_ERR_INVAL;
}

/* ==========================================================================
 * G. Credit Wait (p. 2-45) -- the FIFO of sends held for a Send Credit
 * ========================================================================== */

static int sendwait_push(struct scs_fsm *f, struct scs_cdt *cdt,
			 const uint8_t *body, uint32_t len)
{
	uint32_t i;

	/* Any body msg_transmit() can carry, Credit Wait can hold: the pool's
	 * slot is SCS_SYSAP_BODY_LEN wide, which is the largest of them. */
	if (len == 0u || len > SCS_SYSAP_BODY_LEN)
		return SCS_ERR_INVAL;
	if (f->sw == (struct scs_sendwait *)0 || f->n_sw == 0u)
		return SCS_ERR_NOCREDIT;    /* no pool bound: honest refusal */
	for (i = 0; i < f->n_sw; i++) {
		if (f->sw[i].in_use)
			continue;
		scs_bzero(&f->sw[i], (uint32_t)sizeof(f->sw[i]));
		f->sw[i].in_use = 1u;
		f->sw[i].next = SCS_NIL;
		f->sw[i].cdt_index = cdt_index(f, cdt);
		f->sw[i].len = len;
		scs_copy(f->sw[i].body, body, len);
		if (cdt->sw_tail == SCS_NIL)
			cdt->sw_head = i;
		else
			f->sw[cdt->sw_tail].next = i;
		cdt->sw_tail = i;
		cdt->sw_count++;
		cdt->credit_stalls++;
		f->credit_stalls++;
		return SCS_OK;
	}
	return SCS_ERR_NOCREDIT;
}

static uint32_t sendwait_pop(struct scs_fsm *f, struct scs_cdt *cdt)
{
	uint32_t i = cdt->sw_head;

	if (i == SCS_NIL)
		return SCS_NIL;
	cdt->sw_head = f->sw[i].next;
	if (cdt->sw_head == SCS_NIL)
		cdt->sw_tail = SCS_NIL;
	if (cdt->sw_count > 0u)
		cdt->sw_count--;
	return i;
}

/* "Whenever the Send Credit count in a CDT is increased, the CDT's queue of
 * waiting CDRPs is examined. If that queue is nonempty, as many waiting CDRPs
 * as possible are resumed, based on the number of Send Credits currently
 * available." (p. 2-45) */
static void sendwait_drain(struct scs_fsm *f, struct scs_cdt *cdt)
{
	while (cdt->sw_head != SCS_NIL && cdt->credit_send > 0u &&
	       cdt->state == (uint8_t)VMS_SCS_CDT_OPEN) {
		uint32_t i = sendwait_pop(f, cdt);

		if (i == SCS_NIL)
			return;
		if (msg_transmit(f, cdt, f->sw[i].body, f->sw[i].len) !=
		    SCS_OK) {
			/* The port refused it. Put it back at the head; the
			 * next credit or the next drain retries -- SCS has not
			 * lost the message and has not sent it twice. */
			f->sw[i].next = cdt->sw_head;
			cdt->sw_head = i;
			if (cdt->sw_tail == SCS_NIL)
				cdt->sw_tail = i;
			cdt->sw_count++;
			return;
		}
		f->sw[i].in_use = 0u;
	}
}

/* Every queued send on this CDT fails. SCS NEVER RETRIES ACROSS A BREAK
 * (design SS3.2.5), so the queue is emptied and the SYSAP is TOLD. */
static void sendwait_fail_all(struct scs_fsm *f, struct scs_cdt *cdt,
			      uint32_t reason)
{
	while (cdt->sw_head != SCS_NIL) {
		uint32_t i = sendwait_pop(f, cdt);

		if (i == SCS_NIL)
			break;
		f->sw[i].in_use = 0u;
		cdt->sends_failed_pathlost++;
		if (cdt->sysap != (const struct scs_sysap_ops *)0 &&
		    cdt->sysap->send_failed !=
		    (void (*)(void *, vms_conid_t, uint32_t))0)
			cdt->sysap->send_failed(cdt->sysap->ctx,
						cdt->local_conid, reason);
	}
}

/* ==========================================================================
 * H. Closing a CDT
 * ========================================================================== */

static void cdt_close(struct scs_fsm *f, struct scs_cdt *cdt,
		      enum scs_close_reason reason)
{
	const struct scs_sysap_ops *sysap = cdt->sysap;
	vms_conid_t conid = cdt->local_conid;

	scs_cancel(f, SCS_TIMER_CONNECT, cdt->timer_key);
	scs_cancel(f, SCS_TIMER_DISCONNECT, cdt->timer_key);

	/* The ledger is DISCARDED, not carried: a re-formed circuit starts new
	 * connections with fresh credits (design SS3.2.5). */
	sendwait_fail_all(f, cdt, (uint32_t)reason);
	cdt->credit_grant = 0u;
	cdt->credit_send = 0u;
	cdt->credit_receive = 0u;
	cdt->credit_held = 0u;
	cdt->credit_pending = 0u;

	cdt->state = (uint8_t)VMS_SCS_CDT_CLOSED;
	cdt->close_reason = (uint8_t)reason;
	sb_unqueue_cdt(f, cdt);

	if (sysap != (const struct scs_sysap_ops *)0 &&
	    sysap->closed != (void (*)(void *, vms_conid_t, uint32_t))0)
		sysap->closed(sysap->ctx, conid, (uint32_t)reason);

	cdt_release(f, cdt);
}

/* ==========================================================================
 * I. The event record a handler is given
 * ========================================================================== */

struct scs_rx {
	vms_scs_sysid_t                  from;
	const struct vms_scs_hdr        *hdr;   /* the SS4(h)(1b) envelope    */
	const struct vms_scs_ctrl_frame *ctrl;  /* NULL unless the connect
						 * classes' richer parse ran  */
	const uint8_t                   *body;  /* SYSAP bytes / LOCAL_SEND   */
	uint32_t                         body_len;
	const struct scs_connect_args   *args;  /* LOCAL_CONNECT only         */
	const uint8_t                   *conndata; /* LOCAL_ACCEPT only       */
	uint32_t                         out_conid; /* handler's answer       */
};

typedef int (*scs_handler_t)(struct scs_fsm *f, struct scs_cdt *cdt,
			     struct scs_rx *rx);

/* The handful of mutual recursions the ladder genuinely has: an inbound
 * connect can accept or reject in the same call, an op 9 releases the op 6,
 * and every answered request returns its listening CDT to LISTEN. */
static void listen_reset(struct scs_fsm *f, struct scs_cdt *listen);
static void disc_advance(struct scs_fsm *f, struct scs_cdt *cdt);
static int  disc_emit_req(struct scs_fsm *f, struct scs_cdt *cdt);
static int  h_local_reject(struct scs_fsm *f, struct scs_cdt *listen,
			   struct scs_rx *rx);

static void cdt_set_state(struct scs_cdt *cdt, enum vms_scs_cdt_state s)
{
	cdt->state = (uint8_t)s;
}

static const struct scs_sdir *sdir_by_index(const struct scs_fsm *f,
					    uint32_t index)
{
	if (index >= SCS_MAX_SYSAPS || !f->sdir[index].in_use)
		return (const struct scs_sdir *)0;
	return &f->sdir[index];
}

static struct scs_sdir *sdir_by_name(struct scs_fsm *f, const uint8_t *name)
{
	uint32_t i;

	for (i = 0; i < SCS_MAX_SYSAPS; i++) {
		if (f->sdir[i].in_use && scs_name_eq(f->sdir[i].name, name))
			return &f->sdir[i];
	}
	return (struct scs_sdir *)0;
}

/* ==========================================================================
 * J. Handlers -- the CONNECT half of the ladder
 * ========================================================================== */

/*
 * [CLOSED] LOCAL_CONNECT -- the initiator's op 0.
 *
 * The credit field carries what THIS end has just extended, read straight out
 * of the ledger credit_extend() set (spec SS4(h)(1c): CONNECT_REQ is one of the
 * only two verbs that ever carries a non-zero credit).
 */
static int h_local_connect(struct scs_fsm *f, struct scs_cdt *cdt,
			   struct scs_rx *rx)
{
	int rc;

	(void)rx;
	credit_extend(cdt, cdt->credit_grant);
	rc = ctrl_send_grant(f, cdt, (uint16_t)SCS_MTYPE_CON_REQ,
			     cdt->credit_receive);
	if (rc != SCS_OK)
		return rc;

	cdt->initiator = 1u;
	cdt_set_state(cdt, VMS_SCS_CDT_CONNECT_SENT);
	scs_arm(f, SCS_TIMER_CONNECT, cdt->timer_key, f->cfg.connect_timeout_ms);
	return SCS_OK;
}

/*
 * [CONNECT_SENT] RX op 1 -- CONNECT-ECHO. "'received'; echoes remote_conid =
 * initiator's handle, local_conid still 0. EVERY accept emits this first"
 * (spec SS4(m)). It is an acknowledgement of receipt, not a state change: the
 * connection is still waiting for the op 2 that binds the Con.ID pair. It
 * carries credit 0 (SS4(h)(1c), 100 % of real-VAX type-1 frames), so it does
 * NOT touch the ledger.
 */
static int h_rx_echo(struct scs_fsm *f, struct scs_cdt *cdt, struct scs_rx *rx)
{
	(void)f;
	(void)rx;
	cdt->echo_rcvd = 1u;
	return SCS_OK;
}

/* Emit the op-3 CONFIRM and, if it goes out, open the connection.
 *
 * The confirm is LOAD-BEARING (spec SS4(m), vms-760): "Omit frame 139 and
 * everything from 145 onward disappears" -- a joiner that never confirms gets
 * its config burst bound and silently discarded, and the member never opens
 * connections back. So a confirm that could not be transmitted leaves the CDT
 * in ACCEPT_RCVD, where the connect timer retries it, rather than declaring a
 * connection open that the peer regards as half-open.
 */
static int h_send_confirm(struct scs_fsm *f, struct scs_cdt *cdt)
{
	int rc = ctrl_send_plain(f, cdt, (uint16_t)SCS_MTYPE_ACCP_RSP);

	if (rc != SCS_OK)
		return rc;

	cdt->confirmed = 1u;
	cdt_set_state(cdt, VMS_SCS_CDT_OPEN);
	scs_cancel(f, SCS_TIMER_CONNECT, cdt->timer_key);
	if (cdt->sysap != (const struct scs_sysap_ops *)0 &&
	    cdt->sysap->opened != (void (*)(void *, vms_conid_t))0)
		cdt->sysap->opened(cdt->sysap->ctx, cdt->local_conid);
	sendwait_drain(f, cdt);
	return SCS_OK;
}

/*
 * [CONNECT_SENT] RX op 2 -- CONNECT-RESPONSE, the ACCEPT. It "supplies the
 * acceptor's handle in local_conid; binds the Con.ID pair" (SS4(m)) and it
 * carries the peer's INITIAL GRANT (SS4(h)(1c): type 2 values 1/6/8/10).
 *
 * That grant is the ONLY source of Send Credit on this connection. If the peer
 * granted 0 this end can never send, and it will say so (SCS_ERR_NOCREDIT)
 * rather than invent a window -- the same rule the port applies to a circuit
 * that opened without a credit-window body.
 */
static int h_rx_accept(struct scs_fsm *f, struct scs_cdt *cdt,
		       struct scs_rx *rx)
{
	if (rx->hdr == (const struct vms_scs_hdr *)0)
		return SCS_ERR_INVAL;

	cdt->remote_conid = rx->hdr->conid_local;
	cdt->remote_conid_valid = 1u;
	credit_receive_grant(cdt, rx->hdr->credit);
	cdt_set_state(cdt, VMS_SCS_CDT_ACCEPT_RCVD);
	return h_send_confirm(f, cdt);
}

/* [ACCEPT_RCVD] the connect timer: the confirm never made it out. Retry once
 * per beat; the circuit's own TIMVCFAIL is the backstop under this. */
static int h_confirm_retry(struct scs_fsm *f, struct scs_cdt *cdt,
			   struct scs_rx *rx)
{
	(void)rx;
	if (h_send_confirm(f, cdt) == SCS_OK)
		return SCS_OK;
	scs_log(f, "%SCS-W-NOCONFIRM, connection confirm could not be sent");
	cdt_close(f, cdt, SCS_CLOSE_TIMEOUT);
	return SCS_OK;
}

/* ==========================================================================
 * K. Handlers -- the ACCEPTOR half (ch. 2's listening CDT)
 * ========================================================================== */

/* The op-1 echo: GROUNDED to carry local_conid = 0 (SS4(m)), which is also the
 * truth -- on this end no connection CDT exists yet. ch. 2: the connection's
 * own CDT is allocated only when the SYSAP accepts. */
static int ctrl_send_echo(struct scs_fsm *f, struct scs_cdt *listen)
{
	struct vms_scs_ctrl_frame c;
	int rc = ctrl_prepare(f, listen, (uint16_t)SCS_MTYPE_CON_RSP, &c);

	if (rc != SCS_OK)
		return rc;
	c.credit = 0u;
	c.conid_local = 0u;
	return ctrl_emit(f, listen, &c);
}

/*
 * Record on the listening CDT everything the inbound op 0 told us, and queue
 * it to the requester's Path Block for as long as it holds the request.
 *
 * THE INITIATOR'S GRANT ARRIVES HERE, and the connection's own CDT does not
 * exist yet (ch. 2). It is parked on the listening CDT's Send Credit -- the
 * right field, since that is exactly "messages this end may send to that
 * peer" -- and handed to the real CDT in h_local_accept(). Losing it is how an
 * acceptor ends up unable to send on a connection the initiator believes it
 * granted six buffers on; the R1 end-to-end test asserts
 * A.credit_send == B.credit_grant in BOTH directions for that reason.
 *
 * THE PB QUEUE matters even for a listening CDT: ch. 2 makes it how a broken
 * circuit finds everything it took down, and a listening CDT left in CONNECT
 * RECEIVED for a system that has gone away can never accept another connect,
 * because ch. 2 lets a SYSAP hold only one at a time. listen_reset() takes it
 * back off.
 */
static void listen_record_request(struct scs_fsm *f, struct scs_cdt *listen,
				  const struct scs_rx *rx)
{
	struct scs_sb *sb;

	listen->peer_sysid = rx->from;
	listen->remote_conid = rx->hdr->conid_local;
	listen->remote_conid_valid = 1u;
	listen->credit_send = rx->hdr->credit;
	scs_copy(listen->remote_name, rx->ctrl->name2, VMS_SCS_PROCNAME_LEN);
	/* spec SS4(N): SCA content [94:110] IS the 16-byte connect data. */
	scs_copy(listen->conndata, rx->ctrl->blank, VMS_SCS_PROCNAME_LEN);

	sb = sb_find_or_alloc(f, rx->from);
	if (sb != (struct scs_sb *)0)
		sb_queue_cdt(f, sb, listen);
}

/* Put the recorded request to the SYSAP and act on its answer. */
static int listen_ask_sysap(struct scs_fsm *f, struct scs_cdt *listen,
			    const struct scs_sdir *sd, struct scs_rx *rx)
{
	int decision;

	if (sd->ops == (const struct scs_sysap_ops *)0 ||
	    sd->ops->connect_req == (int (*)(void *, vms_conid_t,
					     vms_scs_sysid_t, vms_conid_t,
					     const uint8_t *, uint32_t))0)
		return scs_fsm_reject(f, listen->local_conid);

	decision = sd->ops->connect_req(sd->ops->ctx, listen->local_conid,
					listen->peer_sysid,
					listen->remote_conid,
					listen->conndata,
					VMS_SCS_PROCNAME_LEN);
	if (decision == SCS_CONNECT_DEFER)
		return SCS_OK;
	if (decision == 0)
		return scs_fsm_accept(f, listen->local_conid,
				      (const uint8_t *)0, &rx->out_conid);
	return scs_fsm_reject(f, listen->local_conid);
}

/* [LISTEN] RX op 0 -- a connect naming this SYSAP. */
static int h_rx_con_req(struct scs_fsm *f, struct scs_cdt *listen,
			struct scs_rx *rx)
{
	const struct scs_sdir *sd = sdir_by_index(f, listen->listen_index);

	if (rx->hdr == (const struct vms_scs_hdr *)0 ||
	    rx->ctrl == (const struct vms_scs_ctrl_frame *)0 ||
	    sd == (const struct scs_sdir *)0)
		return SCS_ERR_INVAL;

	listen_record_request(f, listen, rx);

	/* "Every accept emits this first" (SS4(m)); ch. 2 calls it the
	 * CONNECT_RSP that says the request reached the target SYSAP. */
	if (ctrl_send_echo(f, listen) != SCS_OK) {
		sb_unqueue_cdt(f, listen);
		return SCS_ERR_TXFAIL;
	}

	cdt_set_state(listen, VMS_SCS_CDT_CONNECT_RCVD);
	scs_arm(f, SCS_TIMER_CONNECT, listen->timer_key,
		f->cfg.connect_timeout_ms);
	return listen_ask_sysap(f, listen, sd, rx);
}

/*
 * [CONNECT_RCVD] RX op 0 -- a SECOND connect while the SYSAP is still deciding
 * about the first. ch. 2: SCS "replies with a response that essentially says
 * 'busy ... try again later'", and a SYSAP handles one incoming connect at a
 * time. The exact busy encoding is not isolated on our wire, so this end sends
 * the one refusal the wire DOES ground -- REJECT_REQUEST (spec SS4(h)(1h): 733
 * frames, terminal, never followed by application traffic) -- and counts it
 * separately, rather than inventing a "busy" status byte.
 */
static int h_rx_con_req_busy(struct scs_fsm *f, struct scs_cdt *listen,
			     struct scs_rx *rx)
{
	struct vms_scs_ctrl_frame c;
	int rc;

	if (rx->hdr == (const struct vms_scs_hdr *)0)
		return SCS_ERR_INVAL;

	rc = ctrl_prepare(f, listen, (uint16_t)SCS_MTYPE_REJ_REQ, &c);
	if (rc != SCS_OK)
		return rc;
	/* Addressed to the NEW requester, not to the one being decided. */
	c.conid_remote = rx->hdr->conid_local;
	c.conid_local = 0u;
	c.credit = 0u;
	rc = ctrl_emit(f, listen, &c);
	if (rc == SCS_OK) {
		f->connect_busy++;
		f->connects_rejected++;
	}
	return rc;
}

/* [CONNECT_RCVD] LOCAL_ACCEPT -- allocate the connection's OWN CDT. */
static int h_local_accept(struct scs_fsm *f, struct scs_cdt *listen,
			  struct scs_rx *rx)
{
	const struct scs_sdir *sd = sdir_by_index(f, listen->listen_index);
	struct scs_cdt *cdt;
	struct scs_sb *sb;
	int why = SCS_ERR_NOCDT;
	int rc;

	if (sd == (const struct scs_sdir *)0)
		return SCS_ERR_INVAL;
	sb = sb_find_or_alloc(f, listen->peer_sysid);
	if (sb == (struct scs_sb *)0)
		return SCS_ERR_NOSB;

	cdt = cdt_alloc(f, &why);
	if (cdt == (struct scs_cdt *)0)
		return why;

	cdt->peer_sysid = listen->peer_sysid;
	cdt->remote_conid = listen->remote_conid;
	cdt->remote_conid_valid = listen->remote_conid_valid;
	cdt->listen_index = listen->listen_index;
	cdt->sysap = sd->ops;
	scs_copy(cdt->local_name, sd->name, VMS_SCS_PROCNAME_LEN);
	scs_copy(cdt->remote_name, listen->remote_name, VMS_SCS_PROCNAME_LEN);
	if (rx->conndata != (const uint8_t *)0)
		scs_copy(cdt->conndata, rx->conndata, VMS_SCS_PROCNAME_LEN);
	sb_queue_cdt(f, sb, cdt);

	credit_extend(cdt, sd->initial_credits);
	/* The initiator's own grant, carried over from the op-0 the listening
	 * CDT took it off (see h_rx_con_req). This is the ONLY source of this
	 * end's Send Credit, and it is a read of what the peer actually put on
	 * the wire -- never a mirror of what we granted it. */
	credit_receive_grant(cdt, listen->credit_send);
	rc = ctrl_send_grant(f, cdt, (uint16_t)SCS_MTYPE_ACCP_REQ,
			     cdt->credit_receive);
	if (rc != SCS_OK) {
		sb_unqueue_cdt(f, cdt);
		cdt_release(f, cdt);
		return rc;
	}

	cdt_set_state(cdt, VMS_SCS_CDT_ACCEPT_SENT);
	scs_arm(f, SCS_TIMER_CONNECT, cdt->timer_key, f->cfg.connect_timeout_ms);
	rx->out_conid = cdt->local_conid;

	/* ch. 2: the listening CDT returns to LISTEN once the request has been
	 * answered, and is then ready for the next one. */
	listen_reset(f, listen);
	return SCS_OK;
}

/* [CONNECT_RCVD] LOCAL_REJECT -- ch. 2: "The SCS REJECT service does not
 * involve a CDT at all, but merely the sending of a message rejecting a
 * connect request." No CDT is allocated and none is awaited: an op-5
 * REJECT_RSP that matches nothing is dropped and counted. */
static int h_local_reject(struct scs_fsm *f, struct scs_cdt *listen,
			  struct scs_rx *rx)
{
	struct vms_scs_ctrl_frame c;
	int rc;

	(void)rx;
	rc = ctrl_prepare(f, listen, (uint16_t)SCS_MTYPE_REJ_REQ, &c);
	if (rc != SCS_OK)
		return rc;
	c.conid_local = 0u;   /* no CDT exists on this end -- and that is true */
	c.credit = 0u;
	rc = ctrl_emit(f, listen, &c);
	if (rc == SCS_OK)
		f->connects_rejected++;
	listen_reset(f, listen);
	return rc;
}

/* [ACCEPT_SENT] RX op 3 -- the initiator's CONFIRM. The connection is open. */
static int h_rx_confirm(struct scs_fsm *f, struct scs_cdt *cdt,
			struct scs_rx *rx)
{
	(void)rx;
	cdt->confirmed = 1u;
	cdt_set_state(cdt, VMS_SCS_CDT_OPEN);
	scs_cancel(f, SCS_TIMER_CONNECT, cdt->timer_key);
	if (cdt->sysap != (const struct scs_sysap_ops *)0 &&
	    cdt->sysap->opened != (void (*)(void *, vms_conid_t))0)
		cdt->sysap->opened(cdt->sysap->ctx, cdt->local_conid);
	sendwait_drain(f, cdt);
	return SCS_OK;
}

/* RX op 4 -- the peer refused our connect. Answer op 5 and close. */
static int h_rx_reject(struct scs_fsm *f, struct scs_cdt *cdt,
		       struct scs_rx *rx)
{
	(void)rx;
	(void)ctrl_send_plain(f, cdt, (uint16_t)SCS_MTYPE_REJ_RSP);
	cdt_close(f, cdt, SCS_CLOSE_REJECTED);
	return SCS_OK;
}

/* The connect timer expired with no answer: no retry, no silent stall. */
static int h_timer_connect(struct scs_fsm *f, struct scs_cdt *cdt,
			   struct scs_rx *rx)
{
	(void)rx;
	if (cdt->is_listening) {
		/* The SYSAP never answered the request it was handed. Refuse
		 * the peer honestly and take the listener back. */
		return h_local_reject(f, cdt, rx);
	}
	scs_log(f, "%SCS-W-CONNTIMO, connect verb unanswered, connection closed");
	cdt_close(f, cdt, SCS_CLOSE_TIMEOUT);
	return SCS_OK;
}

/* ==========================================================================
 * L. Handlers -- the DATA half: MTYPE dispatch and the credit account
 * ========================================================================== */

/*
 * WHAT THE THREE ACCOUNT TYPES DO TO THE LEDGER -- and the one place OVMX had
 * to CHOOSE a reading, recorded here rather than buried.
 *
 * GROUNDED, and applied to all three: the credit a type 8, 9 or 10 CARRIES is
 * added to this end's Send Credit (p. 2-43; spec SS4(h)(1c) measures all three
 * carrying real, varying counts, and SS4(h)(1g)'s SCSFLOWCUSH dose-response
 * shows the total returned across the type-10 and type-8 carriers is an exact
 * count of messages received).
 *
 * GROUNDED, and applied to type 10 ALONE: an application message consumes one
 * of the receive buffers this end extended (p. 2-43's debit/credit system,
 * 676/676 in the ledger replay).
 *
 * THE CHOICE -- types 8 and 9 spend no Send Credit and consume no receive
 * buffer. SS4(h)(1c) counted 8/9 as consuming a buffer when it replayed the
 * ledger, but in that corpus the 8/9 pair occurs exactly once per dialogue,
 * immediately before the teardown, so whether their buffers were ever returned
 * is unobservable there -- the connection ends. Against that, p. 2-44 states
 * what the special credit message is FOR: it exists precisely because there is
 * no message to piggyback on. Making it spend the Send Credit it is sent to
 * replenish is circular, and with the grounded credit value of 1 it returns
 * exactly nothing (+1 carried, -1 spent) -- the mechanism would be a no-op.
 * SS4(h)(1f) itself declines to name types 8 and 9, and records that p. 2-44
 * "describes no reply at all" to the special credit message, so the reply's
 * accounting is not grounded either. OVMX therefore keeps both outside the
 * debit half of the account and inside the carry half. The two ledger
 * invariants hold exactly under this reading:
 *     receive + held + pending == grant           (locally, always)
 *     our Send Credit == the peer's Receive Credit (at quiescence)
 * and the R1 property test asserts both after every step.
 */
static void credit_carry_inbound(struct scs_cdt *cdt,
				 const struct vms_scs_hdr *hdr)
{
	credit_receive_grant(cdt, hdr->credit);
}

/* Emit the special credit message, op 8 (spec SS4(h)(1g): IDENTIFIED as
 * p. 2-44's special credit message by the SCSFLOWCUSH dose-response). Its
 * credit field is the Pending Receive Credit count -- the ledger read, taken
 * and reset in one step, exactly as an ordinary message's piggyback is. */
static int credit_msg_send(struct scs_fsm *f, struct scs_cdt *cdt)
{
	uint16_t n = credit_pending_peek(cdt);
	int rc = ctrl_send_grant(f, cdt, (uint16_t)SCS_MTYPE_CR_REQ, n);

	if (rc != SCS_OK)
		return rc;
	credit_pending_commit(cdt, n);
	cdt->credit_msg_sent = 1u;
	f->credit_msgs_sent++;
	return SCS_OK;
}

/* [OPEN] RX op 8 -- the peer's special credit message. Answered with op 9
 * "with the handle pair swapped", 131 of 131, worst case 3.1 ms (SS4(h)(1f)):
 * machine-speed, no timer, and -- see the note above -- not gated on credit,
 * because a response that could be blocked by the account is a response the
 * corpus would not show 131 times out of 131. Our op 9 carries OUR pending
 * count, which is a ledger read like every other credit byte here. */
static int h_rx_credit_req(struct scs_fsm *f, struct scs_cdt *cdt,
			   struct scs_rx *rx)
{
	if (rx->hdr == (const struct vms_scs_hdr *)0)
		return SCS_ERR_INVAL;
	credit_carry_inbound(cdt, rx->hdr);
	{
		uint16_t n = credit_pending_peek(cdt);

		if (ctrl_send_grant(f, cdt, (uint16_t)SCS_MTYPE_CR_RSP, n) ==
		    SCS_OK)
			credit_pending_commit(cdt, n);
	}
	sendwait_drain(f, cdt);
	return SCS_OK;
}

/* RX op 9 -- the answer to OUR op 8. If a disconnect was waiting on it, this
 * is what releases the DISCONNECT-REQUEST (spec SS4(h)(1f): "frames between
 * the 8 and the DISCONNECT_REQ: exactly the 9", 131 of 131). */
static int h_rx_credit_rsp(struct scs_fsm *f, struct scs_cdt *cdt,
			   struct scs_rx *rx)
{
	if (rx->hdr == (const struct vms_scs_hdr *)0)
		return SCS_ERR_INVAL;
	credit_carry_inbound(cdt, rx->hdr);
	if (cdt->credit_msg_sent) {
		cdt->credit_msg_sent = 0u;
		cdt->credit_msg_done = 1u;
		f->credit_msgs_answered++;
	}
	sendwait_drain(f, cdt);
	if (cdt->disc_pending && !cdt->disc_sent)
		return disc_emit_req(f, cdt);
	return SCS_OK;
}

/* RX op 10 -- an application message. This is the CDL delivery path, live: the
 * Con.ID found the CDT, the CDT names the SYSAP, the SYSAP's input routine is
 * called with its own 132 bytes and nothing below them. */
static int h_rx_appl_msg(struct scs_fsm *f, struct scs_cdt *cdt,
			 struct scs_rx *rx)
{
	int taken = -1;

	if (rx->hdr == (const struct vms_scs_hdr *)0)
		return SCS_ERR_INVAL;
	credit_carry_inbound(cdt, rx->hdr);
	credit_consume_receive(f, cdt);   /* p. 2-43: type 10 takes a buffer */
	cdt->msgs_received++;

	if (cdt->sysap != (const struct scs_sysap_ops *)0 &&
	    cdt->sysap->message != (int (*)(void *, vms_conid_t,
					    const uint8_t *, uint32_t))0 &&
	    rx->body != (const uint8_t *)0)
		taken = cdt->sysap->message(cdt->sysap->ctx, cdt->local_conid,
					    rx->body, rx->body_len);
	if (taken != 0)
		f->rx_undelivered++;

	sendwait_drain(f, cdt);
	/* p. 2-44: a message arriving is exactly when the receiving end tests
	 * whether its own Receive Credit has fallen dangerously low. */
	if (cdt->credit_pending > 0u && credit_dangerously_low(f, cdt))
		(void)credit_msg_send(f, cdt);
	return SCS_OK;
}

/* [OPEN] LOCAL_SEND. */
static int h_local_send(struct scs_fsm *f, struct scs_cdt *cdt,
			struct scs_rx *rx)
{
	if (rx->body == (const uint8_t *)0)
		return SCS_ERR_INVAL;
	if (cdt->sw_head != SCS_NIL) {
		/* Something is already waiting: FIFO order is the p. 2-46 rule
		 * ("queue priority is based on time spent in the queue"), so a
		 * new send goes behind it even if a credit is free. */
		return sendwait_push(f, cdt, rx->body, rx->body_len);
	}
	if (cdt->credit_send == 0u)
		return sendwait_push(f, cdt, rx->body, rx->body_len);
	return msg_transmit(f, cdt, rx->body, rx->body_len);
}

/* ==========================================================================
 * M. Handlers -- the TEARDOWN half of the ladder
 * ========================================================================== */

/*
 * The DISCONNECT-REQUEST itself. GROUNDED to carry credit 0 even when the
 * ledger says one is owed -- spec SS4(h)(1f), 131 of 131: "the last credit on
 * a connection is never returned, because the connection is being destroyed."
 * So this is ctrl_send_plain, and the residual pending count is deliberately
 * left on the floor rather than piggybacked.
 */
static int disc_emit_req(struct scs_fsm *f, struct scs_cdt *cdt)
{
	int rc = ctrl_send_plain(f, cdt, (uint16_t)SCS_MTYPE_DISC_REQ);

	if (rc != SCS_OK)
		return rc;
	cdt->disc_sent = 1u;
	disc_advance(f, cdt);
	scs_arm(f, SCS_TIMER_DISCONNECT, cdt->timer_key,
		f->cfg.disconnect_timeout_ms);
	return SCS_OK;
}

/*
 * The teardown state rule. Spec SS4(m): the disconnect is BIDIRECTIONAL --
 * "each side sends its own op 6 and answers the peer's with op 7" -- so there
 * are two half-exchanges and the CDT's state is how far along the pair is:
 *
 *   both matched          -> CLOSED
 *   ours matched          -> DISC_MATCH  (waiting for the peer's own op 6)
 *   theirs matched        -> DISC_RCVD   (we answered; our op 6 outstanding)
 *   ours merely sent      -> DISC_SENT
 *
 * That is a total order on progress, so every one of the frozen ladder's
 * teardown states is reachable and distinguishable, which is what the R1
 * transition test walks.
 */
static void disc_advance(struct scs_fsm *f, struct scs_cdt *cdt)
{
	if (cdt->disc_matched && cdt->disc_peer_matched) {
		cdt_close(f, cdt, cdt->disc_pending ? SCS_CLOSE_LOCAL
						    : SCS_CLOSE_REMOTE);
		return;
	}
	if (cdt->disc_matched)
		cdt_set_state(cdt, VMS_SCS_CDT_DISC_MATCH);
	else if (cdt->disc_peer_matched)
		cdt_set_state(cdt, VMS_SCS_CDT_DISC_RCVD);
	else if (cdt->disc_sent)
		cdt_set_state(cdt, VMS_SCS_CDT_DISC_SENT);
}

/*
 * [OPEN] LOCAL_DISCONNECT -- THE 8-BEFORE-DISCONNECT RULE.
 *
 * Spec SS4(h)(1f), each figure N-of-N over the reference library: the 8->9
 * exchange is biconditional with teardown (131 dialogues carry a type 8; 131
 * of those disconnect; 0 disconnect without one), the type-8 sender is always
 * the connection's opener AND the first DISCONNECT_REQ sender, and the only
 * frame between the 8 and the op 6 is the 9. So a locally-initiated teardown
 * sends op 8 first and lets the op-9 arrival release the op 6; the disconnect
 * timer is the backstop if the 9 never comes.
 *
 * A teardown this end did NOT initiate emits no op 8 -- also grounded, because
 * the type-8 sender is the rank-0 DISCONNECT_REQ sender in all 131.
 */
static int h_local_disconnect(struct scs_fsm *f, struct scs_cdt *cdt,
			      struct scs_rx *rx)
{
	(void)rx;
	cdt->disc_pending = 1u;
	scs_arm(f, SCS_TIMER_DISCONNECT, cdt->timer_key,
		f->cfg.disconnect_timeout_ms);

	if (credit_msg_send(f, cdt) == SCS_OK)
		return SCS_OK;      /* the op 9 will release the op 6 */

	/* The op 8 could not be BUILT or TRANSMITTED (there is no credit gate
	 * on it -- see the accounting note above). The connection must still
	 * come down: the disconnect timer emits the op 6 and counts the
	 * shortfall, so a lost credit message can never wedge a teardown open.
	 */
	return SCS_OK;
}

/* RX op 6 -- the peer's DISCONNECT-REQUEST. Answer op 7, and (SS4(m)) send our
 * own op 6 if we have not already. Both carry credit 0 (SS4(h)(1c): types 6
 * and 7 are outside the account, 100 % of real-VAX frames). */
static int h_rx_disc_req(struct scs_fsm *f, struct scs_cdt *cdt,
			 struct scs_rx *rx)
{
	(void)rx;
	if (ctrl_send_plain(f, cdt, (uint16_t)SCS_MTYPE_DISC_RSP) != SCS_OK)
		return SCS_ERR_TXFAIL;
	cdt->disc_peer_matched = 1u;

	if (!cdt->disc_sent) {
		if (ctrl_send_plain(f, cdt,
				    (uint16_t)SCS_MTYPE_DISC_REQ) == SCS_OK) {
			cdt->disc_sent = 1u;
			scs_arm(f, SCS_TIMER_DISCONNECT, cdt->timer_key,
				f->cfg.disconnect_timeout_ms);
		}
	}
	disc_advance(f, cdt);
	return SCS_OK;
}

/* RX op 7 -- our own DISCONNECT-REQUEST was matched. */
static int h_rx_disc_rsp(struct scs_fsm *f, struct scs_cdt *cdt,
			 struct scs_rx *rx)
{
	(void)rx;
	cdt->disc_matched = 1u;
	disc_advance(f, cdt);
	return SCS_OK;
}

/* The disconnect timer. Two jobs, in this order: get the op 6 out if the 8->9
 * leg never completed, then force the connection closed if the teardown itself
 * stalled. Either way the connection goes away -- a teardown that hangs
 * forever because a credit message was lost is the worse failure. */
static int h_timer_disconnect(struct scs_fsm *f, struct scs_cdt *cdt,
			      struct scs_rx *rx)
{
	(void)rx;
	if (cdt->disc_pending && !cdt->disc_sent) {
		f->disc_without_credit_msg++;
		scs_log(f, "%SCS-W-NOCREDMSG, disconnect proceeding without the "
			   "credit-message exchange");
		if (disc_emit_req(f, cdt) == SCS_OK)
			return SCS_OK;
	}
	scs_log(f, "%SCS-W-DISCTIMO, disconnect unmatched, connection closed");
	cdt_close(f, cdt, SCS_CLOSE_TIMEOUT);
	return SCS_OK;
}

/*
 * VC_DOWN -- THE CONTRACT (design SS3.2.5). One handler, present in every row
 * of the table, because a broken circuit takes a connection down from whatever
 * state it was in: p. 2-31's rule is that if the port cannot satisfy the
 * delivery guarantee the VC is broken "and every connection on it with it".
 *
 * cdt_close() does all four required things: CLOSED with path-lost, the ledger
 * discarded, every Credit Wait entry failed with path-lost, the SYSAP told.
 * NOTHING is put on the wire here and nothing is retried.
 */
static int h_vc_down(struct scs_fsm *f, struct scs_cdt *cdt, struct scs_rx *rx)
{
	const struct scs_sysap_ops *sysap;

	(void)rx;
	if (!cdt->is_listening) {
		cdt_close(f, cdt, SCS_CLOSE_PATHLOST);
		return SCS_OK;
	}

	/*
	 * A LISTENING CDT is not a connection and is not destroyed by a break:
	 * destroying it would silently deregister the SYSAP. What the break
	 * DOES take away is the request it was holding, so the SYSAP is told
	 * the requester is gone -- otherwise it waits forever to decide about a
	 * node that has left, and (ch. 2's one-at-a-time rule) refuses every
	 * later connect as busy.
	 */
	sysap = cdt->sysap;
	if (sysap != (const struct scs_sysap_ops *)0 &&
	    sysap->closed != (void (*)(void *, vms_conid_t, uint32_t))0)
		sysap->closed(sysap->ctx, cdt->local_conid,
			      (uint32_t)SCS_CLOSE_PATHLOST);
	listen_reset(f, cdt);
	return SCS_OK;
}

/* ==========================================================================
 * N. THE TABLE -- [cdt state][event], one cell per grounded edge
 *
 * A NULL cell is not a bug and not a silent drop: scs_dispatch() counts it in
 * f->ignored_events, which is the honest measure of what this node received
 * and had no edge for.
 *
 * TWO COLUMNS ARE DELIBERATELY EMPTY, and both are documented rather than
 * quietly absent:
 *
 *   SCS_EV_VC_UP is an SB-level fact, handled in scs_fsm_vc_up(). A circuit
 *   coming up changes NO connection's state, because every connection on that
 *   circuit was closed when it went down and SCS does not re-open one
 *   (design SS3.2.5). The SYSAP reconnects, or nothing does.
 *
 *   SCS_EV_RX_APPL_DG has no edge because the frozen scs_sysap_ops has no
 *   datagram input routine and spec SS4(h)(1d) REFUTES the candidate wire
 *   discriminator between a message and a datagram. Counting one is honest;
 *   delivering it through the message path would assert a distinction the wire
 *   does not carry.
 * ========================================================================== */

static const scs_handler_t
scs_table[VMS_SCS_CDT_STATE__COUNT][SCS_EV__COUNT] = {

	/* [CLOSED] a free CDL slot, or one a SYSAP has just been given by
	 * scs_fsm_connect and has not yet driven. Only the local CONNECT
	 * starts anything; an inbound frame naming a closed CDT is counted. */
	[VMS_SCS_CDT_CLOSED] = {
		[SCS_EV_LOCAL_CONNECT] = h_local_connect,
	},

	/* [LISTEN] ch. 2's listening CDT: it holds the SYSAP's connect-request
	 * handler and nothing else -- no peer, no ledger, no Con.ID pair. */
	[VMS_SCS_CDT_LISTEN] = {
		[SCS_EV_RX_CON_REQ] = h_rx_con_req,
		[SCS_EV_VC_DOWN]    = h_vc_down,
	},

	/* [CONNECT_SENT] our op 0 is outstanding. SS4(m)'s ordering invariant
	 * lives here: the echo may arrive, then the accept, then we confirm. */
	[VMS_SCS_CDT_CONNECT_SENT] = {
		[SCS_EV_RX_CON_RSP]     = h_rx_echo,     /* op 1 */
		[SCS_EV_RX_ACCP_REQ]    = h_rx_accept,   /* op 2 */
		[SCS_EV_RX_REJ_REQ]     = h_rx_reject,   /* op 4 */
		[SCS_EV_RX_DISC_REQ]    = h_rx_disc_req, /* op 6 */
		[SCS_EV_TIMER_CONNECT]  = h_timer_connect,
		[SCS_EV_VC_DOWN]        = h_vc_down,
	},

	/* [CONNECT_RCVD] the listening CDT while the SYSAP decides. */
	[VMS_SCS_CDT_CONNECT_RCVD] = {
		[SCS_EV_RX_CON_REQ]      = h_rx_con_req_busy,
		[SCS_EV_LOCAL_ACCEPT]    = h_local_accept,
		[SCS_EV_LOCAL_REJECT]    = h_local_reject,
		[SCS_EV_TIMER_CONNECT]   = h_timer_connect,
		[SCS_EV_VC_DOWN]         = h_vc_down,
	},

	/* [ACCEPT_SENT] our op 2 is outstanding; the initiator must confirm.
	 * SS4(m): without that op 3 the connection stays half-open and the
	 * whole membership dialogue on it never starts. */
	[VMS_SCS_CDT_ACCEPT_SENT] = {
		[SCS_EV_RX_ACCP_RSP]    = h_rx_confirm,  /* op 3 */
		[SCS_EV_RX_DISC_REQ]    = h_rx_disc_req,
		[SCS_EV_TIMER_CONNECT]  = h_timer_connect,
		[SCS_EV_VC_DOWN]        = h_vc_down,
	},

	/* [ACCEPT_RCVD] the peer accepted and our CONFIRM could not be
	 * transmitted. A real state, not a transient: the connection is NOT
	 * open until the confirm is on the wire. */
	[VMS_SCS_CDT_ACCEPT_RCVD] = {
		[SCS_EV_RX_REJ_REQ]     = h_rx_reject,
		[SCS_EV_RX_DISC_REQ]    = h_rx_disc_req,
		[SCS_EV_TIMER_CONNECT]  = h_confirm_retry,
		[SCS_EV_VC_DOWN]        = h_vc_down,
	},

	/* [OPEN] the connection carries data. */
	[VMS_SCS_CDT_OPEN] = {
		[SCS_EV_RX_CR_REQ]        = h_rx_credit_req,  /* op 8  */
		[SCS_EV_RX_CR_RSP]        = h_rx_credit_rsp,  /* op 9  */
		[SCS_EV_RX_APPL_MSG]      = h_rx_appl_msg,    /* op 10 */
		[SCS_EV_RX_DISC_REQ]      = h_rx_disc_req,
		[SCS_EV_LOCAL_SEND]       = h_local_send,
		[SCS_EV_LOCAL_DISCONNECT] = h_local_disconnect,
		[SCS_EV_TIMER_DISCONNECT] = h_timer_disconnect,
		[SCS_EV_VC_DOWN]          = h_vc_down,
	},

	/* [DISC_SENT] / [DISC_RCVD] / [DISC_MATCH] -- the teardown. Data still
	 * arrives here: a message the peer sent before its side saw our op 6 is
	 * a real message that already consumed a real buffer, so it is
	 * accounted and delivered rather than dropped. */
	[VMS_SCS_CDT_DISC_SENT] = {
		[SCS_EV_RX_DISC_RSP]      = h_rx_disc_rsp,
		[SCS_EV_RX_DISC_REQ]      = h_rx_disc_req,
		[SCS_EV_RX_CR_RSP]        = h_rx_credit_rsp,
		[SCS_EV_RX_APPL_MSG]      = h_rx_appl_msg,
		[SCS_EV_TIMER_DISCONNECT] = h_timer_disconnect,
		[SCS_EV_VC_DOWN]          = h_vc_down,
	},
	[VMS_SCS_CDT_DISC_RCVD] = {
		[SCS_EV_RX_DISC_RSP]      = h_rx_disc_rsp,
		[SCS_EV_RX_DISC_REQ]      = h_rx_disc_req,
		[SCS_EV_RX_CR_RSP]        = h_rx_credit_rsp,
		[SCS_EV_RX_APPL_MSG]      = h_rx_appl_msg,
		[SCS_EV_TIMER_DISCONNECT] = h_timer_disconnect,
		[SCS_EV_VC_DOWN]          = h_vc_down,
	},
	[VMS_SCS_CDT_DISC_MATCH] = {
		[SCS_EV_RX_DISC_REQ]      = h_rx_disc_req,
		[SCS_EV_RX_DISC_RSP]      = h_rx_disc_rsp,
		[SCS_EV_RX_APPL_MSG]      = h_rx_appl_msg,
		[SCS_EV_TIMER_DISCONNECT] = h_timer_disconnect,
		[SCS_EV_VC_DOWN]          = h_vc_down,
	},
};

static int scs_dispatch(struct scs_fsm *f, struct scs_cdt *cdt,
			enum scs_event ev, struct scs_rx *rx)
{
	scs_handler_t h;
	struct scs_rx local;

	if (f == (struct scs_fsm *)0 || cdt == (struct scs_cdt *)0 ||
	    !cdt->in_use)
		return SCS_ERR_NOCONN;
	if ((unsigned)ev >= (unsigned)SCS_EV__COUNT)
		return SCS_ERR_INVAL;
	if ((unsigned)cdt->state >= (unsigned)VMS_SCS_CDT_STATE__COUNT)
		return SCS_ERR_INVAL;

	if (rx == (struct scs_rx *)0) {
		scs_bzero(&local, (uint32_t)sizeof(local));
		rx = &local;
	}
	h = scs_table[cdt->state][ev];
	if (h == (scs_handler_t)0) {
		f->ignored_events++;
		return SCS_ERR_NOTOPEN;
	}
	return h(f, cdt, rx);
}

/* ==========================================================================
 * O. Lifecycle, binding, configuration
 * ========================================================================== */

static void cfg_defaults(struct scs_fsm_cfg *c)
{
	c->connect_timeout_ms = SCS_CONNECT_TIMEOUT_MS_DEFAULT;
	c->disconnect_timeout_ms = SCS_DISCONNECT_TIMEOUT_MS_DEFAULT;
	c->flowcush = SCS_FLOWCUSH_DEFAULT;
	c->pad0 = 0u;
}

int scs_fsm_init(struct scs_fsm *f, const struct scs_fsm_ops *ops)
{
	if (f == (struct scs_fsm *)0 || ops == (const struct scs_fsm_ops *)0)
		return SCS_ERR_INVAL;
	scs_bzero(f, (uint32_t)sizeof(*f));
	f->ops = ops;
	cfg_defaults(&f->cfg);
	return SCS_OK;
}

int scs_fsm_bind_cdl(struct scs_fsm *f, struct scs_cdt *cdl, uint32_t n)
{
	uint32_t i;

	if (f == (struct scs_fsm *)0 || cdl == (struct scs_cdt *)0 || n == 0u)
		return SCS_ERR_INVAL;
	/* The CDL index rides in 16 bits of the Con.ID (ch. 2), and index 0 is
	 * reserved by the +1 that keeps a minted Con.ID's low half non-zero. */
	if (n > SCS_CONID_SLOT_MASK)
		return SCS_ERR_INVAL;
	for (i = 0; i < n; i++)
		cdt_reset(&cdl[i], 0u);
	f->cdl = cdl;
	f->n_cdl = n;
	return SCS_OK;
}

int scs_fsm_bind_sbs(struct scs_fsm *f, struct scs_sb *sbs, uint32_t n)
{
	uint32_t i;

	if (f == (struct scs_fsm *)0 || sbs == (struct scs_sb *)0 || n == 0u)
		return SCS_ERR_INVAL;
	for (i = 0; i < n; i++) {
		scs_bzero(&sbs[i], (uint32_t)sizeof(sbs[i]));
		sbs[i].cdt_head = SCS_NIL;
	}
	f->sbs = sbs;
	f->n_sbs = n;
	return SCS_OK;
}

int scs_fsm_bind_sendwait(struct scs_fsm *f, struct scs_sendwait *sw,
			  uint32_t n)
{
	uint32_t i;

	if (f == (struct scs_fsm *)0)
		return SCS_ERR_INVAL;
	for (i = 0; i < n; i++)
		scs_bzero(&sw[i], (uint32_t)sizeof(sw[i]));
	f->sw = sw;
	f->n_sw = n;
	return SCS_OK;
}

void scs_fsm_seed_conid(struct scs_fsm *f, uint16_t boot_seed)
{
	if (f == (struct scs_fsm *)0)
		return;
	f->conid.seed = boot_seed;
	f->conid.seeded = 1u;
}

void scs_fsm_set_cfg(struct scs_fsm *f, const struct scs_fsm_cfg *cfg)
{
	if (f == (struct scs_fsm *)0)
		return;
	if (cfg == (const struct scs_fsm_cfg *)0)
		cfg_defaults(&f->cfg);
	else
		f->cfg = *cfg;
}

void scs_fsm_stop(struct scs_fsm *f)
{
	uint32_t i;

	if (f == (struct scs_fsm *)0 || f->cdl == (struct scs_cdt *)0)
		return;
	for (i = 0; i < f->n_cdl; i++) {
		if (f->cdl[i].in_use)
			cdt_close(f, &f->cdl[i], SCS_CLOSE_UNLISTEN);
	}
	for (i = 0; i < SCS_MAX_SYSAPS; i++)
		scs_bzero(&f->sdir[i], (uint32_t)sizeof(f->sdir[i]));
	for (i = 0; f->sbs != (struct scs_sb *)0 && i < f->n_sbs; i++) {
		scs_bzero(&f->sbs[i], (uint32_t)sizeof(f->sbs[i]));
		f->sbs[i].cdt_head = SCS_NIL;
	}
}

/* ==========================================================================
 * P. The SYSAP registry seed (ch. 2's SDIR queue + listening CDTs)
 * ========================================================================== */

/* Return a listening CDT to LISTEN, forgetting the request it was holding
 * (ch. 2: "it remains in that state until a response is received ... at which
 * time the listening CDT is returned to the LISTEN state"). */
static void listen_reset(struct scs_fsm *f, struct scs_cdt *listen)
{
	scs_cancel(f, SCS_TIMER_CONNECT, listen->timer_key);
	sb_unqueue_cdt(f, listen);
	listen->peer_sysid = 0;
	listen->remote_conid = 0u;
	listen->remote_conid_valid = 0u;
	listen->credit_send = 0u;   /* the parked grant belongs to that request */
	scs_bzero(listen->remote_name, VMS_SCS_PROCNAME_LEN);
	scs_bzero(listen->conndata, VMS_SCS_PROCNAME_LEN);
	cdt_set_state(listen, VMS_SCS_CDT_LISTEN);
}

int scs_fsm_listen(struct scs_fsm *f, const uint8_t *name,
		   const struct scs_sysap_ops *ops, uint16_t initial_credits)
{
	struct scs_cdt *listen;
	uint32_t i, free_slot = SCS_MAX_SYSAPS;
	int why = SCS_ERR_NOCDT;

	if (f == (struct scs_fsm *)0 || name == (const uint8_t *)0 ||
	    ops == (const struct scs_sysap_ops *)0)
		return SCS_ERR_INVAL;
	for (i = 0; i < SCS_MAX_SYSAPS; i++) {
		if (f->sdir[i].in_use && scs_name_eq(f->sdir[i].name, name))
			return SCS_ERR_BUSY;
		if (!f->sdir[i].in_use && free_slot == SCS_MAX_SYSAPS)
			free_slot = i;
	}
	if (free_slot == SCS_MAX_SYSAPS)
		return SCS_ERR_NOSYSAP;

	listen = cdt_alloc(f, &why);
	if (listen == (struct scs_cdt *)0)
		return why;

	listen->is_listening = 1u;
	listen->sysap = ops;
	listen->listen_index = free_slot;
	scs_copy(listen->local_name, name, VMS_SCS_PROCNAME_LEN);
	cdt_set_state(listen, VMS_SCS_CDT_LISTEN);

	scs_bzero(&f->sdir[free_slot], (uint32_t)sizeof(f->sdir[free_slot]));
	f->sdir[free_slot].in_use = 1u;
	f->sdir[free_slot].ops = ops;
	f->sdir[free_slot].initial_credits = initial_credits;
	f->sdir[free_slot].listen_cdt = cdt_index(f, listen);
	scs_copy(f->sdir[free_slot].name, name, VMS_SCS_PROCNAME_LEN);
	return SCS_OK;
}

int scs_fsm_unlisten(struct scs_fsm *f, const uint8_t *name)
{
	struct scs_sdir *sd;
	struct scs_cdt *cdt;
	uint32_t i, index;

	if (f == (struct scs_fsm *)0 || name == (const uint8_t *)0)
		return SCS_ERR_INVAL;
	sd = sdir_by_name(f, name);
	if (sd == (struct scs_sdir *)0)
		return SCS_ERR_NOSYSAP;
	index = (uint32_t)(sd - f->sdir);

	/* Open connections on the withdrawn name are disconnected, as
	 * vms_scs.h's scs_sysap_unlisten promises. */
	for (i = 0; f->cdl != (struct scs_cdt *)0 && i < f->n_cdl; i++) {
		cdt = &f->cdl[i];
		if (cdt->in_use && !cdt->is_listening &&
		    cdt->listen_index == index)
			cdt_close(f, cdt, SCS_CLOSE_UNLISTEN);
	}
	cdt = scs_fsm_cdt_at(f, sd->listen_cdt);
	if (cdt != (struct scs_cdt *)0 && cdt->in_use)
		cdt_release(f, cdt);
	scs_bzero(sd, (uint32_t)sizeof(*sd));
	return SCS_OK;
}

/*
 * THE READ THE SCS DIRECTORY SERVICE ANSWERS FROM (p. 2-50: "answers 'Yes' or
 * 'No' when asked if a particular SYSAP name is present in its list of
 * listening SYSAPs"). It walks the SAME SDIR queue an inbound CONNECT_REQ is
 * routed through, so a HIT means a connect to that name would be delivered
 * right now -- which is the entire point of the service. There is deliberately
 * no cache and no second table: an answer that could outlive the registration
 * it describes would be a fabricated one (INV-6).
 */
int scs_fsm_sysap_lookup(const struct scs_fsm *f, const uint8_t *name,
			 struct scs_sysap_info *out)
{
	const struct scs_sdir *sd;
	const struct scs_cdt *listen;
	uint32_t i;

	if (f == (const struct scs_fsm *)0 || name == (const uint8_t *)0)
		return SCS_ERR_INVAL;

	sd = (const struct scs_sdir *)0;
	for (i = 0; i < SCS_MAX_SYSAPS; i++) {
		if (f->sdir[i].in_use && scs_name_eq(f->sdir[i].name, name)) {
			sd = &f->sdir[i];
			break;
		}
	}
	if (sd == (const struct scs_sdir *)0)
		return SCS_ERR_NOSYSAP;
	if (out == (struct scs_sysap_info *)0)
		return SCS_OK;

	scs_bzero(out, (uint32_t)sizeof(*out));
	scs_copy(out->name, sd->name, VMS_SCS_PROCNAME_LEN);
	out->initial_credits = sd->initial_credits;
	out->dir_data_valid = sd->dir_data_valid;
	if (sd->dir_data_valid)
		scs_copy(out->dir_data, sd->dir_data, VMS_SCS_PROCNAME_LEN);

	/* The listening CDT's Con.ID -- p. 2-48's own contents of an SDIR. It
	 * is reported only if that CDT is really there; a registration whose
	 * listener has gone reports 0, not a plausible handle. */
	listen = (const struct scs_cdt *)0;
	if (f->cdl != (struct scs_cdt *)0 && sd->listen_cdt < f->n_cdl)
		listen = &f->cdl[sd->listen_cdt];
	if (listen != (const struct scs_cdt *)0 && listen->in_use)
		out->listen_conid = listen->local_conid;
	return SCS_OK;
}

/*
 * The ops table `name` was registered with (FC-P2.4). Same ONE SDIR queue as
 * scs_fsm_sysap_lookup above, and for the same reason: vms_scs.h's frozen
 * CONNECT service names the local SYSAP by NAME, so the glue must be able to
 * reach that SYSAP's OWN callbacks without keeping a second name table beside
 * the registry (integration note E20). NULL for a name nobody registered --
 * never a plausible-looking neighbour's table.
 *
 * It is a separate accessor rather than a field of struct scs_sysap_info
 * because that struct is a READBACK VIEW (a copy of registry state a
 * diagnostic may print), and a live function-pointer table is not something a
 * view should carry.
 */
const struct scs_sysap_ops *scs_fsm_sysap_ops(const struct scs_fsm *f,
					      const uint8_t *name)
{
	uint32_t i;

	if (f == (const struct scs_fsm *)0 || name == (const uint8_t *)0)
		return (const struct scs_sysap_ops *)0;
	for (i = 0; i < SCS_MAX_SYSAPS; i++) {
		if (f->sdir[i].in_use && scs_name_eq(f->sdir[i].name, name))
			return f->sdir[i].ops;
	}
	return (const struct scs_sysap_ops *)0;
}

int scs_fsm_sysap_set_dir_data(struct scs_fsm *f, const uint8_t *name,
			       const uint8_t *data)
{
	struct scs_sdir *sd;

	if (f == (struct scs_fsm *)0 || name == (const uint8_t *)0)
		return SCS_ERR_INVAL;
	sd = sdir_by_name(f, name);
	if (sd == (struct scs_sdir *)0)
		return SCS_ERR_NOSYSAP;

	if (data == (const uint8_t *)0) {
		sd->dir_data_valid = 0u;
		scs_bzero(sd->dir_data, VMS_SCS_PROCNAME_LEN);
		return SCS_OK;
	}
	scs_copy(sd->dir_data, data, VMS_SCS_PROCNAME_LEN);
	sd->dir_data_valid = 1u;
	return SCS_OK;
}

/* ==========================================================================
 * Q. Connection and data services
 * ========================================================================== */

int scs_fsm_connect(struct scs_fsm *f, const struct scs_connect_args *a,
		    vms_conid_t *out_conid)
{
	struct scs_cdt *cdt;
	struct scs_sb *sb;
	int why = SCS_ERR_NOCDT;
	int rc;

	if (f == (struct scs_fsm *)0 || a == (const struct scs_connect_args *)0 ||
	    a->local_name == (const uint8_t *)0 ||
	    a->remote_name == (const uint8_t *)0)
		return SCS_ERR_INVAL;

	sb = sb_find_or_alloc(f, a->dst);
	if (sb == (struct scs_sb *)0)
		return SCS_ERR_NOSB;

	cdt = cdt_alloc(f, &why);
	if (cdt == (struct scs_cdt *)0)
		return why;

	cdt->peer_sysid = a->dst;
	cdt->sysap = a->sysap;
	cdt->credit_grant = a->initial_credits;
	scs_copy(cdt->local_name, a->local_name, VMS_SCS_PROCNAME_LEN);
	scs_copy(cdt->remote_name, a->remote_name, VMS_SCS_PROCNAME_LEN);
	if (a->conndata != (const uint8_t *)0)
		scs_copy(cdt->conndata, a->conndata, VMS_SCS_PROCNAME_LEN);
	sb_queue_cdt(f, sb, cdt);

	rc = scs_dispatch(f, cdt, SCS_EV_LOCAL_CONNECT, (struct scs_rx *)0);
	if (rc != SCS_OK) {
		/* Nothing went out: release the CDT rather than leave a row
		 * SDA would print for a connection that was never attempted. */
		sb_unqueue_cdt(f, cdt);
		cdt_release(f, cdt);
		return rc;
	}
	if (out_conid != (vms_conid_t *)0)
		*out_conid = cdt->local_conid;
	return SCS_OK;
}

int scs_fsm_accept(struct scs_fsm *f, vms_conid_t listen_conid,
		   const uint8_t *conndata, vms_conid_t *out_conid)
{
	struct scs_cdt *listen;
	struct scs_rx rx;
	int rc;

	if (f == (struct scs_fsm *)0)
		return SCS_ERR_INVAL;
	listen = scs_fsm_cdt_by_conid(f, listen_conid);
	if (listen == (struct scs_cdt *)0)
		return SCS_ERR_NOCONN;

	scs_bzero(&rx, (uint32_t)sizeof(rx));
	rx.conndata = conndata;
	rc = scs_dispatch(f, listen, SCS_EV_LOCAL_ACCEPT, &rx);
	if (rc == SCS_OK && out_conid != (vms_conid_t *)0)
		*out_conid = rx.out_conid;
	return rc;
}

int scs_fsm_reject(struct scs_fsm *f, vms_conid_t listen_conid)
{
	struct scs_cdt *listen;

	if (f == (struct scs_fsm *)0)
		return SCS_ERR_INVAL;
	listen = scs_fsm_cdt_by_conid(f, listen_conid);
	if (listen == (struct scs_cdt *)0)
		return SCS_ERR_NOCONN;
	return scs_dispatch(f, listen, SCS_EV_LOCAL_REJECT, (struct scs_rx *)0);
}

int scs_fsm_disconnect(struct scs_fsm *f, vms_conid_t local_conid)
{
	struct scs_cdt *cdt;

	if (f == (struct scs_fsm *)0)
		return SCS_ERR_INVAL;
	cdt = scs_fsm_cdt_by_conid(f, local_conid);
	if (cdt == (struct scs_cdt *)0)
		return SCS_ERR_NOCONN;
	return scs_dispatch(f, cdt, SCS_EV_LOCAL_DISCONNECT,
			    (struct scs_rx *)0);
}

int scs_fsm_send_msg(struct scs_fsm *f, vms_conid_t local_conid,
		     const uint8_t *body, uint32_t len)
{
	struct scs_cdt *cdt;
	struct scs_sb *sb;
	struct scs_rx rx;

	if (f == (struct scs_fsm *)0 || body == (const uint8_t *)0)
		return SCS_ERR_INVAL;
	/* The length IS the wire class (vms_scs_fsm.h SS1): the 190-content
	 * SYSAP class, the 94-content directory/MSCP-command class, or -- since
	 * FC-P6.3 -- any shorter body carried in its own SCA length ("THE THIRD
	 * APPLICATION-MESSAGE SHAPE"). A body LONGER than the 190-content class
	 * holds is still refused: nothing gets truncated onto the wire. */
	if (len == 0u || len > SCS_SYSAP_BODY_LEN)
		return SCS_ERR_INVAL;
	cdt = scs_fsm_cdt_by_conid(f, local_conid);
	if (cdt == (struct scs_cdt *)0)
		return SCS_ERR_NOCONN;
	/*
	 * Design SS3.2.5: a send on a connection whose circuit is gone fails
	 * PATH-LOST, and says so -- it is not queued, not retried, and not
	 * reported as a generic "no such connection".
	 */
	sb = (cdt->sb_index == SCS_NIL) ? (struct scs_sb *)0
					: &f->sbs[cdt->sb_index];
	if (sb != (struct scs_sb *)0 && !sb->vc_up)
		return SCS_ERR_PATHLOST;
	if (cdt->state != (uint8_t)VMS_SCS_CDT_OPEN)
		return SCS_ERR_NOTOPEN;

	scs_bzero(&rx, (uint32_t)sizeof(rx));
	rx.body = body;
	rx.body_len = len;
	return scs_dispatch(f, cdt, SCS_EV_LOCAL_SEND, &rx);
}

int scs_fsm_return_credit(struct scs_fsm *f, vms_conid_t local_conid,
			  uint16_t n)
{
	struct scs_cdt *cdt;

	if (f == (struct scs_fsm *)0)
		return SCS_ERR_INVAL;
	cdt = scs_fsm_cdt_by_conid(f, local_conid);
	if (cdt == (struct scs_cdt *)0)
		return SCS_ERR_NOCONN;
	if (n > cdt->credit_held)
		return SCS_ERR_INVAL;   /* returning more than was held would
					 * MANUFACTURE credit out of nothing */
	cdt->credit_held = (uint16_t)(cdt->credit_held - n);
	cdt->credit_pending = (uint16_t)(cdt->credit_pending + n);

	/* p. 2-44: with the Receive Credit dangerously low the pending count
	 * goes out at once instead of waiting for a message to ride on. */
	if (cdt->state == (uint8_t)VMS_SCS_CDT_OPEN &&
	    cdt->credit_pending > 0u && credit_dangerously_low(f, cdt))
		(void)credit_msg_send(f, cdt);
	return SCS_OK;
}

/* ==========================================================================
 * R. The receive path -- MTYPE dispatch through the CDL
 * ========================================================================== */

/* MTYPE -> event. The two vocabularies are deliberately parallel (vms_scs.h
 * SS1 and SS3 are both the $SCSDEF order), so this is a bounds check plus a
 * cast rather than a switch that could drift out of step with the enum. */
static int mtype_to_event(uint16_t mtype, enum scs_event *out)
{
	if (mtype > (uint16_t)SCS_MTYPE_APPL_DG)
		return 0;
	*out = (enum scs_event)mtype;   /* SCS_EV_RX_* 0..11 == the MTYPEs */
	return 1;
}

/* Do the connect classes' richer parse only when the verb needs the name pair
 * or the SS4(N) connect data. Everything else runs on the uniform envelope. */
static int mtype_wants_ctrl_parse(uint16_t mtype)
{
	return mtype == (uint16_t)SCS_MTYPE_CON_REQ ||
	       mtype == (uint16_t)SCS_MTYPE_ACCP_REQ;
}

/*
 * An op-0 CONNECT_REQ names a SYSAP, not a Con.ID: it carries destination
 * Con.ID 0 because nothing is bound yet (spec SS4(m)). It is routed by NAME
 * through the SDIR queue to that SYSAP's listening CDT (ch. 2) -- and if no
 * SYSAP is listening, refused, never silently dropped.
 */
static void rx_route_connect(struct scs_fsm *f, struct scs_rx *rx)
{
	struct scs_sdir *sd;
	struct scs_cdt *listen;

	if (rx->ctrl == (const struct vms_scs_ctrl_frame *)0) {
		f->rx_parse_failed++;
		return;
	}
	sd = sdir_by_name(f, rx->ctrl->name1);
	if (sd == (struct scs_sdir *)0) {
		/* ch. 2: SCS must answer "no such SYSAP" rather than ignore
		 * the request. The wire's only grounded refusal is
		 * REJECT_REQUEST; the discriminating byte between "no such
		 * SYSAP" and "the SYSAP said no" is not isolated in any
		 * capture, so none is invented -- the counter carries the
		 * distinction instead. */
		f->connect_no_sysap++;
		f->rx_no_cdt++;
		return;
	}
	listen = scs_fsm_cdt_at(f, sd->listen_cdt);
	if (listen == (struct scs_cdt *)0 || !listen->in_use) {
		f->rx_no_cdt++;
		return;
	}
	(void)scs_dispatch(f, listen, SCS_EV_RX_CON_REQ, rx);
}

void scs_fsm_rx_message(struct scs_fsm *f, vms_scs_sysid_t from,
			vms_conid_t dst_conid, const uint8_t *frame,
			uint32_t len)
{
	struct vms_scs_hdr hdr;
	struct vms_scs_ctrl_frame ctrl;
	struct vms_frame_info fi;
	struct scs_rx rx;
	struct scs_cdt *cdt;
	enum scs_event ev;

	if (f == (struct scs_fsm *)0 || frame == (const uint8_t *)0)
		return;
	f->rx_frames++;

	if (vms_scs_hdr_parse_frame(frame, len, &hdr) != VMS_CODEC_OK) {
		f->rx_parse_failed++;
		return;
	}
	if (!mtype_to_event(hdr.mtype, &ev)) {
		f->rx_parse_failed++;
		return;
	}

	scs_bzero(&rx, (uint32_t)sizeof(rx));
	rx.from = from;
	rx.hdr = &hdr;

	if (mtype_wants_ctrl_parse(hdr.mtype) &&
	    vms_frame_classify(frame, len, &fi) == VMS_CODEC_OK &&
	    vms_scs_ctrl_parse(frame, len, &fi, &ctrl) == VMS_CODEC_OK)
		rx.ctrl = &ctrl;

	if (hdr.mtype == (uint16_t)SCS_MTYPE_APPL_MSG &&
	    vms_scs_msg_body(frame, len, &rx.body, &rx.body_len) !=
	    VMS_CODEC_OK) {
		f->rx_parse_failed++;
		return;
	}

	if (hdr.mtype == (uint16_t)SCS_MTYPE_CON_REQ) {
		rx_route_connect(f, &rx);
		return;
	}

	/* THE CDL PATH. The port already read abs 64; use its value, and fall
	 * back to the header's own copy of the same field when the glue did not
	 * supply one. Either way the CDL verifies the FULL Con.ID. */
	cdt = scs_fsm_cdt_by_conid(f, dst_conid != 0u ? dst_conid
						      : hdr.conid_remote);
	if (cdt == (struct scs_cdt *)0) {
		f->rx_no_cdt++;
		return;
	}
	if (cdt->peer_sysid != from) {
		/* A Con.ID this node minted, arriving from a system it was
		 * never bound to. Counted, never honoured. */
		f->rx_conid_mismatch++;
		return;
	}
	(void)scs_dispatch(f, cdt, ev, &rx);
}

void scs_fsm_rx_datagram(struct scs_fsm *f, vms_scs_sysid_t from,
			 const uint8_t *frame, uint32_t len)
{
	(void)from;
	(void)frame;
	(void)len;
	if (f == (struct scs_fsm *)0)
		return;
	/* See the table's own note: the frozen scs_sysap_ops has no datagram
	 * input routine, and SS4(h)(1d) refutes the candidate message-versus-
	 * datagram wire discriminator. Counted honestly rather than delivered
	 * through the message path. */
	f->rx_frames++;
	f->rx_undelivered++;
}

/* ==========================================================================
 * S. Circuit lifecycle and timers
 * ========================================================================== */

void scs_fsm_vc_up(struct scs_fsm *f, vms_scs_sysid_t peer)
{
	struct scs_sb *sb;

	if (f == (struct scs_fsm *)0)
		return;
	sb = sb_find_or_alloc(f, peer);
	if (sb == (struct scs_sb *)0)
		return;
	sb->vc_up = 1u;
	sb->vc_ups++;
	/* No connection is opened here. SCS does not connect on its own; the
	 * SYSAP does (design SS3.2.5). */
}

void scs_fsm_vc_down(struct scs_fsm *f, vms_scs_sysid_t peer, uint32_t reason)
{
	struct scs_sb *sb;
	uint32_t cur;

	(void)reason;
	if (f == (struct scs_fsm *)0)
		return;
	sb = scs_fsm_sb_by_sysid(f, peer);
	if (sb == (struct scs_sb *)0)
		return;
	sb->vc_up = 0u;
	sb->vc_downs++;

	/* ch. 2's Path Block scan, executed. cdt_close() unqueues as it goes,
	 * so the head is re-read every round rather than walked with a stale
	 * `next`. */
	cur = sb->cdt_head;
	while (cur != SCS_NIL && cur < f->n_cdl) {
		struct scs_cdt *cdt = &f->cdl[cur];

		sb->cdts_pathlost++;
		(void)scs_dispatch(f, cdt, SCS_EV_VC_DOWN, (struct scs_rx *)0);
		if (sb->cdt_head == cur)
			break;              /* it did not leave: stop, loudly */
		cur = sb->cdt_head;
	}
}

void scs_fsm_timer(struct scs_fsm *f, enum scs_timer which, uint32_t key)
{
	struct scs_cdt *cdt;
	enum scs_event ev;

	if (f == (struct scs_fsm *)0)
		return;
	cdt = scs_fsm_cdt_at(f, key);
	if (cdt == (struct scs_cdt *)0 || !cdt->in_use)
		return;
	ev = (which == SCS_TIMER_CONNECT) ? SCS_EV_TIMER_CONNECT
					  : SCS_EV_TIMER_DISCONNECT;
	(void)scs_dispatch(f, cdt, ev, (struct scs_rx *)0);
}

/* ==========================================================================
 * T. Projections and names
 * ========================================================================== */

void scs_fsm_cdt_project(const struct scs_fsm *f, const struct scs_cdt *cdt,
			 struct vms_scs_cdt_view *out)
{
	if (out == (struct vms_scs_cdt_view *)0)
		return;
	scs_bzero(out, (uint32_t)sizeof(*out));
	if (f == (const struct scs_fsm *)0 || cdt == (const struct scs_cdt *)0)
		return;

	out->local_conid = cdt->local_conid;
	out->remote_conid_valid = cdt->remote_conid_valid;
	/* INV-6: a Con.ID never learned stays 0 WITH ITS FLAG CLEAR; the
	 * reader blanks the column rather than printing a fabricated handle. */
	if (cdt->remote_conid_valid)
		out->remote_conid = cdt->remote_conid;
	out->state = cdt->state;
	out->peer_sysid_lo = (uint32_t)(cdt->peer_sysid & 0xffffffffu);
	out->peer_sysid_hi = (uint32_t)((cdt->peer_sysid >> 32) & 0xffffffffu);
	scs_copy(out->local_name, cdt->local_name, VMS_SCS_PROCNAME_LEN);
	scs_copy(out->remote_name, cdt->remote_name, VMS_SCS_PROCNAME_LEN);
	out->credit_send = cdt->credit_send;
	out->credit_receive = cdt->credit_receive;
	out->credit_pending = cdt->credit_pending;
	out->msgs_sent = cdt->msgs_sent;
	out->msgs_received = cdt->msgs_received;
	/* FC-P2.4's MTYPE column: the SAME function the transmit path uses to
	 * choose abs 30 (spec SS4(m)'s phase rule off this CDT's data_phase),
	 * so the diagnostic reports the byte the next frame will really carry
	 * and cannot drift from it. */
	out->msgtype = cdt_msgtype(cdt);
}

void scs_fsm_view_project(const struct scs_fsm *f, struct vms_scs_view *out)
{
	uint32_t i;

	if (out == (struct vms_scs_view *)0)
		return;
	scs_bzero(out, (uint32_t)sizeof(*out));
	if (f == (const struct scs_fsm *)0)
		return;

	for (i = 0; f->sbs != (struct scs_sb *)0 && i < f->n_sbs; i++) {
		if (f->sbs[i].in_use)
			out->n_sbs++;
	}
	for (i = 0; f->cdl != (struct scs_cdt *)0 && i < f->n_cdl; i++) {
		if (f->cdl[i].in_use &&
		    f->cdl[i].state != (uint8_t)VMS_SCS_CDT_CLOSED)
			out->n_cdts++;
	}
	for (i = 0; i < SCS_MAX_SYSAPS; i++) {
		if (f->sdir[i].in_use)
			out->n_sysaps++;
	}
	/* SS4(t)'s two halves, reported as the allocator actually holds them. */
	out->conid_seq = f->conid.minted;
	out->conid_epoch = f->conid.seeded ? (uint32_t)f->conid.seed : 0u;
	out->dir_lookups_served = f->dir_lookups_served;
	out->dir_lookups_sent = f->dir_lookups_sent;
	out->credit_stalls = f->credit_stalls;
}

static const char *const scs_cdt_state_names[VMS_SCS_CDT_STATE__COUNT] = {
	"closed", "listen", "connect sent", "connect rcvd", "accept sent",
	"accept rcvd", "open", "disc sent", "disc rcvd", "disc match"
};

const char *scs_cdt_state_name(enum vms_scs_cdt_state s)
{
	if ((unsigned)s >= (unsigned)VMS_SCS_CDT_STATE__COUNT)
		return "?";
	return scs_cdt_state_names[s];
}

static const char *const scs_close_reason_names[SCS_CLOSE_REASON__COUNT] = {
	"none", "local disconnect", "remote disconnect", "rejected",
	"timeout", "path lost", "unlisten"
};

const char *scs_close_reason_name(enum scs_close_reason r)
{
	if ((unsigned)r >= (unsigned)SCS_CLOSE_REASON__COUNT)
		return "?";
	return scs_close_reason_names[r];
}

static const char *const scs_mtype_names[SCS_MTYPE__COUNT] = {
	"CON_REQ", "CON_RSP", "ACCP_REQ", "ACCP_RSP", "REJ_REQ", "REJ_RSP",
	"DISC_REQ", "DISC_RSP", "CR_REQ", "CR_RSP", "APPL_MSG", "APPL_DG"
};

const char *scs_mtype_name(enum scs_mtype m)
{
	if ((unsigned)m >= (unsigned)SCS_MTYPE__COUNT)
		return "?";
	return scs_mtype_names[m];
}
