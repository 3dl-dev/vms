/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_cl_io_fsm.c - the MSCP disk CLASS DRIVER's pure core (FC-P7.1).
 *
 * The contract, the three structures, the INV-6 field-by-field ledger, the
 * device-naming rule and the two honest gaps (the peer's ALLOCLASS, WRITE's
 * ungrounded data direction) are all in vms_mscp_cl_io_fsm.h. This file is the
 * mechanism.
 *
 * TWO RULES, the same two vms_mscp_srv_fsm.c follows:
 *   - every message is built and parsed ONLY through the FC-P6.2 codec and
 *     FC-P3.4's discovery FSM (design SS3.9 rule 2: not one raw wire offset
 *     lives here, except the buffer descriptor the codec deliberately leaves
 *     opaque -- see the header SS2);
 *   - PURE (design SS3.9 rule 4, gate RULE4): no seam call, no allocation, no
 *     clock but ops->now_ms.
 *
 * INCLUDES: kernel-core headers only
 * (tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_mscp_cl_io_fsm.h"

/* ==========================================================================
 * 0. Local primitives (a pure TU has no libc)
 * ========================================================================== */

static void cl_bzero(void *p, uint32_t n)
{
	uint8_t *b = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		b[i] = 0u;
}

static void cl_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static int cl_name_eq(const char *a, const char *b, uint32_t max)
{
	uint32_t i;

	for (i = 0; i < max; i++) {
		if (a[i] != b[i])
			return 0;
		if (a[i] == '\0')
			return 1;
	}
	return 1;
}

static void cl_log(const struct mscp_cl_fsm *f, const char *msg)
{
	if (f->ops != (const struct mscp_cl_ops *)0 &&
	    f->ops->log != (void (*)(void *, const char *))0)
		f->ops->log(f->ops->ctx, msg);
}

static uint32_t cl_now(const struct mscp_cl_fsm *f)
{
	if (f->ops != (const struct mscp_cl_ops *)0 &&
	    f->ops->now_ms != (uint32_t (*)(void *))0)
		return f->ops->now_ms(f->ops->ctx);
	return 0u;
}

static uint64_t cl_time_now(const struct mscp_cl_fsm *f)
{
	if (f->ops != (const struct mscp_cl_ops *)0 &&
	    f->ops->time_now != (uint64_t (*)(void *))0)
		return f->ops->time_now(f->ops->ctx);
	return 0u;
}

/* ==========================================================================
 * 1. Slot management -- CDDB / UCB / CDRP
 * ========================================================================== */

static struct mscp_cl_cddb *cl_cddb_by_conid(struct mscp_cl_fsm *f,
					     vms_conid_t conid)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		if (f->cddb[i].in_use && f->cddb[i].conid == conid)
			return &f->cddb[i];
	}
	return (struct mscp_cl_cddb *)0;
}

static struct mscp_cl_cddb *cl_cddb_alloc(struct mscp_cl_fsm *f,
					  vms_conid_t conid,
					  vms_scs_sysid_t peer)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_CTLRS; i++) {
		struct mscp_cl_cddb *c = &f->cddb[i];

		if (c->in_use)
			continue;
		cl_bzero(c, (uint32_t)sizeof(*c));
		c->in_use = 1u;
		c->state = (uint8_t)MSCP_CL_ST_AVAILABLE;
		c->conid = conid;
		c->peer = peer;
		vms_mscp_cl_fsm_init(&c->disc);
		return c;
	}
	f->no_cddb_slot++;
	return (struct mscp_cl_cddb *)0;
}

static uint32_t cl_cddb_index(const struct mscp_cl_fsm *f,
			      const struct mscp_cl_cddb *c)
{
	return (uint32_t)(c - &f->cddb[0]);
}

static struct mscp_cl_ucb *cl_ucb_alloc(struct mscp_cl_fsm *f)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_UNITS; i++) {
		if (!f->ucb[i].in_use) {
			cl_bzero(&f->ucb[i], (uint32_t)sizeof(f->ucb[i]));
			f->ucb[i].in_use = 1u;
			return &f->ucb[i];
		}
	}
	f->no_ucb_slot++;
	return (struct mscp_cl_ucb *)0;
}

/* The UCB for (controller, unit number) -- the only correct key: two served
 * nodes may both serve a unit 0 and they are different devices. */
static struct mscp_cl_ucb *cl_ucb_find(struct mscp_cl_fsm *f, uint32_t cddb,
				       uint16_t unit)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_UNITS; i++) {
		struct mscp_cl_ucb *u = &f->ucb[i];

		if (u->in_use && u->cddb == (uint8_t)cddb &&
		    u->unit.unit == unit)
			return u;
	}
	return (struct mscp_cl_ucb *)0;
}

static struct mscp_cl_ucb *cl_ucb_by_devnam(struct mscp_cl_fsm *f,
					    const char *devnam)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_UNITS; i++) {
		struct mscp_cl_ucb *u = &f->ucb[i];

		if (u->in_use && u->registered &&
		    cl_name_eq(u->devnam, devnam, MSCP_CL_NAME_MAX))
			return u;
	}
	return (struct mscp_cl_ucb *)0;
}

static uint32_t cl_ucb_index(const struct mscp_cl_fsm *f,
			     const struct mscp_cl_ucb *u)
{
	return (uint32_t)(u - &f->ucb[0]);
}

static struct mscp_cl_cdrp *cl_cdrp_alloc(struct mscp_cl_fsm *f)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		if (!f->cdrp[i].in_use) {
			cl_bzero(&f->cdrp[i], (uint32_t)sizeof(f->cdrp[i]));
			f->cdrp[i].in_use = 1u;
			return &f->cdrp[i];
		}
	}
	f->no_cdrp_slot++;
	return (struct mscp_cl_cdrp *)0;
}

static struct mscp_cl_cdrp *cl_cdrp_by_ref(struct mscp_cl_fsm *f,
					   uint32_t cmd_ref)
{
	uint32_t i;

	if (cmd_ref == 0u)
		return (struct mscp_cl_cdrp *)0;
	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		if (f->cdrp[i].in_use && f->cdrp[i].cmd_ref == cmd_ref)
			return &f->cdrp[i];
	}
	return (struct mscp_cl_cdrp *)0;
}

static struct mscp_cl_cdrp *cl_cdrp_by_name(struct mscp_cl_fsm *f,
					    uint32_t name)
{
	uint32_t i;

	if (name == 0u)
		return (struct mscp_cl_cdrp *)0;
	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		if (f->cdrp[i].in_use && f->cdrp[i].buf_name == name)
			return &f->cdrp[i];
	}
	return (struct mscp_cl_cdrp *)0;
}

/*
 * Finish a request: release the buffer name (a name that outlived its transfer
 * is a buffer a stale frame could still land in) and tell the caller. `status`
 * is a REAL MSCP status word in every path -- there is no success this file
 * composes for itself.
 */
static void cl_cdrp_complete(struct mscp_cl_fsm *f, struct mscp_cl_cdrp *r,
			     uint16_t status, uint32_t bytes)
{
	uint32_t handle = r->handle;

	if (r->buf_name != 0u && f->ops != (const struct mscp_cl_ops *)0 &&
	    f->ops->buf_release != (void (*)(void *, uint32_t))0)
		f->ops->buf_release(f->ops->ctx, r->buf_name);
	cl_bzero(r, (uint32_t)sizeof(*r));

	if (f->ops != (const struct mscp_cl_ops *)0 &&
	    f->ops->io_done != (void (*)(void *, uint32_t, uint16_t,
					 uint32_t))0)
		f->ops->io_done(f->ops->ctx, handle, status, bytes);
}

/* ==========================================================================
 * 2. Sending -- the splice, and the two P.CRF minters
 * ========================================================================== */

/* Send whatever is in cmdframe's body slice on this controller's connection. */
static int cl_emit(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c)
{
	int rc;

	if (f->ops == (const struct mscp_cl_ops *)0 ||
	    f->ops->send_cmd == (int (*)(void *, vms_conid_t, const uint8_t *,
					 uint32_t))0) {
		f->send_failures++;
		return -1;
	}
	rc = f->ops->send_cmd(f->ops->ctx, c->conid,
			      f->cmdframe + VMS_OFF_SYSAP_BODY,
			      VMS_MSCP_CMD_BODY_LEN);
	if (rc != 0) {
		f->send_failures++;
		return -1;
	}
	c->cmds_tx++;
	return 0;
}

/*
 * P.CRF for the classes FC-P3.4 does not mint. sec 5.1 makes the whole 32-bit
 * value opaque and only requires "unique, non-zero" across the commands
 * outstanding on ONE connection, so this reproduces FC-P3.4's own observed
 * composition -- a per-command-class token in the low word, an incrementing
 * message id in the high word -- rather than inventing a second convention.
 * Never zero: the counter skips 0 on wrap.
 */
static uint32_t cl_next_ref(uint16_t *msgid, uint16_t class_token)
{
	uint16_t id = *msgid;

	if (id == 0u)
		id = 1u;
	*msgid = (uint16_t)(id + 1u);
	return VMS_MSCP_CL_CMD_REF(class_token, id);
}

/* The two class tokens this file adds beside FC-P3.4's SCC (2) and GUS (1).
 * OVMX's own, on the same convention, and distinct from both. */
#define MSCP_CL_ONLINE_CLASS 0x0003u
#define MSCP_CL_XFER_CLASS   0x0004u

/* ==========================================================================
 * 3. The discovery walk (FC-P3.4 owns the protocol; this drives it)
 *
 * The link is ALL ZERO and only the body slice is transmitted: SCS fills abs
 * 56-71 from the real CDT and the port fills abs 0-55 from the real circuit
 * (design SS3.2.4 ruling E1). Not one byte of the zero prefix reaches the
 * wire, which is exactly why passing zeros there is honest -- the same call
 * vms_cnxman_join_fsm.c already documents for the same walk.
 * ========================================================================== */

static void cl_send_scc(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c)
{
	struct vms_mscp_link link;
	vms_codec_status_t st;

	cl_bzero(&link, (uint32_t)sizeof(link));
	/*
	 * P.CNTF: this class driver asks for no controller option -- an
	 * explicit zero, which is honestly what "we have none to declare" is
	 * (sec 6.16's own default is all controller flags clear).
	 * P.HTMO: MSCP_CL_HOST_TIMEOUT_SECS, and it is DECLARED here because it
	 * is ENFORCED in cl_deadline_ms() below -- the header's "what it
	 * declares to the server and what it enforces on itself are one value".
	 */
	st = vms_mscp_cl_fsm_build_scc(&c->disc, &link, 0u,
				       (uint16_t)MSCP_CL_HOST_TIMEOUT_SECS,
				       cl_time_now(f), f->cmdframe,
				       (uint32_t)sizeof(f->cmdframe), NULL);
	if (st != VMS_CODEC_OK) {
		f->codec_failures++;
		return;
	}
	(void)cl_emit(f, c);
}

static void cl_send_gus(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c)
{
	struct vms_mscp_link link;
	vms_codec_status_t st;

	cl_bzero(&link, (uint32_t)sizeof(link));
	st = vms_mscp_cl_fsm_build_gus(&c->disc, &link, f->cmdframe,
				       (uint32_t)sizeof(f->cmdframe), NULL);
	if (st != VMS_CODEC_OK) {
		f->codec_failures++;
		return;
	}
	(void)cl_emit(f, c);
}

/* ==========================================================================
 * 4. The served device's NAME (see the header SS12 and its file header)
 * ========================================================================== */

static uint32_t cl_put_u32(char *out, uint32_t outsz, uint32_t n, uint32_t v)
{
	char digits[10];
	uint32_t d = 0u;

	do {
		digits[d++] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v != 0u && d < (uint32_t)sizeof(digits));

	while (d > 0u && n + 1u < outsz)
		out[n++] = digits[--d];
	return n;
}

static uint32_t cl_put_str(char *out, uint32_t outsz, uint32_t n,
			   const char *s)
{
	uint32_t i;

	for (i = 0; s[i] != '\0' && n + 1u < outsz; i++)
		out[n++] = s[i];
	return n;
}

/* The peer's SCSNODE as the wire carries it: blank/NUL padded. Trailing pad is
 * not part of the name. */
static uint32_t cl_scsnode_trim(const uint8_t *scsnode, uint32_t len)
{
	while (len > 0u && (scsnode[len - 1u] == (uint8_t)' ' ||
			    scsnode[len - 1u] == 0u))
		len--;
	return len;
}

int mscp_cl_unit_name(const uint8_t *scsnode, uint32_t scsnode_len,
		      uint8_t alloclass, int alloclass_valid, uint16_t unit,
		      char *out, uint32_t outsz)
{
	uint32_t n = 0u;
	uint32_t i;

	if (out == (char *)0 || outsz == 0u)
		return -1;
	out[0] = '\0';

	if (alloclass_valid) {
		/* The OpenVMS allocation-class form design P7 names. Reachable
		 * the day a grounded transport for the peer's ALLOCLASS lands;
		 * nothing in this tree calls it with a valid one today. */
		n = cl_put_str(out, outsz, n, "$");
		n = cl_put_u32(out, outsz, n, (uint32_t)alloclass);
		n = cl_put_str(out, outsz, n, "$");
	} else {
		uint32_t len;

		if (scsnode == (const uint8_t *)0)
			return -1;
		len = cl_scsnode_trim(scsnode, scsnode_len);
		if (len == 0u)
			return -1;   /* no node, no name -- and so no device */
		for (i = 0; i < len && n + 1u < outsz; i++)
			out[n++] = (char)scsnode[i];
		n = cl_put_str(out, outsz, n, "$");
	}

	n = cl_put_str(out, outsz, n, "DUA");
	n = cl_put_u32(out, outsz, n, (uint32_t)unit);
	n = cl_put_str(out, outsz, n, ":");
	if (n + 1u >= outsz) {
		out[0] = '\0';
		return -1;   /* would not fit a VMS device name: no device */
	}
	out[n] = '\0';
	return 0;
}

/*
 * Give one discovered unit a UCB and, if it can be honestly named, a device.
 * `u` is the peer's OWN GUS end answer, already parsed by FC-P3.4.
 */
static void cl_unit_discovered(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c,
			       const struct vms_mscp_cl_unit *found)
{
	uint32_t ci = cl_cddb_index(f, c);
	struct mscp_cl_ucb *u = cl_ucb_find(f, ci, found->unit);

	if (u != (struct mscp_cl_ucb *)0) {
		u->unit = *found;   /* a re-walk refreshes the peer's answer */
		return;
	}
	u = cl_ucb_alloc(f);
	if (u == (struct mscp_cl_ucb *)0)
		return;
	u->cddb = (uint8_t)ci;
	u->unit = *found;
	c->units_found++;

	/*
	 * THE NAME, from two values read out of real executive state (the
	 * header's own rule). OVMX holds no ALLOCLASS for the peer, so
	 * alloclass_valid is 0 here and the node-qualified spelling is what
	 * goes out -- counted, so the omission is measurable.
	 */
	f->alloclass_absent++;
	if (mscp_cl_unit_name(c->scsnode, (uint32_t)c->scsnode_len, 0u, 0,
			      found->unit, u->devnam,
			      (uint32_t)sizeof(u->devnam)) != 0) {
		f->units_unnamed++;
		cl_log(f, "vms: MSCP served unit has no nameable device "
			  "(the serving node advertised no SCSNODE)\n");
		return;   /* the UCB is real; NO device is created */
	}

	u->registered = 1u;
	f->units_registered++;
	if (f->ops != (const struct mscp_cl_ops *)0 &&
	    f->ops->unit_ready != (void (*)(void *,
					    const struct mscp_cl_ucb *))0)
		f->ops->unit_ready(f->ops->ctx, u);
}

/* Withdraw every unit a controller served: the path really went away, so the
 * device really goes away too. */
static void cl_units_gone(struct mscp_cl_fsm *f, uint32_t cddb_index)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_UNITS; i++) {
		struct mscp_cl_ucb *u = &f->ucb[i];

		if (!u->in_use || u->cddb != (uint8_t)cddb_index)
			continue;
		if (u->registered && f->ops != (const struct mscp_cl_ops *)0 &&
		    f->ops->unit_gone != (void (*)(void *,
						   const struct mscp_cl_ucb *))0)
			f->ops->unit_gone(f->ops->ctx, u);
		cl_bzero(u, (uint32_t)sizeof(*u));
	}
}

/* ==========================================================================
 * 5. ONLINE (sec 4.3: a unit must be Unit-Online before a transfer)
 * ========================================================================== */

static void cl_send_online(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c,
			   struct mscp_cl_ucb *u)
{
	struct vms_mscp_online_cmd cmd;

	cl_bzero(&cmd, (uint32_t)sizeof(cmd));
	cmd.hdr.cmd_ref = cl_next_ref(&c->online_msgid, MSCP_CL_ONLINE_CLASS);
	cmd.hdr.unit = u->unit.unit;
	/*
	 * P.MOD and the host P.UNFL: this class driver asks for NOTHING. It
	 * does not set MD.SWP (it has no operator surface asking for software
	 * write protection) and it does not assert the 0x8000 bit the corpus
	 * shows a real VMS class driver sending -- docs/design-mscp-direction.md
	 * records that bit as HOST-ORIGINATED with no decoded meaning, and
	 * replaying a captured node's bit is exactly the placeholder INV-6
	 * forbids. Explicit zeros, which is what "we ask for nothing" is.
	 */
	if (vms_mscp_online_cmd_build(&cmd, f->cmdframe,
				      (uint32_t)sizeof(f->cmdframe),
				      NULL) != VMS_CODEC_OK) {
		f->codec_failures++;
		return;
	}
	/* Marked outstanding BEFORE the send, for the reason cl_issue_xfer's
	 * own note gives: the ONLINE end can arrive inside ops->send_cmd, and
	 * it clears this flag. */
	u->online_pending = 1u;
	f->onlines_sent++;
	if (cl_emit(f, c) != 0) {
		u->online_pending = 0u;
		f->onlines_sent--;
	}
}

/* ==========================================================================
 * 6. The transfer commands
 * ========================================================================== */

/*
 * Table A-6 offset 16's twelve bytes, little-endian: { offset, SCS buffer
 * NAME, SCS connection ID }. The byte-for-byte inverse of
 * vms_mscp_srv_fsm.c's srv_read_bufdesc() -- see the header SS2 for why this
 * composition lives at the MSCP layer and not in the FC-P6.2 codec.
 */
static void cl_write_bufdesc(const struct mscp_cl_bufdesc *d, uint8_t *out)
{
	out[0]  = (uint8_t)(d->offset      );
	out[1]  = (uint8_t)(d->offset >>  8);
	out[2]  = (uint8_t)(d->offset >> 16);
	out[3]  = (uint8_t)(d->offset >> 24);
	out[4]  = (uint8_t)(d->name        );
	out[5]  = (uint8_t)(d->name   >>  8);
	out[6]  = (uint8_t)(d->name   >> 16);
	out[7]  = (uint8_t)(d->name   >> 24);
	out[8]  = (uint8_t)(d->conid       );
	out[9]  = (uint8_t)(d->conid  >>  8);
	out[10] = (uint8_t)(d->conid  >> 16);
	out[11] = (uint8_t)(d->conid  >> 24);
}

/* The deadline this request runs under: the CONTROLLER's own declared timeout
 * when it declared one, else this client's own P.HTMO (which it also declared
 * on the wire). See the header's "THE REQUEST DEADLINE" section. */
static uint32_t cl_deadline_ms(struct mscp_cl_fsm *f,
			       const struct mscp_cl_cddb *c)
{
	if (c->ctlr_timeout != 0u) {
		f->deadline_from_ctmo++;
		return (uint32_t)c->ctlr_timeout * 1000u;
	}
	f->deadline_from_own_htmo++;
	return MSCP_CL_HOST_TIMEOUT_SECS * 1000u;
}

/*
 * Name the caller's buffer for the port and put the REAL name in the
 * descriptor. Returns 0, or non-zero -- and then the transfer is refused
 * rather than sent with a zero or invented name (INV-6).
 */
static int cl_name_buffer(struct mscp_cl_fsm *f, struct mscp_cl_cdrp *r,
			  uint8_t access)
{
	uint32_t name = 0u;

	if (f->ops == (const struct mscp_cl_ops *)0 ||
	    f->ops->buf_register == (int (*)(void *, uint8_t *, uint32_t,
					     uint8_t, uint32_t *))0 ||
	    f->ops->buf_register(f->ops->ctx, r->buf, r->byte_count, access,
				 &name) != 0 || name == 0u) {
		f->buf_register_failures++;
		return -1;
	}
	r->buf_name = name;
	return 0;
}

/*
 * Issue the READ or WRITE its CDRP has been holding. Every field on the wire
 * comes from the CDRP (the caller's real request) or from this connection's
 * own real identifiers.
 */
static int cl_issue_xfer(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c,
			 struct mscp_cl_cdrp *r)
{
	struct vms_mscp_xfer_cmd cmd;
	struct mscp_cl_bufdesc desc;
	uint8_t access = r->opcode == VMS_MSCP_OP_READ
			 ? (uint8_t)MSCP_CL_BUF_IN : (uint8_t)MSCP_CL_BUF_OUT;

	if (cl_name_buffer(f, r, access) != 0)
		return -1;

	cl_bzero(&desc, (uint32_t)sizeof(desc));
	desc.offset = 0u;             /* the whole registration is this request */
	desc.name = r->buf_name;      /* the name OUR port really minted        */
	desc.conid = (uint32_t)c->conid; /* OUR local Con.ID for this CDT       */

	cl_bzero(&cmd, (uint32_t)sizeof(cmd));
	cmd.hdr.cmd_ref = cl_next_ref(&c->xfer_msgid, MSCP_CL_XFER_CLASS);
	cmd.hdr.unit = f->ucb[r->ucb].unit.unit;
	cmd.hdr.opcode = r->opcode;
	cmd.byte_count = r->byte_count;
	cmd.lbn = r->lbn;
	cl_write_bufdesc(&desc, cmd.buffer_desc);

	if (vms_mscp_xfer_cmd_build(&cmd, f->cmdframe,
				    (uint32_t)sizeof(f->cmdframe),
				    NULL) != VMS_CODEC_OK) {
		f->codec_failures++;
		return -1;
	}

	/*
	 * THE REQUEST IS FULLY ARMED BEFORE THE COMMAND GOES OUT, and the order
	 * is load-bearing rather than tidy: the answer can come back INSIDE
	 * ops->send_cmd (it does in the R1 rung, where both ends are in one
	 * process, and it can on a real node whenever the completion path runs
	 * on the same fork context). A CDRP whose P.CRF were filled in after
	 * the send would be invisible to its own end message, and the request
	 * would sit until the deadline reaped a transfer that had actually
	 * succeeded. Undone exactly on a send failure, so a refused command
	 * leaves no armed request behind.
	 */
	r->cmd_ref = cmd.hdr.cmd_ref;
	r->waiting_online = 0u;
	r->started_ms = cl_now(f);
	r->deadline_ms = cl_deadline_ms(f, c);
	if (r->opcode == VMS_MSCP_OP_READ)
		f->reads_issued++;
	else
		f->writes_issued++;

	if (cl_emit(f, c) != 0) {
		r->cmd_ref = 0u;
		r->deadline_ms = 0u;
		return -1;
	}
	return 0;
}

/* Every request that was waiting for this unit's ONLINE, now that it really
 * completed. */
static void cl_release_waiters(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c,
			       uint32_t ucb_index)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		struct mscp_cl_cdrp *r = &f->cdrp[i];

		if (!r->in_use || !r->waiting_online ||
		    r->ucb != (uint8_t)ucb_index)
			continue;
		if (cl_issue_xfer(f, c, r) != 0)
			cl_cdrp_complete(f, r,
					 VMS_MSCP_STATUS(VMS_MSCP_ST_CTLR_ERR,
							 VMS_MSCP_SUB_CNT_INCONSISTENT),
					 0u);
	}
}

/* Fail every request waiting on a unit whose ONLINE the server refused, with
 * the server's OWN status word -- never a status this file composes. */
static void cl_fail_waiters(struct mscp_cl_fsm *f, uint32_t ucb_index,
			    uint16_t status)
{
	uint32_t i;

	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		struct mscp_cl_cdrp *r = &f->cdrp[i];

		if (r->in_use && r->waiting_online &&
		    r->ucb == (uint8_t)ucb_index)
			cl_cdrp_complete(f, r, status, 0u);
	}
}

/* ==========================================================================
 * 7. The [state][event] table
 * ========================================================================== */

struct cl_ev {
	vms_conid_t     conid;
	const uint8_t  *frame;      /* the spliced end frame, abs-addressed */
	uint32_t        frame_len;
	struct mscp_cl_cddb *cddb;
};

typedef void (*cl_handler_t)(struct mscp_cl_fsm *f, struct cl_ev *e);

static void h_conn_open(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	struct mscp_cl_cddb *c = e->cddb;

	c->state = (uint8_t)MSCP_CL_ST_DISCOVER;
	cl_log(f, "vms: MSCP class driver: controller online, discovering\n");
	cl_send_scc(f, c);   /* sec 4(n) step 1 */
}

static void h_conn_close(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	struct mscp_cl_cddb *c = e->cddb;
	uint32_t ci = cl_cddb_index(f, c);
	uint32_t i;

	/*
	 * REQUESTS FIRST, then the units. A CDRP names its UCB, so clearing the
	 * UCBs first would leave every request pointing at a zeroed slot whose
	 * `cddb` reads 0 -- and this loop would then abort another controller's
	 * requests. The order is load-bearing, not stylistic.
	 */
	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		struct mscp_cl_cdrp *r = &f->cdrp[i];

		if (!r->in_use || r->ucb >= (uint8_t)MSCP_CL_MAX_UNITS ||
		    !f->ucb[r->ucb].in_use ||
		    f->ucb[r->ucb].cddb != (uint8_t)ci)
			continue;
		/* Table B-1 ST.ABO: the path this request rode is gone. A real
		 * status, not a fabricated success and not a hang. */
		f->reqs_aborted++;
		cl_cdrp_complete(f, r,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_ABORTED, 0u), 0u);
	}
	cl_units_gone(f, ci);
	cl_bzero(c, (uint32_t)sizeof(*c));
}

/*
 * The SET CONTROLLER CHARACTERISTICS end. FC-P3.4's FSM owns the sequencing
 * (it refuses an out-of-order or P.CRF-mismatched answer); this handler reads
 * the two facts the SERVER declared about itself and then drives the next
 * step.
 */
static void h_scc_end(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	struct mscp_cl_cddb *c = e->cddb;
	struct vms_mscp_scc_end end;

	if (vms_mscp_cl_fsm_on_scc_end(&c->disc, e->frame, e->frame_len) !=
	    VMS_CODEC_OK) {
		f->ends_unmatched++;
		return;
	}
	if (vms_mscp_scc_end_parse(e->frame, e->frame_len, &end) ==
	    VMS_CODEC_OK) {
		/* sec 6.16 makes P.CNTI unique, so a ZERO is an absent
		 * identity and is not recorded as one. */
		if (end.ctlr_id != 0u) {
			c->ctlr_id = end.ctlr_id;
			c->ctlr_id_valid = 1u;
		}
		c->ctlr_timeout = end.ctlr_timeout;
	}

	if (c->disc.state == VMS_MSCP_CL_ST_SCC1_DONE)
		cl_send_scc(f, c);          /* sec 4(n): twice */
	else if (c->disc.state == VMS_MSCP_CL_ST_GUS_READY)
		cl_send_gus(f, c);          /* sec 4(n) step 2: the walk */
}

static void h_gus_end(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	struct mscp_cl_cddb *c = e->cddb;
	struct vms_mscp_cl_unit found;
	int terminator = 0;

	cl_bzero(&found, (uint32_t)sizeof(found));
	if (vms_mscp_cl_fsm_on_gus_end(&c->disc, e->frame, e->frame_len, &found,
				       &terminator) != VMS_CODEC_OK) {
		f->ends_unmatched++;
		return;
	}
	if (terminator) {
		/* The Unit-Offline terminator: the walk is complete and this
		 * controller is fully known. */
		c->state = (uint8_t)MSCP_CL_ST_ONLINE;
		return;
	}
	cl_unit_discovered(f, c, &found);
	cl_send_gus(f, c);   /* the next step of the NEXT-UNIT walk */
}

static void h_online_end(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	struct mscp_cl_cddb *c = e->cddb;
	struct vms_mscp_online_end end;
	struct mscp_cl_ucb *u;

	if (vms_mscp_online_end_parse(e->frame, e->frame_len, &end) !=
	    VMS_CODEC_OK) {
		f->ends_unparsed++;
		return;
	}
	u = cl_ucb_find(f, cl_cddb_index(f, c), end.eh.hdr.unit);
	if (u == (struct mscp_cl_ucb *)0) {
		f->ends_unmatched++;
		return;
	}
	u->online_pending = 0u;

	if (end.eh.status_major != (unsigned)VMS_MSCP_ST_SUCCESS) {
		/* The server refused. Its OWN status word goes to every waiter;
		 * this file composes nothing. */
		f->onlines_refused++;
		cl_fail_waiters(f, cl_ucb_index(f, u), end.eh.status);
		return;
	}

	u->online = 1u;
	/* P.UNSZ: the volume's REAL block count as the server reported it. Only
	 * an ONLINE end carries one, which is why a discovered-but-not-online
	 * unit honestly has none. */
	u->unit_size = end.unit_size;
	u->unit_size_valid = 1u;
	u->unit.unit_flags = end.unit_flags;   /* the ECHO rule's result */
	cl_release_waiters(f, c, cl_ucb_index(f, u));
}

/*
 * A READ or WRITE end message. It is the ONLY thing that completes a transfer,
 * and the byte count reported to the caller is the SERVER's own P.BCNT, never
 * the count we asked for.
 */
static void h_xfer_end(struct mscp_cl_fsm *f, struct cl_ev *e, uint8_t opcode)
{
	struct vms_mscp_xfer_end end;
	struct mscp_cl_cdrp *r;
	vms_codec_status_t st;

	st = opcode == VMS_MSCP_OP_READ
		     ? vms_mscp_read_end_parse(e->frame, e->frame_len, &end)
		     : vms_mscp_write_end_parse(e->frame, e->frame_len, &end);
	if (st != VMS_CODEC_OK) {
		f->ends_unparsed++;
		return;
	}
	r = cl_cdrp_by_ref(f, end.eh.hdr.cmd_ref);
	if (r == (struct mscp_cl_cdrp *)0 || r->opcode != opcode) {
		/* sec 5.1 requires the echo, so an end whose P.CRF matches no
		 * outstanding request of this class is not ours to apply. */
		f->ends_unmatched++;
		return;
	}

	if (end.eh.status_major != (unsigned)VMS_MSCP_ST_SUCCESS)
		f->io_failed++;
	else if (opcode == VMS_MSCP_OP_READ) {
		/*
		 * A SUCCESSFUL READ whose bytes did not all land is a SHORT
		 * READ, and reporting the server's success on top of a partly
		 * filled buffer is exactly the dishonest completion INV-6
		 * exists to stop. Counted, and answered Host Buffer Access
		 * Error (Table B-1 ST.HST -- our buffer did not receive them).
		 */
		if (r->received < end.byte_count) {
			f->short_transfers++;
			cl_cdrp_complete(f, r,
					 VMS_MSCP_STATUS(VMS_MSCP_ST_HOST_BUF_ERR,
							 0u),
					 r->received);
			return;
		}
		f->reads_completed++;
	} else {
		f->writes_completed++;
	}

	cl_cdrp_complete(f, r, end.eh.status, end.byte_count);
}

static void h_read_end(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	h_xfer_end(f, e, VMS_MSCP_OP_READ);
}

static void h_write_end(struct mscp_cl_fsm *f, struct cl_ev *e)
{
	h_xfer_end(f, e, VMS_MSCP_OP_WRITE);
}

static const cl_handler_t cl_table[MSCP_CL_ST__COUNT][MSCP_CL_EV__COUNT] = {
	/* MSCP_CL_ST_AVAILABLE: only the connection's own edges. */
	[MSCP_CL_ST_AVAILABLE] = {
		[MSCP_CL_EV_CONN_OPEN]  = h_conn_open,
		[MSCP_CL_EV_CONN_CLOSE] = h_conn_close
	},
	/* MSCP_CL_ST_DISCOVER: the SCC/GUS walk, and nothing else -- a
	 * transfer end here belongs to no request this driver issued. */
	[MSCP_CL_ST_DISCOVER] = {
		[MSCP_CL_EV_CONN_CLOSE] = h_conn_close,
		[MSCP_CL_EV_SCC_END]    = h_scc_end,
		[MSCP_CL_EV_GUS_END]    = h_gus_end
	},
	/* MSCP_CL_ST_ONLINE: Controller-Online. GUS still has a cell because a
	 * re-walk refreshes the peer's own answers. */
	[MSCP_CL_ST_ONLINE] = {
		[MSCP_CL_EV_CONN_CLOSE] = h_conn_close,
		[MSCP_CL_EV_SCC_END]    = h_scc_end,
		[MSCP_CL_EV_GUS_END]    = h_gus_end,
		[MSCP_CL_EV_ONLINE_END] = h_online_end,
		[MSCP_CL_EV_READ_END]   = h_read_end,
		[MSCP_CL_EV_WRITE_END]  = h_write_end
	}
};

static void cl_dispatch(struct mscp_cl_fsm *f, struct mscp_cl_cddb *c,
			enum mscp_cl_event ev, struct cl_ev *e)
{
	cl_handler_t h;

	if ((unsigned)c->state >= (unsigned)MSCP_CL_ST__COUNT ||
	    (unsigned)ev >= (unsigned)MSCP_CL_EV__COUNT) {
		f->ignored_events++;
		return;
	}
	h = cl_table[c->state][ev];
	if (h == (cl_handler_t)0) {
		f->ignored_events++;   /* an empty cell is COUNTED, not guessed */
		return;
	}
	e->cddb = c;
	h(f, e);
}

/* ==========================================================================
 * 8. Lifecycle and the events
 * ========================================================================== */

void mscp_cl_fsm_init(struct mscp_cl_fsm *f, const struct mscp_cl_ops *ops)
{
	if (f == (struct mscp_cl_fsm *)0)
		return;
	cl_bzero(f, (uint32_t)sizeof(*f));
	f->ops = ops;
}

int mscp_cl_fsm_conn_open(struct mscp_cl_fsm *f, vms_conid_t conid,
			  vms_scs_sysid_t peer, const uint8_t *scsnode,
			  uint32_t scsnode_len)
{
	struct mscp_cl_cddb *c;
	struct cl_ev e;
	uint32_t n;

	if (f == (struct mscp_cl_fsm *)0)
		return -1;
	if (cl_cddb_by_conid(f, conid) != (struct mscp_cl_cddb *)0)
		return 0;   /* idempotent: this connection already has a CDDB */
	c = cl_cddb_alloc(f, conid, peer);
	if (c == (struct mscp_cl_cddb *)0)
		return -1;

	if (scsnode != (const uint8_t *)0 && scsnode_len != 0u) {
		n = scsnode_len < (uint32_t)sizeof(c->scsnode)
		    ? scsnode_len : (uint32_t)sizeof(c->scsnode);
		cl_copy(c->scsnode, scsnode, n);
		c->scsnode_len = (uint8_t)n;
	}

	cl_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	cl_dispatch(f, c, MSCP_CL_EV_CONN_OPEN, &e);
	return 0;
}

void mscp_cl_fsm_conn_closed(struct mscp_cl_fsm *f, vms_conid_t conid)
{
	struct mscp_cl_cddb *c;
	struct cl_ev e;

	if (f == (struct mscp_cl_fsm *)0)
		return;
	c = cl_cddb_by_conid(f, conid);
	if (c == (struct mscp_cl_cddb *)0)
		return;
	cl_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	cl_dispatch(f, c, MSCP_CL_EV_CONN_CLOSE, &e);
}

/*
 * THE SPLICE (header SS8), and the ONE extra thing the CLIENT half needs that
 * the server half does not.
 *
 * A SYSAP is handed a BODY (byte 0 == frame-absolute 72); the codec addresses
 * an MSCP message FRAME-absolutely. The server's splice is therefore a copy at
 * VMS_OFF_SYSAP_BODY and nothing else -- every entry it uses is a per-class
 * parse that only reads the body span.
 *
 * FC-P3.4's discovery FSM is different: it CLASSIFIES before it parses
 * (vms_mscp_cl_fsm.c's mscp_cl_classify_end -> vms_frame_classify ->
 * vms_mscp_classify), which is a check on the frame's own SCA length and
 * message-type words, and a bare body-copy leaves those zero. So the client's
 * splice rebuilds the abs[0,72) envelope through the codec's OWN
 * vms_mscp_link_build, from an ALL-ZERO link and the SCA content length the
 * body we REALLY RECEIVED implies.
 *
 * WHAT THAT DOES AND DOES NOT ASSERT. It asserts nothing: the only non-zero
 * bytes it produces are the length and the format/message-type constants,
 * both derived from the received body's own size, and the classification is
 * then a CHECK on the bytes that really arrived rather than an assertion added
 * to them. NO FRAME BUILT THIS WAY IS EVER TRANSMITTED -- it exists for the
 * length of one dispatch, inside this object. (It is the same reconstruction
 * FC-P6.3's own R1 interop leg makes to drive the same discovery FSM.)
 */
static uint32_t cl_end_frame_len(uint32_t body_len)
{
	return VMS_MSCP_END_FRAME_LEN(body_len);
}

static int cl_splice_end(struct mscp_cl_fsm *f, const uint8_t *body,
			 uint32_t body_len)
{
	struct vms_mscp_link link;
	uint32_t written = 0u;

	cl_bzero(&link, (uint32_t)sizeof(link));
	cl_bzero(f->endframe, (uint32_t)sizeof(f->endframe));
	if (cl_end_frame_len(body_len) > (uint32_t)sizeof(f->endframe))
		return -1;
	if (vms_mscp_link_build(&link, VMS_MSCP_END_SCA_LEN(body_len),
				f->endframe, (uint32_t)sizeof(f->endframe),
				&written) != VMS_CODEC_OK)
		return -1;
	cl_copy(f->endframe + VMS_OFF_SYSAP_BODY, body, body_len);
	return 0;
}

/* The endcode's own opcode selects the event. A COMMAND arriving on a class
 * driver's connection is not ours to service -- this is a class driver, it
 * receives END MESSAGES. */
static enum mscp_cl_event cl_event_for_endcode(uint8_t endcode)
{
	if ((endcode & VMS_MSCP_END_BIT) == 0u)
		return MSCP_CL_EV_END_OTHER;
	switch (endcode & VMS_MSCP_OPCODE_MASK) {
	case VMS_MSCP_OP_SCC:    return MSCP_CL_EV_SCC_END;
	case VMS_MSCP_OP_GUS:    return MSCP_CL_EV_GUS_END;
	case VMS_MSCP_OP_ONLINE: return MSCP_CL_EV_ONLINE_END;
	case VMS_MSCP_OP_READ:   return MSCP_CL_EV_READ_END;
	case VMS_MSCP_OP_WRITE:  return MSCP_CL_EV_WRITE_END;
	default:                 return MSCP_CL_EV_END_OTHER;
	}
}

int mscp_cl_fsm_end_msg(struct mscp_cl_fsm *f, vms_conid_t conid,
			const uint8_t *body, uint32_t len)
{
	struct mscp_cl_cddb *c;
	struct cl_ev e;
	uint8_t endcode = 0u;
	uint32_t n;

	if (f == (struct mscp_cl_fsm *)0 || body == (const uint8_t *)0)
		return -1;
	if (len < VMS_MSCP_HDR_LEN) {
		f->ends_unparsed++;
		return -1;
	}
	c = cl_cddb_by_conid(f, conid);
	if (c == (struct mscp_cl_cddb *)0) {
		f->ends_no_cddb++;
		return -1;
	}

	n = len < VMS_MSCP_END_BODY_MAX ? len : VMS_MSCP_END_BODY_MAX;
	if (cl_splice_end(f, body, n) != 0) {
		f->codec_failures++;
		return -1;
	}

	if (vms_mscp_read_opcode(f->endframe, cl_end_frame_len(n), &endcode) !=
	    VMS_CODEC_OK) {
		f->ends_unparsed++;
		return -1;
	}

	f->ends_rx++;
	c->ends_rx++;
	cl_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.frame = f->endframe;
	e.frame_len = cl_end_frame_len(n);
	cl_dispatch(f, c, cl_event_for_endcode(endcode), &e);
	return 0;
}

void mscp_cl_fsm_block_data(struct mscp_cl_fsm *f, uint32_t name,
			    uint32_t offset, uint32_t len,
			    uint32_t bytes_remaining)
{
	struct mscp_cl_cdrp *r;

	(void)offset; (void)bytes_remaining;
	if (f == (struct mscp_cl_fsm *)0)
		return;
	r = cl_cdrp_by_name(f, name);
	if (r == (struct mscp_cl_cdrp *)0) {
		f->block_unmatched++;
		return;   /* not one of ours: the port already counted it */
	}

	r->received += len;
	if (r->received > r->byte_count)
		r->received = r->byte_count;
	f->block_bytes_rx += len;
	/*
	 * NOTHING COMPLETES HERE. A transfer is finished when the server's END
	 * MESSAGE says so and reports its own byte count; treating the last
	 * data frame as a completion would be this driver deciding a result the
	 * server has not stated. h_xfer_end() cross-checks `received` against
	 * that end message and refuses a short read.
	 */
}

uint32_t mscp_cl_fsm_tick(struct mscp_cl_fsm *f)
{
	uint32_t reaped = 0u;
	uint32_t now;
	uint32_t i;

	if (f == (struct mscp_cl_fsm *)0)
		return 0u;
	now = cl_now(f);
	for (i = 0; i < MSCP_CL_MAX_REQS; i++) {
		struct mscp_cl_cdrp *r = &f->cdrp[i];

		if (!r->in_use || r->deadline_ms == 0u)
			continue;
		if ((uint32_t)(now - r->started_ms) < r->deadline_ms)
			continue;

		f->reqs_aborted++;
		if (r->opcode == VMS_MSCP_OP_WRITE)
			f->writes_undelivered++;
		cl_log(f, "vms: MSCP class driver: request timed out, "
			  "aborted\n");
		/* Table B-1 ST.ABO. The honest end for a transfer whose bytes
		 * never arrived: the caller is told, in place of a request that
		 * hangs forever (vms_pe_fsm.h SS8d, "the port does not
		 * retransmit a lost block frame"). */
		cl_cdrp_complete(f, r,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_ABORTED, 0u), 0u);
		reaped++;
	}
	return reaped;
}

/* ==========================================================================
 * 9. The I/O services
 * ========================================================================== */

/*
 * The shared body of read and write. Validates against REAL state, takes a
 * CDRP, and either issues the command now or holds it behind a real ONLINE.
 */
static int cl_io(struct mscp_cl_fsm *f, const char *devnam, uint32_t lbn,
		 uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		 uint32_t handle, uint8_t opcode)
{
	struct mscp_cl_ucb *u;
	struct mscp_cl_cddb *c;
	struct mscp_cl_cdrp *r;
	uint32_t bytes;

	if (f == (struct mscp_cl_fsm *)0 || devnam == (const char *)0 ||
	    buf == (uint8_t *)0 || nblocks == 0u)
		return -1;
	bytes = nblocks * MSCP_CL_BLOCK_SIZE;
	if (bytes > buf_len)
		return -1;   /* the caller's buffer cannot hold it: refused */

	u = cl_ucb_by_devnam(f, devnam);
	if (u == (struct mscp_cl_ucb *)0)
		return -1;   /* no such served device */
	if (u->cddb >= MSCP_CL_MAX_CTLRS || !f->cddb[u->cddb].in_use)
		return -1;
	c = &f->cddb[u->cddb];
	if (c->state != (uint8_t)MSCP_CL_ST_ONLINE)
		return -1;   /* the controller is not online to us yet */

	r = cl_cdrp_alloc(f);
	if (r == (struct mscp_cl_cdrp *)0)
		return -1;   /* counted; never an evicted request */
	r->opcode = opcode;
	r->ucb = (uint8_t)cl_ucb_index(f, u);
	r->handle = handle;
	r->lbn = lbn;
	r->byte_count = bytes;
	r->buf = buf;
	r->buf_len = buf_len;

	if (!u->online) {
		/*
		 * sec 4.3: a command to a Unit-Offline unit is rejected. Bring
		 * it online FIRST and hold this request until the real ONLINE
		 * end arrives -- under a deadline from the moment we start
		 * waiting, so a unit that never comes online is a request that
		 * is reaped, not one that hangs.
		 */
		r->waiting_online = 1u;
		r->started_ms = cl_now(f);
		r->deadline_ms = cl_deadline_ms(f, c);
		if (!u->online_pending)
			cl_send_online(f, c, u);
		return 0;
	}
	if (cl_issue_xfer(f, c, r) != 0) {
		cl_bzero(r, (uint32_t)sizeof(*r));
		return -1;   /* refused SYNCHRONOUSLY: no io_done, no leak */
	}
	return 0;
}

int mscp_cl_fsm_read(struct mscp_cl_fsm *f, const char *devnam, uint32_t lbn,
		     uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		     uint32_t handle)
{
	return cl_io(f, devnam, lbn, nblocks, buf, buf_len, handle,
		     VMS_MSCP_OP_READ);
}

int mscp_cl_fsm_write(struct mscp_cl_fsm *f, const char *devnam, uint32_t lbn,
		      uint32_t nblocks, uint8_t *buf, uint32_t buf_len,
		      uint32_t handle)
{
	return cl_io(f, devnam, lbn, nblocks, buf, buf_len, handle,
		     VMS_MSCP_OP_WRITE);
}

/* ==========================================================================
 * 10. Readback
 * ========================================================================== */

const struct mscp_cl_cddb *mscp_cl_fsm_cddb_at(const struct mscp_cl_fsm *f,
					       uint32_t index)
{
	if (f == (const struct mscp_cl_fsm *)0 || index >= MSCP_CL_MAX_CTLRS)
		return (const struct mscp_cl_cddb *)0;
	return f->cddb[index].in_use ? &f->cddb[index]
				     : (const struct mscp_cl_cddb *)0;
}

const struct mscp_cl_ucb *mscp_cl_fsm_ucb_at(const struct mscp_cl_fsm *f,
					     uint32_t index)
{
	if (f == (const struct mscp_cl_fsm *)0 || index >= MSCP_CL_MAX_UNITS)
		return (const struct mscp_cl_ucb *)0;
	return f->ucb[index].in_use ? &f->ucb[index]
				    : (const struct mscp_cl_ucb *)0;
}

uint32_t mscp_cl_fsm_unit_count(const struct mscp_cl_fsm *f)
{
	uint32_t i, n = 0u;

	if (f == (const struct mscp_cl_fsm *)0)
		return 0u;
	for (i = 0; i < MSCP_CL_MAX_UNITS; i++) {
		if (f->ucb[i].in_use)
			n++;
	}
	return n;
}

const char *mscp_cl_state_name(enum mscp_cl_state s)
{
	switch (s) {
	case MSCP_CL_ST_AVAILABLE: return "controller-available";
	case MSCP_CL_ST_DISCOVER:  return "discovering";
	case MSCP_CL_ST_ONLINE:    return "controller-online";
	default:                   return "?";
	}
}
