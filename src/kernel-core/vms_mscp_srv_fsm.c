/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_mscp_srv_fsm.c - the MSCP disk server's pure core (FC-P6.3).
 *
 * The structures (UQB/HQB/HRB), the controller-state model, the INV-6 rule for
 * every field, and the write-protection contract are all in
 * vms_mscp_srv_fsm.h. Read it first; this file is the behaviour.
 *
 * READ THE TABLE, NOT THE PROSE. srv_table[][] below IS the specification of
 * this machine: one cell per [state][event], one small handler per edge. An
 * empty cell is an event that state has no edge for -- ignored and COUNTED
 * (`ignored_events`), never guessed at.
 *
 * NO WIRE OFFSET LIVES HERE (design SS3.9 rule 2). Every command is parsed and
 * every end message built through vms_cluster_codec_mscp.h; the only POSITION
 * this file names is VMS_OFF_SYSAP_BODY, the codec's own published body origin,
 * used to splice a SYSAP body into the frame-absolute buffer the codec
 * addresses -- exactly what vms_cnxman_join_fsm.c does for the client half.
 *
 * PURE TU (design SS3.9 rule 4, gate RULE4): no seam call, no allocation, no
 * clock but ops->now_ms.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_mscp_srv_fsm.h"

/* ==========================================================================
 * 0. Small shared helpers (a pure TU calls no library -- it builds on the
 * host too, where the substrate's memset is not in scope)
 * ========================================================================== */

static void srv_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

static void srv_copy(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static void srv_log(const struct mscp_srv_fsm *f, const char *msg)
{
	if (f->ops != (const struct mscp_srv_ops *)0 &&
	    f->ops->log != (void (*)(void *, const char *))0)
		f->ops->log(f->ops->ctx, msg);
}

static uint32_t srv_now(const struct mscp_srv_fsm *f)
{
	if (f->ops != (const struct mscp_srv_ops *)0 &&
	    f->ops->now_ms != (uint32_t (*)(void *))0)
		return f->ops->now_ms(f->ops->ctx);
	return 0u;
}

/* ==========================================================================
 * 1. The UQB table -- served units, read from the executive and nowhere else
 * ========================================================================== */

uint32_t mscp_srv_fsm_refresh_units(struct mscp_srv_fsm *f)
{
	uint32_t i, n = 0u;

	if (f == (struct mscp_srv_fsm *)0)
		return 0u;

	srv_bzero(f->uqb, (uint32_t)sizeof(f->uqb));
	f->n_units = 0u;
	if (f->ops == (const struct mscp_srv_ops *)0 ||
	    f->ops->unit_at == (int (*)(void *, uint32_t,
					struct mscp_srv_unit_info *))0)
		return 0u;

	for (i = 0; i < MSCP_SRV_MAX_UNITS; i++) {
		struct mscp_srv_unit_info info;

		srv_bzero(&info, (uint32_t)sizeof(info));
		if (f->ops->unit_at(f->ops->ctx, i, &info) != 0)
			break;      /* the honest end of the executive's list */
		/*
		 * sec 6.12 makes a ZERO unit identifier mean "virtually no
		 * characteristics are valid", and a zero-length volume is not a
		 * volume. Either one means the executive did not really hand us
		 * a serveable unit, so it is NOT taken as one (INV-6: a UQB is
		 * never created for something the executive does not hold).
		 */
		if (info.unit_id == 0u || info.unit_size == 0u)
			continue;
		f->uqb[n].in_use = 1u;
		f->uqb[n].info = info;
		n++;
	}
	f->n_units = n;
	return n;
}

uint32_t mscp_srv_fsm_unit_count(const struct mscp_srv_fsm *f)
{
	return f != (const struct mscp_srv_fsm *)0 ? f->n_units : 0u;
}

/* The UQB slot for an MSCP unit NUMBER, or -1. */
static int srv_uqb_slot(const struct mscp_srv_fsm *f, uint16_t unit)
{
	uint32_t i;

	for (i = 0; i < f->n_units; i++) {
		if (f->uqb[i].in_use && f->uqb[i].info.unit == unit)
			return (int)i;
	}
	return -1;
}

/*
 * The GUS NEXT-UNIT walk's own lookup (sec 6.12: MD.NXU returns "the status of
 * the next unit with a unit number >= the specified unit number, in ascending
 * order"). Returns the UQB slot with the SMALLEST unit number that is >= `from`,
 * or -1 when there is none -- which is the walk's terminator, not an error.
 */
static int srv_uqb_next(const struct mscp_srv_fsm *f, uint16_t from)
{
	uint32_t i;
	int best = -1;

	for (i = 0; i < f->n_units; i++) {
		if (!f->uqb[i].in_use || f->uqb[i].info.unit < from)
			continue;
		if (best < 0 ||
		    f->uqb[i].info.unit < f->uqb[best].info.unit)
			best = (int)i;
	}
	return best;
}

const struct mscp_srv_uqb *mscp_srv_fsm_uqb_at(const struct mscp_srv_fsm *f,
					       uint32_t index)
{
	if (f == (const struct mscp_srv_fsm *)0 || index >= MSCP_SRV_MAX_UNITS)
		return (const struct mscp_srv_uqb *)0;
	return f->uqb[index].in_use ? &f->uqb[index]
				    : (const struct mscp_srv_uqb *)0;
}

/* ==========================================================================
 * 2. The HQB table -- one per remote class driver
 * ========================================================================== */

static struct mscp_srv_hqb *srv_hqb_by_conid(struct mscp_srv_fsm *f,
					     vms_conid_t conid)
{
	uint32_t i;

	for (i = 0; i < MSCP_SRV_MAX_HOSTS; i++) {
		if (f->hqb[i].in_use && f->hqb[i].conid == conid)
			return &f->hqb[i];
	}
	return (struct mscp_srv_hqb *)0;
}

static struct mscp_srv_hqb *srv_hqb_alloc(struct mscp_srv_fsm *f,
					  vms_conid_t conid,
					  vms_scs_sysid_t peer)
{
	uint32_t i;

	for (i = 0; i < MSCP_SRV_MAX_HOSTS; i++) {
		if (f->hqb[i].in_use)
			continue;
		srv_bzero(&f->hqb[i], (uint32_t)sizeof(f->hqb[i]));
		f->hqb[i].in_use = 1u;
		f->hqb[i].state = (uint8_t)MSCP_SRV_ST_AVAILABLE;
		f->hqb[i].conid = conid;
		f->hqb[i].peer = peer;
		return &f->hqb[i];
	}
	f->no_hqb_slot++;
	return (struct mscp_srv_hqb *)0;
}

static uint32_t srv_hqb_index(const struct mscp_srv_fsm *f,
			      const struct mscp_srv_hqb *h)
{
	return (uint32_t)(h - &f->hqb[0]);
}

const struct mscp_srv_hqb *mscp_srv_fsm_hqb_at(const struct mscp_srv_fsm *f,
					       uint32_t index)
{
	if (f == (const struct mscp_srv_fsm *)0 || index >= MSCP_SRV_MAX_HOSTS)
		return (const struct mscp_srv_hqb *)0;
	return f->hqb[index].in_use ? &f->hqb[index]
				    : (const struct mscp_srv_hqb *)0;
}

const char *mscp_srv_state_name(enum mscp_srv_state s)
{
	switch (s) {
	case MSCP_SRV_ST_AVAILABLE: return "CONTROLLER-AVAILABLE";
	case MSCP_SRV_ST_ONLINE:    return "CONTROLLER-ONLINE";
	default:                    return "?";
	}
}

/* ==========================================================================
 * 3. The HRB table -- one per outstanding remote request
 * ========================================================================== */

static struct mscp_srv_hrb *srv_hrb_alloc(struct mscp_srv_fsm *f)
{
	uint32_t i;

	for (i = 0; i < MSCP_SRV_MAX_REQS; i++) {
		if (f->hrb[i].in_use)
			continue;
		srv_bzero(&f->hrb[i], (uint32_t)sizeof(f->hrb[i]));
		f->hrb[i].in_use = 1u;
		f->hrb[i].started_ms = srv_now(f);
		return &f->hrb[i];
	}
	f->no_hrb_slot++;
	return (struct mscp_srv_hrb *)0;
}

/* The staging slot an HRB owns for its whole life (see the header). */
static uint8_t *srv_hrb_buf(struct mscp_srv_fsm *f, const struct mscp_srv_hrb *r)
{
	uint32_t i = (uint32_t)(r - &f->hrb[0]);

	if (f->xferbuf == (uint8_t *)0 || i >= MSCP_SRV_MAX_REQS)
		return (uint8_t *)0;
	return f->xferbuf + (i * f->xferbuf_slot);
}

static struct mscp_srv_hrb *srv_hrb_by_name(struct mscp_srv_fsm *f,
					    uint32_t name)
{
	uint32_t i;

	if (name == 0u)
		return (struct mscp_srv_hrb *)0;
	for (i = 0; i < MSCP_SRV_MAX_REQS; i++) {
		if (f->hrb[i].in_use && f->hrb[i].local_name == name)
			return &f->hrb[i];
	}
	return (struct mscp_srv_hrb *)0;
}

/* The HRB waiting on the worker for `tag`, or NULL. Matched on the tag and on
 * WAIT_IO together: an HRB that has moved on cannot be answered by an old
 * completion (see the header's note on why the index is not the key). */
static struct mscp_srv_hrb *srv_hrb_by_io_tag(struct mscp_srv_fsm *f,
					      uint32_t tag)
{
	uint32_t i;

	if (tag == 0u)
		return (struct mscp_srv_hrb *)0;
	for (i = 0; i < MSCP_SRV_MAX_REQS; i++) {
		if (f->hrb[i].in_use &&
		    f->hrb[i].state == (uint8_t)MSCP_SRV_REQ_WAIT_IO &&
		    f->hrb[i].io_tag == tag)
			return &f->hrb[i];
	}
	return (struct mscp_srv_hrb *)0;
}

/* Mint the next outstanding-request tag. Never 0 (the header says why), and
 * never a value another outstanding request is already using -- with
 * MSCP_SRV_MAX_REQS outstanding at most, a full 32-bit wrap would have to lap
 * the whole space before a live tag could repeat, and the scan makes even that
 * impossible rather than improbable. */
static uint32_t srv_next_io_tag(struct mscp_srv_fsm *f)
{
	uint32_t guard;

	for (guard = 0; guard <= MSCP_SRV_MAX_REQS; guard++) {
		f->next_io_tag++;
		if (f->next_io_tag == 0u)
			f->next_io_tag = 1u;
		if (srv_hrb_by_io_tag(f, f->next_io_tag) ==
		    (struct mscp_srv_hrb *)0)
			return f->next_io_tag;
	}
	return 0u;   /* unreachable with MAX_REQS+1 tries; 0 = "no tag" */
}

static void srv_hrb_free(struct mscp_srv_fsm *f, struct mscp_srv_hrb *r)
{
	if (r->local_name != 0u && f->ops != (const struct mscp_srv_ops *)0 &&
	    f->ops->release_buffer != (void (*)(void *, uint32_t))0)
		f->ops->release_buffer(f->ops->ctx, r->local_name);
	srv_bzero(r, (uint32_t)sizeof(*r));
}

/* ==========================================================================
 * 4. Emission -- the ONE place an end message leaves this file
 *
 * `f->endframe` is the codec's frame-absolute buffer; what goes out is the
 * SYSAP body at abs 72 and nothing below it (design SS3.2.4: SCS owns 56-71,
 * the port owns 0-55).
 * ========================================================================== */

static int srv_emit(struct mscp_srv_fsm *f, vms_conid_t conid, uint32_t body_len)
{
	int rc;

	if (f->ops == (const struct mscp_srv_ops *)0 ||
	    f->ops->send_end == (int (*)(void *, vms_conid_t, const uint8_t *,
					 uint32_t))0) {
		f->end_tx_failed++;
		return -1;
	}
	rc = f->ops->send_end(f->ops->ctx, conid,
			      f->endframe + VMS_OFF_SYSAP_BODY, body_len);
	if (rc != 0) {
		f->end_tx_failed++;
		return -1;
	}
	f->ends_tx++;
	return 0;
}

/* The 12-byte generic end header every class shares, filled from the host's
 * own command (P.CRF, P.UNIT) plus the status this server computed. */
static void srv_end_hdr(struct vms_mscp_end_hdr *eh, uint32_t cmd_ref,
			uint16_t unit, uint16_t status)
{
	srv_bzero(eh, (uint32_t)sizeof(*eh));
	eh->hdr.cmd_ref = cmd_ref;
	eh->hdr.unit = unit;
	eh->flags = 0u;   /* Table A-3: no bad block, no error log -- true */
	eh->status = status;
	eh->status_major = vms_mscp_status_major(status);
	eh->status_subcode = vms_mscp_status_subcode(status);
}

/* ==========================================================================
 * 5. The per-class end-message builders
 * ========================================================================== */

/*
 * SET CONTROLLER CHARACTERISTICS end (sec 6.16). P.CNTF and P.CTMO report
 * what is ACTUALLY in effect in this server -- the flags the host itself set
 * on this connection, and this server's own request-reaper deadline -- not the
 * ungrounded 0xa004/0x0547 pair the reference corpus shows a real controller
 * emit (vms_cluster_codec_mscp.h names both as the CALLER's choice, and
 * docs/design-mscp-direction.md lists the bits behind them as "still
 * ungrounded, do not build on"). Replaying an undecoded captured constant is
 * the fabrication INV-6 exists to stop; whether a real class driver needs it is
 * an R5 question for FC-P6.4, not a guess to make here.
 */
static int srv_send_scc_end(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			    uint32_t cmd_ref, uint16_t unit, uint16_t status)
{
	struct vms_mscp_scc_end e;
	uint32_t body_len = 0u;

	srv_bzero(&e, (uint32_t)sizeof(e));
	srv_end_hdr(&e.eh, cmd_ref, unit, status);
	e.version = 0u;                       /* sec 6.16: this protocol version */
	e.ctlr_flags = h->ctlr_flags;         /* what the host set, in effect    */
	e.ctlr_timeout = (uint16_t)MSCP_SRV_CTLR_TIMEOUT_SECS;
	e.rsvd18 = 0u;                        /* Table A-7 RESERVED, honest zero */
	e.ctlr_id = f->ctlr_id;

	srv_bzero(f->endframe, (uint32_t)sizeof(f->endframe));
	if (vms_mscp_scc_end_build(&e, f->endframe,
				   (uint32_t)sizeof(f->endframe),
				   &body_len) != VMS_CODEC_OK) {
		f->end_tx_failed++;
		return -1;
	}
	return srv_emit(f, h->conid, body_len);
}

/*
 * The P.UNFL a GUS or ONLINE end carries. THE ECHO RULE, executed: the codec's
 * vms_mscp_online_unfl_compose() ORs the host's OWN requested word (recorded on
 * the HQB from its ONLINE command) with the unit's own flags. Before this host
 * has sent an ONLINE for this unit the first half genuinely does not exist, and
 * a zero goes out and is COUNTED -- never back-filled (see the header).
 */
static uint16_t srv_unfl(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			 int uqb_slot)
{
	uint16_t host_unfl = h->host_unfl[uqb_slot];
	uint16_t unit_flags = f->uqb[uqb_slot].info.unit_flags;

	if (host_unfl == 0u)
		f->unfl_no_host_value++;
	return vms_mscp_online_unfl_compose(host_unfl, unit_flags);
}

/* Note the two honestly-absent identities as they go out (INV-6: the value the
 * executive has not got is a counted zero, never a composed one). */
static void srv_note_absences(struct mscp_srv_fsm *f,
			      const struct mscp_srv_unit_info *u)
{
	if (!u->media_valid)
		f->media_absent++;
	if (!u->volume_ser_valid)
		f->vser_absent++;
}

/*
 * GET UNIT STATUS end (sec 6.12), 52 bytes MEASURED. `uqb_slot < 0` is the
 * walk's terminator: sec 6.12's own "if unit identifier = 0, virtually no
 * characteristics are valid", so every characteristic really is zero and the
 * unit number echoed is the one the host asked about.
 */
static int srv_send_gus_end(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			    uint32_t cmd_ref, uint16_t unit, uint16_t status,
			    int uqb_slot)
{
	struct vms_mscp_gus_end e;
	uint32_t body_len = 0u;

	srv_bzero(&e, (uint32_t)sizeof(e));
	srv_end_hdr(&e.eh, cmd_ref, unit, status);
	if (uqb_slot >= 0) {
		const struct mscp_srv_unit_info *u = &f->uqb[uqb_slot].info;

		e.unit_flags = srv_unfl(f, h, uqb_slot);
		e.unit_id = u->unit_id;
		e.media_id = u->media_valid ? u->media_id : 0u;
		e.shadow_unit = u->unit;   /* sec 6.12: == the unit number */
		srv_note_absences(f, u);
		/*
		 * Geometry: ZERO, which is sec 6.12's OWN encoding for
		 * "inapplicable" (track size 0 => group size 0 => cylinder size
		 * 0), and what every geometry-less unit in the reference corpus
		 * emits. A served OVMX volume is a linear block range; inventing
		 * a track/cylinder shape for it would be a hardware claim.
		 */
	}

	srv_bzero(f->endframe, (uint32_t)sizeof(f->endframe));
	if (vms_mscp_gus_end_build(&e, f->endframe,
				   (uint32_t)sizeof(f->endframe),
				   &body_len) != VMS_CODEC_OK) {
		f->end_tx_failed++;
		return -1;
	}
	return srv_emit(f, h->conid, body_len);
}

/* ONLINE end (sec 6.13), 44 bytes MEASURED. */
static int srv_send_online_end(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			       uint32_t cmd_ref, uint16_t unit, uint16_t status,
			       int uqb_slot)
{
	struct vms_mscp_online_end e;
	uint32_t body_len = 0u;

	srv_bzero(&e, (uint32_t)sizeof(e));
	srv_end_hdr(&e.eh, cmd_ref, unit, status);
	if (uqb_slot >= 0) {
		const struct mscp_srv_unit_info *u = &f->uqb[uqb_slot].info;

		e.unit_flags = srv_unfl(f, h, uqb_slot);
		e.unit_id = u->unit_id;
		e.media_id = u->media_valid ? u->media_id : 0u;
		e.unit_size = u->unit_size;   /* the VOLUME's real block count */
		e.volume_ser = u->volume_ser_valid ? u->volume_ser : 0u;
		srv_note_absences(f, u);
	}

	srv_bzero(f->endframe, (uint32_t)sizeof(f->endframe));
	if (vms_mscp_online_end_build(&e, f->endframe,
				      (uint32_t)sizeof(f->endframe),
				      &body_len) != VMS_CODEC_OK) {
		f->end_tx_failed++;
		return -1;
	}
	return srv_emit(f, h->conid, body_len);
}

/*
 * Build a READ or WRITE end into f->endframe. Returns the body length, or 0 on
 * a codec refusal. Split from the send because READ's end message does not go
 * out through send_end at all -- it is PIGGYBACKED on the final block-transfer
 * frame (FC-P6.1's TRAP 1), so its bytes are handed to ops->send_read_data.
 */
static uint32_t srv_build_xfer_end(struct mscp_srv_fsm *f, uint8_t base_opcode,
				   uint32_t cmd_ref, uint16_t unit,
				   uint16_t status, uint32_t byte_count)
{
	struct vms_mscp_xfer_end e;
	uint32_t body_len = 0u;
	vms_codec_status_t st;

	srv_bzero(&e, (uint32_t)sizeof(e));
	srv_end_hdr(&e.eh, cmd_ref, unit, status);
	/* eh.hdr.opcode is not read on build: each class builder stamps its own
	 * endcode, which is why `base_opcode` selects the builder below. */
	e.byte_count = byte_count;   /* what ACTUALLY moved, never what was asked */
	e.first_bad_block = 0u;

	srv_bzero(f->endframe, (uint32_t)sizeof(f->endframe));
	st = (base_opcode == VMS_MSCP_OP_READ)
		     ? vms_mscp_read_end_build(&e, f->endframe,
					       (uint32_t)sizeof(f->endframe),
					       &body_len)
		     : vms_mscp_write_end_build(&e, f->endframe,
						(uint32_t)sizeof(f->endframe),
						&body_len);
	if (st != VMS_CODEC_OK) {
		f->end_tx_failed++;
		return 0u;
	}
	return body_len;
}

static int srv_send_xfer_end(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			     uint8_t base_opcode, uint32_t cmd_ref,
			     uint16_t unit, uint16_t status, uint32_t byte_count)
{
	uint32_t body_len = srv_build_xfer_end(f, base_opcode, cmd_ref, unit,
					       status, byte_count);

	if (body_len == 0u)
		return -1;
	return srv_emit(f, h->conid, body_len);
}

/* ==========================================================================
 * 6. The event record a handler is given
 * ========================================================================== */

struct srv_ev {
	struct mscp_srv_hqb *hqb;    /* NULL only for CONN_OPEN               */
	vms_conid_t          conid;
	vms_scs_sysid_t      peer;
	const uint8_t       *frame;  /* f->cmdframe: the spliced command      */
	uint32_t             frame_len;
};

typedef void (*srv_handler_t)(struct mscp_srv_fsm *f, struct srv_ev *e);

/* ==========================================================================
 * 7. Handlers
 * ========================================================================== */

/* The connection reached OPEN: sec 4.1's own "Controller-Online ... exactly
 * when a connection exists between the class driver and the MSCP server". */
static void h_conn_open(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct mscp_srv_hqb *h = srv_hqb_by_conid(f, e->conid);

	if (h == (struct mscp_srv_hqb *)0)
		h = srv_hqb_alloc(f, e->conid, e->peer);
	if (h == (struct mscp_srv_hqb *)0)
		return;   /* counted in no_hqb_slot; nothing invented */
	h->peer = e->peer;
	h->state = (uint8_t)MSCP_SRV_ST_ONLINE;
	srv_log(f, "vms: MSCP$DISK controller-online to a class driver\n");
}

/*
 * The connection closed. sec 4.1: "the MSCP server must guarantee that there
 * are no outstanding commands leftover from a previous incarnation of the
 * connection", so every HRB this host owned is discarded HERE -- with its named
 * buffer released, so a stale transfer cannot land in a buffer that has since
 * changed hands.
 *
 * EXCEPT one the WORKER still owns (FC-P6.6). Its staging slot is in another
 * thread's hands: freeing the HRB would return that slot to the pool while a
 * disk read is writing into it, and would let the next request be handed the
 * same bytes. So it is marked `abandoned` and left in use; the completion path
 * frees it, in silence, because by then there is genuinely nobody to answer.
 */
static void h_conn_close(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct mscp_srv_hqb *h = e->hqb;
	uint32_t i, slot;

	if (h == (struct mscp_srv_hqb *)0)
		return;
	slot = srv_hqb_index(f, h);
	for (i = 0; i < MSCP_SRV_MAX_REQS; i++) {
		if (!f->hrb[i].in_use || f->hrb[i].hqb != (uint8_t)slot)
			continue;
		if (f->hrb[i].state == (uint8_t)MSCP_SRV_REQ_WAIT_IO)
			f->hrb[i].abandoned = 1u;
		else
			srv_hrb_free(f, &f->hrb[i]);
	}
	srv_bzero(h, (uint32_t)sizeof(*h));
}

/* SET CONTROLLER CHARACTERISTICS (sec 6.16). The ONE command whose parameters
 * this server RECORDS: the host's controller flags and host timeout become
 * this HQB's real state and are what the end message reports back. */
static void h_cmd_scc(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct vms_mscp_scc_cmd c;
	struct mscp_srv_hqb *h = e->hqb;

	if (vms_mscp_scc_cmd_parse(e->frame, e->frame_len, &c) != VMS_CODEC_OK) {
		f->cmds_unparsed++;
		return;
	}
	/* sec 6.16: "host must supply 0; server returns Invalid Command if
	 * non-zero (reserved for future non-backward-compatible revisions)". */
	if (c.version != 0u) {
		h->invalid_cmds++;
		(void)srv_send_scc_end(f, h, c.hdr.cmd_ref, c.hdr.unit,
				       VMS_MSCP_STATUS(VMS_MSCP_ST_INVALID_CMD,
						       0u));
		return;
	}
	h->ctlr_flags = c.ctlr_flags;
	h->host_timeout = c.host_timeout;
	if (h->scc_done < 0xffu)
		h->scc_done++;
	/* sec 6.16: "Status codes: Success (sub-code Normal) only." */
	(void)srv_send_scc_end(f, h, c.hdr.cmd_ref, c.hdr.unit,
			       VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
					       VMS_MSCP_SUB_NORMAL));
}

/*
 * GET UNIT STATUS (sec 6.12), with and without the NEXT-UNIT modifier. This is
 * a STATUS QUERY, which sec 4.3 exempts from the "commands addressed to a
 * Unit-Offline unit will be rejected" rule -- so it answers for a unit that is
 * not online, and the walk's terminator is Unit-Offline rather than an error.
 */
static void h_cmd_gus(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct vms_mscp_gus_cmd c;
	struct mscp_srv_hqb *h = e->hqb;
	int slot;
	uint16_t unit, status;

	if (vms_mscp_gus_cmd_parse(e->frame, e->frame_len, &c) != VMS_CODEC_OK) {
		f->cmds_unparsed++;
		return;
	}

	slot = (c.modifiers & VMS_MSCP_MOD_NEXT_UNIT) != 0u
		       ? srv_uqb_next(f, c.hdr.unit)
		       : srv_uqb_slot(f, c.hdr.unit);
	if (slot < 0) {
		/* Table B-2 Unit-Offline sub-code 0, "unit unknown or online to
		 * another controller" -- the end of the enumeration, echoing the
		 * unit number the host actually asked about. */
		(void)srv_send_gus_end(f, h, c.hdr.cmd_ref, c.hdr.unit,
				       VMS_MSCP_STATUS(VMS_MSCP_ST_OFFLINE,
						       VMS_MSCP_SUB_OFL_UNKNOWN),
				       -1);
		return;
	}

	unit = f->uqb[slot].info.unit;
	/* sec 6.12 status codes: Success implies Unit-Online; Unit-Available is
	 * the answer for a real unit this class driver has not ONLINEd yet.
	 * Which one it is comes from THIS host's own per-unit state (sec 4.3). */
	status = h->online[slot]
			 ? VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
					   VMS_MSCP_SUB_NORMAL)
			 : VMS_MSCP_STATUS(VMS_MSCP_ST_AVAILABLE, 0u);
	(void)srv_send_gus_end(f, h, c.hdr.cmd_ref, unit, status, slot);
}

/*
 * ONLINE (sec 6.13). Brings the unit Unit-Online TO THIS CLASS DRIVER and sets
 * the host-settable characteristics -- which is where MD.SWP ("Enable Set Write
 * Protect") and the host's own P.UNFL enter this server's state, and therefore
 * where a host's request for software write protection becomes a real fact the
 * WRITE path then honours.
 */
static void h_cmd_online(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct vms_mscp_online_cmd c;
	struct mscp_srv_hqb *h = e->hqb;
	int slot;
	uint16_t host_unfl, status;

	if (vms_mscp_online_cmd_parse(e->frame, e->frame_len, &c) !=
	    VMS_CODEC_OK) {
		f->cmds_unparsed++;
		return;
	}

	slot = srv_uqb_slot(f, c.hdr.unit);
	if (slot < 0) {
		(void)srv_send_online_end(f, h, c.hdr.cmd_ref, c.hdr.unit,
					  VMS_MSCP_STATUS(VMS_MSCP_ST_OFFLINE,
							  VMS_MSCP_SUB_OFL_UNKNOWN),
					  -1);
		return;
	}

	/* The host's own unit-flags word, plus the MD.SWP modifier expressed as
	 * the Table A-5 bit it asks for. Both are READ off the command. */
	host_unfl = c.unit_flags;
	if ((c.modifiers & VMS_MSCP_MOD_SET_WRITE_PROT) != 0u)
		host_unfl = (uint16_t)(host_unfl | VMS_MSCP_UF_WRITE_PROT_SW);
	h->host_unfl[slot] = host_unfl;

	/* sec 6.13: "Success (Already Online -- unit already Unit-Online to this
	 * class driver, state unaltered)" is a DISTINCT sub-code, not a
	 * duplicate success. */
	status = h->online[slot]
			 ? VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
					   VMS_MSCP_SUB_ALREADY_ONLINE)
			 : VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
					   VMS_MSCP_SUB_NORMAL);
	h->online[slot] = 1u;
	(void)srv_send_online_end(f, h, c.hdr.cmd_ref, c.hdr.unit, status, slot);
}

/* ==========================================================================
 * 7b. The transfer commands' shared gates
 * ========================================================================== */

/*
 * Everything a READ and a WRITE must both pass before a byte moves. Fills
 * *status with the real MSCP status of the FIRST gate that failed and returns
 * -1; returns the UQB slot on success. Every refusal here is a status a class
 * driver can act on -- never a dropped command.
 */
static int srv_xfer_gate(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			 const struct vms_mscp_xfer_cmd *c, uint16_t *status)
{
	int slot = srv_uqb_slot(f, c->hdr.unit);
	uint32_t nblocks;

	if (slot < 0) {
		*status = VMS_MSCP_STATUS(VMS_MSCP_ST_OFFLINE,
					  VMS_MSCP_SUB_OFL_UNKNOWN);
		return -1;
	}
	/* sec 4.3: "Except for status queries, MSCP commands addressed to a unit
	 * that is Unit-Offline will be rejected." A unit this class driver has
	 * not ONLINEd is Unit-Available to it, and that is what it is told. */
	if (!h->online[slot]) {
		*status = VMS_MSCP_STATUS(VMS_MSCP_ST_AVAILABLE, 0u);
		return -1;
	}
	/* sec 5.3: P.BCNT is a whole number of blocks. Table B-2: an Invalid
	 * Command names the offending FIELD by its command-message offset. */
	if (c->byte_count == 0u ||
	    (c->byte_count % MSCP_SRV_BLOCK_SIZE) != 0u ||
	    c->byte_count > f->xferbuf_slot ||
	    f->xferbuf == (uint8_t *)0) {
		*status = VMS_MSCP_STATUS(VMS_MSCP_ST_INVALID_CMD,
					  MSCP_SRV_SUB_INVALID_FIELD(
						  MSCP_SRV_CMDOFF_BCNT));
		return -1;
	}
	nblocks = c->byte_count / MSCP_SRV_BLOCK_SIZE;
	if (c->lbn > f->uqb[slot].info.unit_size ||
	    nblocks > f->uqb[slot].info.unit_size - c->lbn) {
		*status = VMS_MSCP_STATUS(VMS_MSCP_ST_INVALID_CMD,
					  MSCP_SRV_SUB_INVALID_FIELD(
						  MSCP_SRV_CMDOFF_LBN));
		return -1;
	}
	return slot;
}

/*
 * THE WRITE-PROTECTION READING (sec 5.6 / Table B-2's "the sub-code consists of
 * bit flags indicating the reasons why the unit is write protected"). Returns 0
 * when the unit is writable, or the composed ST.WPR status word naming EVERY
 * reason that holds -- 0x1006 for the software protection this host itself
 * asked for, 0x2006 for the volume's own read-only state, 0x3006 when both.
 */
static uint16_t srv_write_protect_status(const struct mscp_srv_fsm *f,
					 const struct mscp_srv_hqb *h,
					 int slot)
{
	const struct mscp_srv_unit_info *u = &f->uqb[slot].info;
	uint16_t sub = 0u;
	uint16_t flags = (uint16_t)(h->host_unfl[slot] | u->unit_flags);

	if ((flags & VMS_MSCP_UF_WRITE_PROT_SW) != 0u)
		sub = (uint16_t)(sub + VMS_MSCP_SUB_WP_SOFTWARE);
	if (u->write_protect || (flags & VMS_MSCP_UF_WRITE_PROT_HW) != 0u)
		sub = (uint16_t)(sub + VMS_MSCP_SUB_WP_HARDWARE);
	if (sub == 0u)
		return 0u;
	return VMS_MSCP_STATUS(VMS_MSCP_ST_WRITE_PROT, sub);
}

/* Copy the 12-byte Table A-6 buffer descriptor out of a parsed transfer
 * command. THE ONLY producer of a remote buffer name in this file. */
static void srv_read_bufdesc(const struct vms_mscp_xfer_cmd *c,
			     struct mscp_srv_bufdesc *out)
{
	out->offset = (uint32_t)c->buffer_desc[0] |
		      ((uint32_t)c->buffer_desc[1] << 8) |
		      ((uint32_t)c->buffer_desc[2] << 16) |
		      ((uint32_t)c->buffer_desc[3] << 24);
	out->name = (uint32_t)c->buffer_desc[4] |
		    ((uint32_t)c->buffer_desc[5] << 8) |
		    ((uint32_t)c->buffer_desc[6] << 16) |
		    ((uint32_t)c->buffer_desc[7] << 24);
	out->conid = (uint32_t)c->buffer_desc[8] |
		     ((uint32_t)c->buffer_desc[9] << 8) |
		     ((uint32_t)c->buffer_desc[10] << 16) |
		     ((uint32_t)c->buffer_desc[11] << 24);
}

/* The status this server answers when it could not even START the local I/O.
 * Table B-1 ST.CNT: the CONTROLLER failed -- not the drive (nothing was asked
 * of it) and not the host (its buffer is fine). No sub-code is claimed, because
 * this project has none grounded for "the server's own I/O queue is full". */
#define MSCP_SRV_STATUS_NO_IO \
	VMS_MSCP_STATUS(VMS_MSCP_ST_CTLR_ERR, 0u)

/*
 * HAND one HRB's block transfer to the served-I/O worker and put the HRB in
 * WAIT_IO (design §3.2.6: the cluster fork thread never calls exec_blockdev_*).
 *
 * Returns 0 when the worker TOOK it -- and from that moment until
 * mscp_srv_fsm_io_done() this file must not touch the staging slot, free the
 * HRB or answer the command. Non-zero means nothing was handed over and the
 * caller may end the request itself.
 */
static int srv_io_submit(struct mscp_srv_fsm *f, struct mscp_srv_hrb *r,
			 enum mscp_srv_io_op op, uint32_t nblocks)
{
	struct mscp_srv_io_req req;
	uint8_t *stage = srv_hrb_buf(f, r);
	uint32_t tag;

	if (f->ops == (const struct mscp_srv_ops *)0 ||
	    f->ops->io_submit == (int (*)(void *,
					  const struct mscp_srv_io_req *))0 ||
	    stage == (uint8_t *)0) {
		f->io_refused++;
		return -1;
	}
	tag = srv_next_io_tag(f);
	if (tag == 0u) {
		f->io_refused++;
		return -1;
	}

	srv_bzero(&req, (uint32_t)sizeof(req));
	req.tag = tag;
	req.op = (uint8_t)op;
	req.unit = f->uqb[r->uqb].info.unit;   /* the EXECUTIVE's own number */
	req.lbn = r->lbn;
	req.nblocks = nblocks;
	req.buf = stage;

	/*
	 * THE HRB IS PUBLISHED AS WAITING **BEFORE** THE HAND-OVER, not after.
	 * The moment the request is visible to the worker its completion may be
	 * on its way back, and a completion that arrives while this HRB still
	 * says "no I/O outstanding" would be counted stale and the host's
	 * command would hang forever. (In production the fork thread is inside
	 * this very call so the race cannot be observed; at the R1 rung a fake
	 * worker completes INLINE, which is exactly the ordering this makes
	 * safe.) On a refusal nothing was handed over, so the publication is
	 * undone exactly.
	 */
	f->io_submitted++;
	r->io_tag = tag;
	r->state = (uint8_t)MSCP_SRV_REQ_WAIT_IO;
	if (f->ops->io_submit(f->ops->ctx, &req) == 0)
		return 0;

	f->io_submitted--;
	f->io_refused++;
	r->io_tag = 0u;
	r->state = (uint8_t)MSCP_SRV_REQ_WAIT_DATA;
	return -1;
}

/* ==========================================================================
 * 7c. READ
 * ========================================================================== */

/*
 * THE SHARED PREAMBLE of both transfer commands: validate against real state,
 * apply write protection (WRITE only -- sec 6.18 lists Write Protected among
 * WRITE's status codes and sec 6.14 does not list it among READ's), read the
 * host's buffer descriptor, and take an HRB with its OWN staging slot.
 *
 * ON ANY REFUSAL IT SENDS THE REAL END MESSAGE ITSELF and returns NULL, so
 * neither handler carries five near-identical refusal blocks and no refusal
 * path can be forgotten. On success the HRB is fully filled from the command
 * and *out_uqb / *out_stage name the unit slot and the staging slot.
 */
static struct mscp_srv_hrb *srv_xfer_begin(struct mscp_srv_fsm *f,
					   struct mscp_srv_hqb *h,
					   const struct vms_mscp_xfer_cmd *c,
					   uint8_t opcode, int *out_uqb,
					   uint8_t **out_stage)
{
	struct mscp_srv_bufdesc desc;
	struct mscp_srv_hrb *r;
	uint8_t *stage;
	uint16_t status = 0u;
	int slot = srv_xfer_gate(f, h, c, &status);

	if (slot < 0)
		goto refuse;

	if (opcode == VMS_MSCP_OP_WRITE) {
		/* Before a byte moves and before a buffer is named. */
		status = srv_write_protect_status(f, h, slot);
		if (status != 0u) {
			f->write_protect_refusals++;
			goto refuse;
		}
	}

	srv_read_bufdesc(c, &desc);
	if (desc.name == 0u) {
		/* The host named no buffer: there is nowhere for the data to
		 * come from or go to, and this server will not invent a name
		 * (INV-6). Table B-2's Host Buffer Access Error is exactly it. */
		status = VMS_MSCP_STATUS(VMS_MSCP_ST_HOST_BUF_ERR, 0u);
		goto refuse;
	}

	/* The HRB, and with it the EXCLUSIVE staging slot: no slot, no
	 * transfer -- refused, never staged over another request's data. */
	r = srv_hrb_alloc(f);
	stage = (r != (struct mscp_srv_hrb *)0) ? srv_hrb_buf(f, r)
						: (uint8_t *)0;
	if (stage == (uint8_t *)0) {
		if (r != (struct mscp_srv_hrb *)0)
			srv_hrb_free(f, r);
		f->reqs_refused_busy++;
		status = VMS_MSCP_STATUS(VMS_MSCP_ST_CTLR_ERR,
					 VMS_MSCP_SUB_CNT_INCONSISTENT);
		goto refuse;
	}

	r->opcode = opcode;
	r->hqb = (uint8_t)srv_hqb_index(f, h);
	r->uqb = (uint8_t)slot;
	r->cmd_ref = c->hdr.cmd_ref;
	r->unit = c->hdr.unit;
	r->lbn = c->lbn;
	r->byte_count = c->byte_count;
	r->desc = desc;
	*out_uqb = slot;
	*out_stage = stage;
	return r;

refuse:
	(void)srv_send_xfer_end(f, h, opcode, c->hdr.cmd_ref, c->hdr.unit,
				status, 0u);
	return (struct mscp_srv_hrb *)0;
}

/* Parse a transfer command and check it really is the opcode this handler
 * owns. Returns 0 on success. */
static int srv_xfer_parse(struct mscp_srv_fsm *f, const struct srv_ev *e,
			  uint8_t opcode, struct vms_mscp_xfer_cmd *out)
{
	if (vms_mscp_xfer_cmd_parse(e->frame, e->frame_len, out) !=
		    VMS_CODEC_OK ||
	    (out->hdr.opcode & VMS_MSCP_OPCODE_MASK) != opcode) {
		f->cmds_unparsed++;
		return -1;
	}
	return 0;
}

/*
 * READ's command half: gate it, then HAND the disk read to the worker. No byte
 * is read here and no end message is sent here -- the answer is built when the
 * worker reports back (srv_read_io_done). A READ therefore spans two fork-thread
 * dispatches, and the HRB is what carries it across.
 */
static void h_cmd_read(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct vms_mscp_xfer_cmd c;
	struct mscp_srv_hqb *h = e->hqb;
	struct mscp_srv_hrb *r;
	uint8_t *stage = (uint8_t *)0;
	int slot = -1;

	if (srv_xfer_parse(f, e, VMS_MSCP_OP_READ, &c) != 0)
		return;
	r = srv_xfer_begin(f, h, &c, VMS_MSCP_OP_READ, &slot, &stage);
	if (r == (struct mscp_srv_hrb *)0)
		return;   /* refused, with a real end message already sent */

	if (srv_io_submit(f, r, MSCP_SRV_IO_READ,
			  c.byte_count / MSCP_SRV_BLOCK_SIZE) != 0) {
		/* Nothing was handed over, so the HRB and its slot are still
		 * ours to end -- with a REAL controller error, never a success
		 * this server cannot back up. */
		srv_hrb_free(f, r);
		(void)srv_send_xfer_end(f, h, VMS_MSCP_OP_READ, c.hdr.cmd_ref,
					c.hdr.unit, MSCP_SRV_STATUS_NO_IO, 0u);
	}
}

/*
 * READ's answer half, on the worker's completion. The bytes are in the HRB's
 * staging slot -- which the worker has just handed back -- and the end message
 * rides the FINAL block-transfer frame (FC-P6.1's TRAP 1), so it is built here
 * and given to the transfer.
 */
static void srv_read_io_done(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			     struct mscp_srv_hrb *r)
{
	uint8_t *stage = srv_hrb_buf(f, r);
	uint32_t end_len;

	end_len = srv_build_xfer_end(f, VMS_MSCP_OP_READ, r->cmd_ref, r->unit,
				     VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
						     VMS_MSCP_SUB_NORMAL),
				     r->byte_count);
	if (end_len == 0u) {
		srv_hrb_free(f, r);
		return;
	}

	if (stage == (uint8_t *)0 ||
	    f->ops->send_read_data == (int (*)(void *, vms_conid_t,
					       vms_scs_sysid_t,
					       const struct mscp_srv_bufdesc *,
					       const uint8_t *, uint32_t,
					       const uint8_t *, uint32_t))0 ||
	    f->ops->send_read_data(f->ops->ctx, h->conid, h->peer, &r->desc,
				   stage, r->byte_count,
				   f->endframe + VMS_OFF_SYSAP_BODY,
				   end_len) != 0) {
		f->xfer_refused++;
		srv_hrb_free(f, r);
		(void)srv_send_xfer_end(f, h, VMS_MSCP_OP_READ, r->cmd_ref,
					r->unit,
					VMS_MSCP_STATUS(VMS_MSCP_ST_CTLR_ERR,
							VMS_MSCP_SUB_CNT_INCONSISTENT),
					0u);
		return;
	}
	f->ends_tx++;        /* the end message went out ON the transfer */
	f->reads_served++;
	srv_hrb_free(f, r);
}

/* ==========================================================================
 * 7d. WRITE
 * ========================================================================== */

/* Withdraw the buffer name this node minted for a WRITE. Done the moment the
 * peer's transfer is over -- a name that outlives its transfer is a buffer a
 * stale frame could still land in, and once the request is handed to the worker
 * that buffer is being READ by another thread. */
static void srv_release_name(struct mscp_srv_fsm *f, struct mscp_srv_hrb *r)
{
	if (r->local_name == 0u)
		return;
	if (f->ops != (const struct mscp_srv_ops *)0 &&
	    f->ops->release_buffer != (void (*)(void *, uint32_t))0)
		f->ops->release_buffer(f->ops->ctx, r->local_name);
	r->local_name = 0u;
}

/*
 * Name the HRB's staging slot as a DESTINATION the host's port may fill, and
 * record the name OUR port minted. Returns 0, or non-zero when no buffer could
 * be named -- and then nothing has been asked of the host.
 */
static int srv_write_name_buffer(struct mscp_srv_fsm *f,
				 struct mscp_srv_hqb *h, struct mscp_srv_hrb *r,
				 uint8_t *stage, uint32_t len)
{
	uint32_t name = 0u;

	if (f->ops->recv_write_data == (int (*)(void *, vms_conid_t,
						vms_scs_sysid_t,
						const struct mscp_srv_bufdesc *,
						uint8_t *, uint32_t,
						uint32_t *))0)
		return -1;
	if (f->ops->recv_write_data(f->ops->ctx, h->conid, h->peer, &r->desc,
				    stage, len, &name) != 0 || name == 0u)
		return -1;
	r->local_name = name;
	return 0;
}

/*
 * ISSUE THE REQUEST DATA (design §3.2.6's E41 ruling). The host's buffer is the
 * SOURCE and it comes straight out of `r->desc`, which was read off the host's
 * own command; the destination is the name our port just minted. Only this side
 * can make this call, because only this side knows both names.
 */
static int srv_write_request_data(struct mscp_srv_fsm *f,
				  struct mscp_srv_hqb *h,
				  struct mscp_srv_hrb *r)
{
	if (f->ops->request_write_data == (int (*)(void *, vms_conid_t,
						   vms_scs_sysid_t,
						   const struct mscp_srv_bufdesc *,
						   uint32_t, uint32_t))0 ||
	    f->ops->request_write_data(f->ops->ctx, h->conid, h->peer, &r->desc,
				       r->local_name, r->byte_count) != 0) {
		f->write_requests_refused++;
		return -1;
	}
	f->write_requests_issued++;
	return 0;
}

/*
 * A WRITE that could not be set up. The name is withdrawn, the HRB and its
 * staging slot go back, and the host gets a REAL controller error -- never an
 * HRB left waiting for a transfer nobody was asked to make.
 */
static void srv_write_refuse(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			     struct mscp_srv_hrb *r)
{
	uint32_t cmd_ref = r->cmd_ref;
	uint16_t unit = r->unit;

	srv_release_name(f, r);
	srv_hrb_free(f, r);
	f->xfer_refused++;
	(void)srv_send_xfer_end(f, h, VMS_MSCP_OP_WRITE, cmd_ref, unit,
				VMS_MSCP_STATUS(VMS_MSCP_ST_CTLR_ERR,
						VMS_MSCP_SUB_CNT_INCONSISTENT),
				0u);
}

/*
 * WRITE's command half: gate it, name the landing buffer, and ASK the host for
 * its bytes. No end message is sent here -- the data has not arrived, and
 * answering now would be the fabricated success this whole layer exists not to
 * emit.
 */
static void h_cmd_write(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct vms_mscp_xfer_cmd c;
	struct mscp_srv_hqb *h = e->hqb;
	struct mscp_srv_hrb *r;
	uint8_t *stage = (uint8_t *)0;
	int slot = -1;

	if (srv_xfer_parse(f, e, VMS_MSCP_OP_WRITE, &c) != 0)
		return;
	/* srv_xfer_begin applies the WRITE-PROTECT answer for us, before a byte
	 * moves and before a buffer is named. */
	r = srv_xfer_begin(f, h, &c, VMS_MSCP_OP_WRITE, &slot, &stage);
	if (r == (struct mscp_srv_hrb *)0)
		return;

	if (srv_write_name_buffer(f, h, r, stage, c.byte_count) != 0 ||
	    srv_write_request_data(f, h, r) != 0)
		srv_write_refuse(f, h, r);
}

/*
 * Every opcode this server does not implement. sec 5.5: an unimplemented
 * command is answered Invalid Command -- ANSWERED, so the host's command
 * completes, never dropped. The endcode carried is the REAL one for the
 * opcode that arrived (`opcode | OP.END`), through the codec's generic end
 * builder: answering with some other class's endcode would assert that a
 * different command was the one being refused.
 */
static void h_cmd_other(struct mscp_srv_fsm *f, struct srv_ev *e)
{
	struct vms_mscp_end_hdr eh;
	struct vms_mscp_hdr cmd;
	struct mscp_srv_hqb *h = e->hqb;
	uint32_t body_len = 0u;

	/* sec 5.1's generic header: P.CRF and P.UNIT to echo, and the opcode
	 * whose OWN endcode this refusal must carry. */
	if (vms_mscp_hdr_read(e->frame, e->frame_len, &cmd) != VMS_CODEC_OK) {
		f->cmds_unparsed++;
		return;
	}
	/* An END-bit opcode is a message a SERVER never receives; there is no
	 * command to complete, so nothing is answered. Counted, not invented. */
	if ((cmd.opcode & VMS_MSCP_END_BIT) != 0u) {
		h->invalid_cmds++;
		return;
	}

	h->invalid_cmds++;
	srv_end_hdr(&eh, cmd.cmd_ref, cmd.unit,
		    VMS_MSCP_STATUS(VMS_MSCP_ST_INVALID_CMD, 0u));
	srv_bzero(f->endframe, (uint32_t)sizeof(f->endframe));
	if (vms_mscp_generic_end_build(&eh, cmd.opcode, f->endframe,
				       (uint32_t)sizeof(f->endframe),
				       &body_len) != VMS_CODEC_OK) {
		f->end_tx_failed++;
		return;
	}
	(void)srv_emit(f, h->conid, body_len);
}

/* ==========================================================================
 * 8. THE TABLE
 *
 * handlers[state][event]. A NULL cell is an event that state has no edge for:
 * every MSCP command arriving at a Controller-AVAILABLE controller falls in one
 * (sec 4.1 -- there is no connection, so there is no controller to command),
 * and it is counted in `ignored_events`, never serviced.
 * ========================================================================== */
static const srv_handler_t srv_table[MSCP_SRV_ST__COUNT][MSCP_SRV_EV__COUNT] = {
	/* [MSCP_SRV_ST_AVAILABLE] */
	{
		h_conn_open,   /* CONN_OPEN  -> sec 4.1, the only edge out    */
		h_conn_close,  /* CONN_CLOSE -> tear a half-built HQB down    */
		(srv_handler_t)0,  /* CMD_SCC    -- no controller to command  */
		(srv_handler_t)0,  /* CMD_GUS                                 */
		(srv_handler_t)0,  /* CMD_ONLINE                              */
		(srv_handler_t)0,  /* CMD_READ                                */
		(srv_handler_t)0,  /* CMD_WRITE                               */
		(srv_handler_t)0   /* CMD_OTHER                               */
	},
	/* [MSCP_SRV_ST_ONLINE] */
	{
		h_conn_open,   /* a re-open on the same Con.ID: idempotent    */
		h_conn_close,
		h_cmd_scc,
		h_cmd_gus,
		h_cmd_online,
		h_cmd_read,
		h_cmd_write,
		h_cmd_other
	}
};

static void srv_dispatch(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			 enum mscp_srv_event ev, struct srv_ev *e)
{
	enum mscp_srv_state st = h != (struct mscp_srv_hqb *)0
					 ? (enum mscp_srv_state)h->state
					 : MSCP_SRV_ST_AVAILABLE;
	srv_handler_t fn;

	if ((unsigned)st >= (unsigned)MSCP_SRV_ST__COUNT ||
	    (unsigned)ev >= (unsigned)MSCP_SRV_EV__COUNT) {
		f->ignored_events++;
		return;
	}
	fn = srv_table[st][ev];
	if (fn == (srv_handler_t)0) {
		f->ignored_events++;
		return;
	}
	e->hqb = h;
	fn(f, e);
}

/* ==========================================================================
 * 9. Entry points
 * ========================================================================== */

void mscp_srv_fsm_init(struct mscp_srv_fsm *f, const struct mscp_srv_ops *ops)
{
	if (f == (struct mscp_srv_fsm *)0)
		return;
	srv_bzero(f, (uint32_t)sizeof(*f));
	f->ops = ops;
}

void mscp_srv_fsm_set_ctlr_id(struct mscp_srv_fsm *f, uint64_t ctlr_id)
{
	if (f != (struct mscp_srv_fsm *)0)
		f->ctlr_id = ctlr_id;
}

void mscp_srv_fsm_bind_xferbuf(struct mscp_srv_fsm *f, uint8_t *buf,
			       uint32_t len)
{
	uint32_t slot;

	if (f == (struct mscp_srv_fsm *)0)
		return;
	slot = (buf != (uint8_t *)0) ? (len / MSCP_SRV_MAX_REQS) : 0u;
	if (slot == 0u) {
		/* Too small to give every HRB a slot: bind NOTHING rather than
		 * hand out overlapping slices. */
		f->xferbuf = (uint8_t *)0;
		f->xferbuf_len = 0u;
		f->xferbuf_slot = 0u;
		return;
	}
	f->xferbuf = buf;
	f->xferbuf_len = slot * MSCP_SRV_MAX_REQS;
	f->xferbuf_slot = slot;
}

void mscp_srv_fsm_conn_open(struct mscp_srv_fsm *f, vms_conid_t conid,
			    vms_scs_sysid_t peer)
{
	struct srv_ev e;

	if (f == (struct mscp_srv_fsm *)0)
		return;
	srv_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.peer = peer;
	srv_dispatch(f, srv_hqb_by_conid(f, conid), MSCP_SRV_EV_CONN_OPEN, &e);
}

void mscp_srv_fsm_conn_closed(struct mscp_srv_fsm *f, vms_conid_t conid)
{
	struct srv_ev e;
	struct mscp_srv_hqb *h;

	if (f == (struct mscp_srv_fsm *)0)
		return;
	h = srv_hqb_by_conid(f, conid);
	if (h == (struct mscp_srv_hqb *)0)
		return;   /* a Con.ID we never had an HQB for: nothing to undo */
	srv_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.peer = h->peer;
	srv_dispatch(f, h, MSCP_SRV_EV_CONN_CLOSE, &e);
}

/* The opcode -> event map. The five this item's captures confirm a real class
 * driver sends; everything else is CMD_OTHER, which is answered, not dropped. */
static enum mscp_srv_event srv_event_for_opcode(uint8_t opcode)
{
	if ((opcode & VMS_MSCP_END_BIT) != 0u)
		return MSCP_SRV_EV_CMD_OTHER;  /* a server receives COMMANDS */
	switch (opcode & VMS_MSCP_OPCODE_MASK) {
	case VMS_MSCP_OP_SCC:    return MSCP_SRV_EV_CMD_SCC;
	case VMS_MSCP_OP_GUS:    return MSCP_SRV_EV_CMD_GUS;
	case VMS_MSCP_OP_ONLINE: return MSCP_SRV_EV_CMD_ONLINE;
	case VMS_MSCP_OP_READ:   return MSCP_SRV_EV_CMD_READ;
	case VMS_MSCP_OP_WRITE:  return MSCP_SRV_EV_CMD_WRITE;
	default:                 return MSCP_SRV_EV_CMD_OTHER;
	}
}

int mscp_srv_fsm_command(struct mscp_srv_fsm *f, vms_conid_t conid,
			 const uint8_t *body, uint32_t len)
{
	struct mscp_srv_hqb *h;
	struct srv_ev e;
	uint8_t opcode = 0u;
	uint32_t n;

	if (f == (struct mscp_srv_fsm *)0 || body == (const uint8_t *)0)
		return -1;
	if (len < VMS_MSCP_CMD_BODY_LEN) {
		f->cmds_unparsed++;
		return -1;
	}

	h = srv_hqb_by_conid(f, conid);
	if (h == (struct mscp_srv_hqb *)0) {
		/* sec 4.1: no connection, no Controller-Online -- and this
		 * server will not answer for a controller it is not. Counted. */
		f->cmds_no_hqb++;
		return -1;
	}

	/* THE SPLICE (see the header): the codec addresses an MSCP message
	 * frame-absolutely, the SYSAP was handed only its body. */
	n = len < VMS_MSCP_CMD_BODY_LEN ? len : VMS_MSCP_CMD_BODY_LEN;
	srv_bzero(f->cmdframe, (uint32_t)sizeof(f->cmdframe));
	srv_copy(f->cmdframe + VMS_OFF_SYSAP_BODY, body, n);

	if (vms_mscp_read_opcode(f->cmdframe, (uint32_t)sizeof(f->cmdframe),
				 &opcode) != VMS_CODEC_OK) {
		f->cmds_unparsed++;
		return -1;
	}

	f->cmds_rx++;
	h->cmds_rx++;
	srv_bzero(&e, (uint32_t)sizeof(e));
	e.conid = conid;
	e.peer = h->peer;
	e.frame = f->cmdframe;
	e.frame_len = (uint32_t)sizeof(f->cmdframe);
	srv_dispatch(f, h, srv_event_for_opcode(opcode), &e);
	return 0;
}

/* ==========================================================================
 * 10. Block-transfer completion (the WRITE half's data)
 * ========================================================================== */

/* The HQB an HRB belongs to, or NULL if it has gone away underneath it. */
static struct mscp_srv_hqb *srv_hrb_hqb(struct mscp_srv_fsm *f,
					const struct mscp_srv_hrb *r)
{
	if (r->hqb >= MSCP_SRV_MAX_HOSTS || !f->hqb[r->hqb].in_use)
		return (struct mscp_srv_hqb *)0;
	return &f->hqb[r->hqb];
}

/*
 * The peer's WRITE data has all arrived: HAND the commit to the served-I/O
 * worker (design §3.2.6 -- the fork thread never calls exec_blockdev_*). The
 * end message is built when the worker reports back (srv_write_io_done).
 *
 * ONLY WHOLE BLOCKS ARE COMMITTED, and only what really arrived. sec 5.3 makes
 * a transfer a whole number of blocks, so a completion carrying less than one
 * is a transfer that did not deliver its data: it is answered Host Buffer
 * Access Error (Table B-1 ST.HST -- the host's buffer did not yield the bytes),
 * NOT a zero-length success, and no I/O is started at all.
 */
static void srv_write_data_complete(struct mscp_srv_fsm *f,
				    struct mscp_srv_hrb *r)
{
	struct mscp_srv_hqb *h = srv_hrb_hqb(f, r);
	uint32_t blocks = r->received / MSCP_SRV_BLOCK_SIZE;

	if (h == (struct mscp_srv_hqb *)0) {
		srv_hrb_free(f, r);
		return;
	}
	srv_release_name(f, r);

	if (blocks == 0u) {
		(void)srv_send_xfer_end(f, h, VMS_MSCP_OP_WRITE, r->cmd_ref,
					r->unit,
					VMS_MSCP_STATUS(VMS_MSCP_ST_HOST_BUF_ERR,
							0u),
					0u);
		srv_hrb_free(f, r);
		return;
	}

	if (srv_io_submit(f, r, MSCP_SRV_IO_WRITE, blocks) != 0) {
		(void)srv_send_xfer_end(f, h, VMS_MSCP_OP_WRITE, r->cmd_ref,
					r->unit, MSCP_SRV_STATUS_NO_IO, 0u);
		srv_hrb_free(f, r);
	}
}

/* WRITE's answer half, on the worker's completion. The byte count reported is
 * the number of bytes ACTUALLY written, never the number the command asked
 * for. */
static void srv_write_io_done(struct mscp_srv_fsm *f, struct mscp_srv_hqb *h,
			      struct mscp_srv_hrb *r)
{
	uint32_t moved = (r->received / MSCP_SRV_BLOCK_SIZE) *
			 MSCP_SRV_BLOCK_SIZE;

	f->writes_served++;
	(void)srv_send_xfer_end(f, h, VMS_MSCP_OP_WRITE, r->cmd_ref, r->unit,
				VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
						VMS_MSCP_SUB_NORMAL),
				moved);
	srv_hrb_free(f, r);
}

/*
 * THE WORKER'S ANSWER (FC-P6.6). Runs on the fork thread; see the header's
 * contract. Its first job is to take the staging slot back -- clearing io_tag
 * so no second completion can match this request -- and only then to decide
 * what the host is told.
 */
void mscp_srv_fsm_io_done(struct mscp_srv_fsm *f, uint32_t tag, uint32_t status)
{
	struct mscp_srv_hrb *r;
	struct mscp_srv_hqb *h;

	if (f == (struct mscp_srv_fsm *)0)
		return;
	r = srv_hrb_by_io_tag(f, tag);
	if (r == (struct mscp_srv_hrb *)0) {
		/* An answer to a question nobody is still asking. Counted and
		 * dropped -- never applied to whichever request happens to
		 * occupy that slot now. */
		f->io_done_stale++;
		return;
	}
	r->io_tag = 0u;
	r->state = (uint8_t)MSCP_SRV_REQ_WAIT_DATA;   /* the slot is ours again */
	if (status == 0u)
		f->io_done_ok++;
	else
		f->io_done_failed++;

	if (r->abandoned) {
		/* The host's connection went while the worker held the slot.
		 * sec 4.1 leaves no command to complete, so it ends in silence
		 * -- but only NOW, when the slot is genuinely back. */
		f->reqs_abandoned++;
		srv_hrb_free(f, r);
		return;
	}

	h = srv_hrb_hqb(f, r);
	if (h == (struct mscp_srv_hqb *)0) {
		srv_hrb_free(f, r);
		return;
	}

	if (status != 0u) {
		/* Table B-1 ST.DRV: the drive could not move the data. NOT a
		 * success with a zero byte count, which is the dishonest shape
		 * INV-6 exists to stop. */
		f->blockdev_failures++;
		(void)srv_send_xfer_end(f, h, r->opcode, r->cmd_ref, r->unit,
					VMS_MSCP_STATUS(VMS_MSCP_ST_DRIVE_ERR,
							0u),
					0u);
		srv_hrb_free(f, r);
		return;
	}

	if (r->opcode == VMS_MSCP_OP_READ)
		srv_read_io_done(f, h, r);
	else
		srv_write_io_done(f, h, r);
}

void mscp_srv_fsm_block_data(struct mscp_srv_fsm *f, uint32_t name,
			     uint32_t offset, uint32_t len,
			     uint32_t bytes_remaining)
{
	struct mscp_srv_hrb *r;

	(void)offset;
	if (f == (struct mscp_srv_fsm *)0)
		return;
	r = srv_hrb_by_name(f, name);
	if (r == (struct mscp_srv_hrb *)0)
		return;   /* not one of ours: the port already counted it */

	r->received += len;
	if (r->received > r->byte_count)
		r->received = r->byte_count;

	/* vms_pe.h: `bytes_remaining` INCLUDES this frame's data, so the
	 * transfer is complete exactly when it has come down to this frame's
	 * own length. Read from the wire, never inferred from a local count. */
	if (bytes_remaining > len)
		return;
	srv_write_data_complete(f, r);
}

/* ==========================================================================
 * 12. The served unit's NAME and NUMBER (see the header SS12)
 * ========================================================================== */

/* Append `v` in decimal. Returns the new length. */
static uint32_t srv_put_u32(char *out, uint32_t outsz, uint32_t n, uint32_t v)
{
	char digits[10];
	uint32_t d = 0;

	do {
		digits[d++] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v != 0u && d < sizeof(digits));
	while (d > 0u && n + 1u < outsz)
		out[n++] = digits[--d];
	return n;
}

void vms_mscp_srv_unit_name(uint8_t alloclass, uint16_t unit, char *out,
			    uint32_t outsz)
{
	uint32_t n = 0;

	if (out == (char *)0 || outsz == 0u)
		return;
	if (outsz > 1u)
		out[n++] = '$';
	n = srv_put_u32(out, outsz, n, (uint32_t)alloclass);
	if (n + 1u < outsz)
		out[n++] = '$';
	if (n + 1u < outsz)
		out[n++] = 'D';
	if (n + 1u < outsz)
		out[n++] = 'U';
	if (n + 1u < outsz)
		out[n++] = 'A';
	n = srv_put_u32(out, outsz, n, (uint32_t)unit);
	if (n + 1u < outsz)
		out[n++] = ':';
	out[n] = '\0';
}

int vms_mscp_srv_unit_from_devnam(const char *devnam, uint16_t *out)
{
	uint32_t i, v = 0u;
	int seen = 0;

	if (devnam == (const char *)0 || out == (uint16_t *)0)
		return -1;
	/* The trailing decimal run, before the optional colon. A name with no
	 * digits at all (or a number past a unit word) yields NO unit, and the
	 * volume is then not served -- never served under a made-up number. */
	for (i = 0; devnam[i] != '\0' && i < MSCP_SRV_DEVNAM_SCAN_MAX; i++) {
		char c = devnam[i];

		if (c >= '0' && c <= '9') {
			v = (v * 10u) + (uint32_t)(c - '0');
			if (v > 0xffffu)
				return -1;
			seen = 1;
		} else if (c == ':') {
			break;
		} else if (seen) {
			return -1;   /* digits then letters again: not a unit */
		}
	}
	if (!seen)
		return -1;
	*out = (uint16_t)v;
	return 0;
}

/* ==========================================================================
 * 11. The beat -- reap a request whose data never came
 * ========================================================================== */

static void srv_abort_hrb(struct mscp_srv_fsm *f, struct mscp_srv_hrb *r)
{
	struct mscp_srv_hqb *h = srv_hrb_hqb(f, r);

	if (h != (struct mscp_srv_hqb *)0)
		(void)srv_send_xfer_end(f, h, r->opcode, r->cmd_ref, r->unit,
					VMS_MSCP_STATUS(VMS_MSCP_ST_ABORTED, 0u),
					0u);
	f->reqs_aborted++;
	srv_hrb_free(f, r);
}

/*
 * A request past its deadline that the WORKER still owns cannot be reaped: its
 * staging slot is in another thread's hands. The lateness is recorded ONCE and
 * the local transfer is left to complete on its own, which is what a VMS server
 * does with an IRP outstanding to a local driver (see the header's note on
 * `abort_pending`). Nothing is answered here, and nothing is freed.
 */
static void srv_defer_abort(struct mscp_srv_fsm *f, struct mscp_srv_hrb *r)
{
	if (r->abort_pending)
		return;
	r->abort_pending = 1u;
	f->reqs_abort_deferred++;
}

uint32_t mscp_srv_fsm_tick(struct mscp_srv_fsm *f)
{
	uint32_t i, now, reaped = 0u;

	if (f == (struct mscp_srv_fsm *)0)
		return 0u;
	now = srv_now(f);
	for (i = 0; i < MSCP_SRV_MAX_REQS; i++) {
		if (!f->hrb[i].in_use)
			continue;
		if ((uint32_t)(now - f->hrb[i].started_ms) <
		    MSCP_SRV_REQ_TIMEOUT_MS)
			continue;
		if (f->hrb[i].state == (uint8_t)MSCP_SRV_REQ_WAIT_IO) {
			srv_defer_abort(f, &f->hrb[i]);
			continue;
		}
		srv_abort_hrb(f, &f->hrb[i]);
		reaped++;
	}
	return reaped;
}
