// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_mscp.c - MSCP-over-SCS typed codec entries (FC-P6.2).
 *
 * Read vms_cluster_codec_mscp.h first: it carries the field map, the
 * measured-length grounding, the P.UNFL echo rule and the self-contained
 * classification rationale. Pure, like every sibling family file: no
 * state, no allocation, no substrate call, no libc -- every byte move goes
 * through the parent TU's vms_wire_get_*, vms_wire_put_* and
 * vms_wire_put_zero primitives.
 */

#include "vms_cluster_codec_mscp.h"

/* ------------------------------------------------------------------ *
 * sec 3  Status split + the P.UNFL echo rule
 * ------------------------------------------------------------------ */

unsigned vms_mscp_status_major(uint16_t status)
{
	return (unsigned)(status & VMS_MSCP_ST_MASK);
}

unsigned vms_mscp_status_subcode(uint16_t status)
{
	return (unsigned)(status >> VMS_MSCP_ST_SUB_SHIFT);
}

uint16_t vms_mscp_online_unfl_compose(uint16_t host_unfl, uint16_t unit_flags)
{
	return (uint16_t)(host_unfl | unit_flags);
}

/* ------------------------------------------------------------------ *
 * sec 4  Self-contained classification
 * ------------------------------------------------------------------ */

/* The registry's own confirmation that this is a valid format-0x13
 * sequenced-application frame -- necessary, not sufficient (see file
 * header "SELF-CONTAINED CLASSIFICATION").
 *
 * FC-P2.1b (spec §4(h)(1b), vms-54f) grounded a dedicated CONID-capable
 * class for the 94-content op-10 shape (VMS_FCLS_SCS_APPLMSG94), which
 * every MSCP command and WRITE-END frame occupies (94 == P.CRF..P.reserved
 * body span, see VMS_MSCP_CMD_SCA_LEN/VMS_MSCP_END_SCA_LEN(WRITE)) -- so
 * those frames now classify there instead of falling through to the
 * VMS_FCLS_SCS_SEQ catch-all. The other four MSCP end-message lengths
 * (SCC/GUS/ONLINE/READ END: 86/110/102/90) are NOT 94 and still land on
 * VMS_FCLS_SCS_SEQ exactly as before, so both classes are accepted here --
 * this file still resolves the SPECIFIC MSCP class itself from length and
 * the opcode's END bit, self-sufficient either way. */
static int mscp_seq_ok(const struct vms_frame_info *fi)
{
	return fi != (const struct vms_frame_info *)0 &&
	       fi->family == VMS_FFAM_SCS &&
	       (fi->cls == VMS_FCLS_SCS_SEQ ||
		fi->cls == VMS_FCLS_SCS_APPLMSG94);
}

static vms_codec_status_t mscp_read_opcode(const uint8_t *frame, uint32_t len,
					   uint8_t *opcode_out)
{
	vms_wire_view_t v;
	uint8_t op;

	vms_wire_view_init(&v, frame, len);
	op = vms_wire_get_u8(&v, VMS_OFF_MSCP_OPCD);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*opcode_out = op;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_classify(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     enum vms_mscp_class *out)
{
	uint16_t content;
	/*
	 * `op` is only ever READ below after mscp_read_opcode() returns
	 * VMS_CODEC_OK, which is the only path that writes it -- but the
	 * elf32-vax cross-compiler's -O2 -Wmaybe-uninitialized (unlike every
	 * other arch this tree builds for) cannot see across that call, so it
	 * is zero-initialized here to keep -Werror clean on every arch. No
	 * behaviour change: the early return on a non-OK status is unaffected.
	 */
	uint8_t op = 0u, base;
	int is_end;
	vms_codec_status_t st;

	if (out == (enum vms_mscp_class *)0)
		return VMS_CODEC_E_INVAL;
	*out = VMS_MSCP_CLS_UNKNOWN;
	if (!mscp_seq_ok(fi))
		return VMS_CODEC_OK;   /* not our family: honest non-match */

	content = fi->sca_content;
	st = mscp_read_opcode(frame, len, &op);
	if (st != VMS_CODEC_OK)
		return VMS_CODEC_OK;  /* too short to hold P.OPCD: non-match */

	is_end = (op & VMS_MSCP_END_BIT) != 0;
	base = (uint8_t)(op & VMS_MSCP_OPCODE_MASK);

	if (!is_end) {
		if (content == VMS_MSCP_CMD_SCA_LEN)
			*out = VMS_MSCP_CLS_CMD;
		return VMS_CODEC_OK;
	}

	if (base == VMS_MSCP_OP_SCC && content == VMS_MSCP_END_SCA_LEN(VMS_MSCP_SCC_END_LEN))
		*out = VMS_MSCP_CLS_SCC_END;
	else if (base == VMS_MSCP_OP_GUS && content == VMS_MSCP_END_SCA_LEN(VMS_MSCP_GUS_END_LEN))
		*out = VMS_MSCP_CLS_GUS_END;
	else if (base == VMS_MSCP_OP_ONLINE && content == VMS_MSCP_END_SCA_LEN(VMS_MSCP_ONLINE_END_LEN))
		*out = VMS_MSCP_CLS_ONLINE_END;
	else if (base == VMS_MSCP_OP_READ && content == VMS_MSCP_END_SCA_LEN(VMS_MSCP_READ_END_LEN))
		*out = VMS_MSCP_CLS_READ_END;
	else if (base == VMS_MSCP_OP_WRITE && content == VMS_MSCP_END_SCA_LEN(VMS_MSCP_WRITE_END_LEN))
		*out = VMS_MSCP_CLS_WRITE_END;

	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 5  The minimal abs[0,72) link
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_mscp_link_build(const struct vms_mscp_link *l,
				       uint16_t sca_content_len,
				       uint8_t *frame, uint32_t cap,
				       uint32_t *written)
{
	struct vms_sca_hdr h;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t hdr_written = 0;

	if (l == (const struct vms_mscp_link *)0)
		return VMS_CODEC_E_INVAL;

	h = l->hdr;
	h.sca_len_field = (uint16_t)(sca_content_len - 2u);
	h.word30 = (uint16_t)((uint16_t)VMS_SCS_MT_MSG |
			      ((uint16_t)VMS_SCS_FORMAT_V13 << 8));

	st = vms_sca_hdr_build(&h, frame, cap, &hdr_written);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* Zero the whole counter/mirror span first, then lay down exactly
	 * the fields this item's own sources ground (see header doc
	 * comment): recv_ack + its two mirrors, send_seq + its mirror,
	 * credit, the Con.ID pair. Everything else in [36,72) stays an
	 * honest zero. */
	vms_wire_put_zero(&w, VMS_OFF_SCS_RECV_ACK,
			  VMS_OFF_SYSAP_BODY - VMS_OFF_SCS_RECV_ACK);
	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK, l->recv_ack);      /* abs32 */
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEND_SEQ, l->send_seq);      /* abs34 */
	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK + 8u, l->recv_ack); /* abs40 mirror */
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEND_SEQ + 10u, l->send_seq);/* abs44 mirror */
	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK + 16u, l->recv_ack);/* abs48 mirror */
	/* abs60: the SCS message-TYPE word (content[46:48]) -- GROUNDED
	 * VMS_SCS_CTRL_APPLICATION (10) for every MSCP frame, per
	 * scs_mscp.h's own citation of design-mscp-direction.md sec 1.2
	 * ("the p. 4-13 APPLICATION MESSAGE ... MTYPE 10"); a protocol
	 * constant, baked in exactly as msgtype/format are. */
	vms_wire_put_le16(&w, VMS_OFF_SCS_CTRL_TYPE,
			  (uint16_t)VMS_SCS_CTRL_APPLICATION);
	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK + 30u, l->credit);  /* abs62 */
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, l->remote_conid);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_LOCAL, l->local_conid);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 6  Commands
 * ------------------------------------------------------------------ */

static vms_codec_status_t mscp_hdr_build(vms_wire_buf_t *w,
					 const struct vms_mscp_hdr *h,
					 uint8_t opcode)
{
	vms_wire_put_le32(w, VMS_OFF_MSCP_CRF, h->cmd_ref);
	vms_wire_put_le16(w, VMS_OFF_MSCP_UNIT, h->unit);
	vms_wire_put_zero(w, VMS_OFF_MSCP_RSVD6, 2u);
	vms_wire_put_u8(w, VMS_OFF_MSCP_OPCD, opcode);
	return w->err;
}

static vms_codec_status_t mscp_hdr_parse(vms_wire_view_t *v,
					 struct vms_mscp_hdr *out)
{
	out->cmd_ref = vms_wire_get_le32(v, VMS_OFF_MSCP_CRF);
	out->unit = vms_wire_get_le16(v, VMS_OFF_MSCP_UNIT);
	out->opcode = vms_wire_get_u8(v, VMS_OFF_MSCP_OPCD);
	if (!vms_wire_view_ok(v))
		return v->err;
	return VMS_CODEC_OK;
}

/* ---- SET CONTROLLER CHARACTERISTICS command ---- */

vms_codec_status_t vms_mscp_scc_cmd_build(const struct vms_mscp_scc_cmd *c,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (c == (const struct vms_mscp_scc_cmd *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_CMD_BODY_LEN);
	st = mscp_hdr_build(&w, &c->hdr, VMS_MSCP_OP_SCC);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_MOD, 0u); /* P.MOD reserved on SCC */
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_C_VRSN, c->version);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_C_CNTF, c->ctlr_flags);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_C_HTMO, c->host_timeout);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_SCC_C_TIME, (uint32_t)(c->time & 0xffffffffu));
	vms_wire_put_le32(&w, VMS_OFF_MSCP_SCC_C_TIME + 4u, (uint32_t)(c->time >> 32));

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_CMD_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_scc_cmd_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_scc_cmd *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint32_t lo, hi;

	if (out == (struct vms_mscp_scc_cmd *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_hdr_parse(&v, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;
	if ((out->hdr.opcode & VMS_MSCP_OPCODE_MASK) != VMS_MSCP_OP_SCC ||
	    (out->hdr.opcode & VMS_MSCP_END_BIT) != 0)
		return VMS_CODEC_E_CLASS;

	out->version = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_C_VRSN);
	out->ctlr_flags = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_C_CNTF);
	out->host_timeout = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_C_HTMO);
	lo = vms_wire_get_le32(&v, VMS_OFF_MSCP_SCC_C_TIME);
	hi = vms_wire_get_le32(&v, VMS_OFF_MSCP_SCC_C_TIME + 4u);
	if (!vms_wire_view_ok(&v))
		return v.err;
	out->time = ((uint64_t)hi << 32) | (uint64_t)lo;
	return VMS_CODEC_OK;
}

/* ---- GET UNIT STATUS command ---- */

vms_codec_status_t vms_mscp_gus_cmd_build(const struct vms_mscp_gus_cmd *c,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (c == (const struct vms_mscp_gus_cmd *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_CMD_BODY_LEN);
	st = mscp_hdr_build(&w, &c->hdr, VMS_MSCP_OP_GUS);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_MOD, c->modifiers);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_CMD_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_gus_cmd_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_gus_cmd *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_mscp_gus_cmd *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_hdr_parse(&v, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;
	if ((out->hdr.opcode & VMS_MSCP_OPCODE_MASK) != VMS_MSCP_OP_GUS ||
	    (out->hdr.opcode & VMS_MSCP_END_BIT) != 0)
		return VMS_CODEC_E_CLASS;

	out->modifiers = vms_wire_get_le16(&v, VMS_OFF_MSCP_MOD);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ---- ONLINE command ---- */

vms_codec_status_t vms_mscp_online_cmd_build(const struct vms_mscp_online_cmd *c,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (c == (const struct vms_mscp_online_cmd *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_CMD_BODY_LEN);
	st = mscp_hdr_build(&w, &c->hdr, VMS_MSCP_OP_ONLINE);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_MOD, c->modifiers);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_ONLINE_C_UNFL, c->unit_flags);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_CMD_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_online_cmd_parse(const uint8_t *frame, uint32_t len,
					     struct vms_mscp_online_cmd *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_mscp_online_cmd *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_hdr_parse(&v, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;
	if ((out->hdr.opcode & VMS_MSCP_OPCODE_MASK) != VMS_MSCP_OP_ONLINE ||
	    (out->hdr.opcode & VMS_MSCP_END_BIT) != 0)
		return VMS_CODEC_E_CLASS;

	out->modifiers = vms_wire_get_le16(&v, VMS_OFF_MSCP_MOD);
	out->unit_flags = vms_wire_get_le16(&v, VMS_OFF_MSCP_ONLINE_C_UNFL);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ---- READ / WRITE (transfer) command ---- */

vms_codec_status_t vms_mscp_xfer_cmd_build(const struct vms_mscp_xfer_cmd *c,
					   uint8_t *frame, uint32_t cap,
					   uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint8_t opcode;

	if (c == (const struct vms_mscp_xfer_cmd *)0)
		return VMS_CODEC_E_INVAL;
	opcode = c->hdr.opcode;
	if (opcode != VMS_MSCP_OP_READ && opcode != VMS_MSCP_OP_WRITE)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_CMD_BODY_LEN);
	st = mscp_hdr_build(&w, &c->hdr, opcode);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_MOD, 0u); /* no modifiers defined for transfers here */
	vms_wire_put_le32(&w, VMS_OFF_MSCP_XFER_C_BCNT, c->byte_count);
	vms_wire_put_bytes(&w, VMS_OFF_MSCP_XFER_C_BUFF, VMS_MSCP_XFER_BUFF_LEN,
			   c->buffer_desc);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_XFER_C_LBN, c->lbn);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_CMD_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_xfer_cmd_parse(const uint8_t *frame, uint32_t len,
					   struct vms_mscp_xfer_cmd *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_mscp_xfer_cmd *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_hdr_parse(&v, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;
	if ((out->hdr.opcode != VMS_MSCP_OP_READ &&
	     out->hdr.opcode != VMS_MSCP_OP_WRITE))
		return VMS_CODEC_E_CLASS;

	out->byte_count = vms_wire_get_le32(&v, VMS_OFF_MSCP_XFER_C_BCNT);
	vms_wire_get_bytes(&v, VMS_OFF_MSCP_XFER_C_BUFF, VMS_MSCP_XFER_BUFF_LEN,
			   out->buffer_desc);
	out->lbn = vms_wire_get_le32(&v, VMS_OFF_MSCP_XFER_C_LBN);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 7  End messages
 * ------------------------------------------------------------------ */

static vms_codec_status_t mscp_end_hdr_build(vms_wire_buf_t *w,
					     const struct vms_mscp_end_hdr *eh,
					     uint8_t base_opcode)
{
	vms_wire_put_le32(w, VMS_OFF_MSCP_CRF, eh->hdr.cmd_ref);
	vms_wire_put_le16(w, VMS_OFF_MSCP_UNIT, eh->hdr.unit);
	vms_wire_put_zero(w, VMS_OFF_MSCP_RSVD6, 2u);
	vms_wire_put_u8(w, VMS_OFF_MSCP_OPCD,
			(uint8_t)(base_opcode | VMS_MSCP_END_BIT));
	vms_wire_put_u8(w, VMS_OFF_MSCP_FLGS, eh->flags);
	vms_wire_put_le16(w, VMS_OFF_MSCP_STS, eh->status);
	return w->err;
}

static vms_codec_status_t mscp_end_hdr_parse(vms_wire_view_t *v,
					     uint8_t expect_base_opcode,
					     struct vms_mscp_end_hdr *out)
{
	uint8_t opcode, flags;
	uint16_t status;

	opcode = vms_wire_get_u8(v, VMS_OFF_MSCP_OPCD);
	flags = vms_wire_get_u8(v, VMS_OFF_MSCP_FLGS);
	status = vms_wire_get_le16(v, VMS_OFF_MSCP_STS);
	if (!vms_wire_view_ok(v))
		return v->err;
	if ((opcode & VMS_MSCP_END_BIT) == 0 ||
	    (opcode & VMS_MSCP_OPCODE_MASK) != expect_base_opcode)
		return VMS_CODEC_E_CLASS;

	out->hdr.cmd_ref = vms_wire_get_le32(v, VMS_OFF_MSCP_CRF);
	out->hdr.unit = vms_wire_get_le16(v, VMS_OFF_MSCP_UNIT);
	out->hdr.opcode = opcode;
	out->flags = flags;
	out->status = status;
	out->status_major = vms_mscp_status_major(status);
	out->status_subcode = vms_mscp_status_subcode(status);
	if (!vms_wire_view_ok(v))
		return v->err;
	return VMS_CODEC_OK;
}

/* ---- SET CONTROLLER CHARACTERISTICS end (28 bytes, MEASURED) ---- */

vms_codec_status_t vms_mscp_scc_end_build(const struct vms_mscp_scc_end *e,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (e == (const struct vms_mscp_scc_end *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_SCC_END_LEN);
	st = mscp_end_hdr_build(&w, &e->eh, VMS_MSCP_OP_SCC);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_E_VRSN, e->version);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_E_CNTF, e->ctlr_flags);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_E_CTMO, e->ctlr_timeout);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_SCC_E_RSVD18, e->rsvd18);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_SCC_E_CNTI,
			  (uint32_t)(e->ctlr_id & 0xffffffffu));
	vms_wire_put_le32(&w, VMS_OFF_MSCP_SCC_E_CNTI + 4u,
			  (uint32_t)(e->ctlr_id >> 32));

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_SCC_END_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_scc_end_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_scc_end *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint32_t lo, hi;

	if (out == (struct vms_mscp_scc_end *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_end_hdr_parse(&v, VMS_MSCP_OP_SCC, &out->eh);
	if (st != VMS_CODEC_OK)
		return st;

	out->version = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_E_VRSN);
	out->ctlr_flags = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_E_CNTF);
	out->ctlr_timeout = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_E_CTMO);
	out->rsvd18 = vms_wire_get_le16(&v, VMS_OFF_MSCP_SCC_E_RSVD18);
	lo = vms_wire_get_le32(&v, VMS_OFF_MSCP_SCC_E_CNTI);
	hi = vms_wire_get_le32(&v, VMS_OFF_MSCP_SCC_E_CNTI + 4u);
	if (!vms_wire_view_ok(&v))
		return v.err;
	out->ctlr_id = ((uint64_t)hi << 32) | (uint64_t)lo;
	return VMS_CODEC_OK;
}

/* ---- GET UNIT STATUS end (52 bytes, MEASURED) ---- */

vms_codec_status_t vms_mscp_gus_end_build(const struct vms_mscp_gus_end *e,
					  uint8_t *frame, uint32_t cap,
					  uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (e == (const struct vms_mscp_gus_end *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_GUS_END_LEN);
	st = mscp_end_hdr_build(&w, &e->eh, VMS_MSCP_OP_GUS);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_MLUN, e->multi_unit);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_UNFL, e->unit_flags);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_GUS_E_UNTI,
			  (uint32_t)(e->unit_id & 0xffffffffu));
	vms_wire_put_le32(&w, VMS_OFF_MSCP_GUS_E_UNTI + 4u,
			  (uint32_t)(e->unit_id >> 32));
	vms_wire_put_le32(&w, VMS_OFF_MSCP_GUS_E_MEDI, e->media_id);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_SHUN, e->shadow_unit);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_TRCK, e->track_size);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_GRP, e->group_size);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_CYL, e->cyl_size);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_RCTS, e->rct_size);
	vms_wire_put_u8(&w, VMS_OFF_MSCP_GUS_E_RBNS, e->rbns);
	vms_wire_put_u8(&w, VMS_OFF_MSCP_GUS_E_RCTC, e->rct_copies);
	/* body[48:50] OBSERVED constant; body[50:52] stays zero -- see the
	 * header doc comment on VMS_MSCP_GUS_TAIL_OBSERVED (INV-6: never
	 * invent the tail's undecoded second half). */
	vms_wire_put_le16(&w, VMS_OFF_MSCP_GUS_E_TAIL, VMS_MSCP_GUS_TAIL_OBSERVED);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_GUS_END_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_gus_end_parse(const uint8_t *frame, uint32_t len,
					  struct vms_mscp_gus_end *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint32_t lo, hi;

	if (out == (struct vms_mscp_gus_end *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_end_hdr_parse(&v, VMS_MSCP_OP_GUS, &out->eh);
	if (st != VMS_CODEC_OK)
		return st;

	out->multi_unit = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_MLUN);
	out->unit_flags = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_UNFL);
	lo = vms_wire_get_le32(&v, VMS_OFF_MSCP_GUS_E_UNTI);
	hi = vms_wire_get_le32(&v, VMS_OFF_MSCP_GUS_E_UNTI + 4u);
	out->unit_id = ((uint64_t)hi << 32) | (uint64_t)lo;
	out->media_id = vms_wire_get_le32(&v, VMS_OFF_MSCP_GUS_E_MEDI);
	out->shadow_unit = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_SHUN);
	out->track_size = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_TRCK);
	out->group_size = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_GRP);
	out->cyl_size = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_CYL);
	out->rct_size = vms_wire_get_le16(&v, VMS_OFF_MSCP_GUS_E_RCTS);
	out->rbns = vms_wire_get_u8(&v, VMS_OFF_MSCP_GUS_E_RBNS);
	out->rct_copies = vms_wire_get_u8(&v, VMS_OFF_MSCP_GUS_E_RCTC);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ---- ONLINE end (44 bytes, MEASURED) ---- */

vms_codec_status_t vms_mscp_online_end_build(const struct vms_mscp_online_end *e,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (e == (const struct vms_mscp_online_end *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, VMS_MSCP_ONLINE_END_LEN);
	st = mscp_end_hdr_build(&w, &e->eh, VMS_MSCP_OP_ONLINE);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le16(&w, VMS_OFF_MSCP_ONLINE_E_MLUN, e->multi_unit);
	vms_wire_put_le16(&w, VMS_OFF_MSCP_ONLINE_E_UNFL, e->unit_flags);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_ONLINE_E_UNTI,
			  (uint32_t)(e->unit_id & 0xffffffffu));
	vms_wire_put_le32(&w, VMS_OFF_MSCP_ONLINE_E_UNTI + 4u,
			  (uint32_t)(e->unit_id >> 32));
	vms_wire_put_le32(&w, VMS_OFF_MSCP_ONLINE_E_MEDI, e->media_id);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_ONLINE_E_UNSZ, e->unit_size);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_ONLINE_E_VSER, e->volume_ser);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_MSCP_ONLINE_END_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_online_end_parse(const uint8_t *frame, uint32_t len,
					     struct vms_mscp_online_end *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint32_t lo, hi;

	if (out == (struct vms_mscp_online_end *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_end_hdr_parse(&v, VMS_MSCP_OP_ONLINE, &out->eh);
	if (st != VMS_CODEC_OK)
		return st;

	out->multi_unit = vms_wire_get_le16(&v, VMS_OFF_MSCP_ONLINE_E_MLUN);
	out->unit_flags = vms_wire_get_le16(&v, VMS_OFF_MSCP_ONLINE_E_UNFL);
	lo = vms_wire_get_le32(&v, VMS_OFF_MSCP_ONLINE_E_UNTI);
	hi = vms_wire_get_le32(&v, VMS_OFF_MSCP_ONLINE_E_UNTI + 4u);
	out->unit_id = ((uint64_t)hi << 32) | (uint64_t)lo;
	out->media_id = vms_wire_get_le32(&v, VMS_OFF_MSCP_ONLINE_E_MEDI);
	out->unit_size = vms_wire_get_le32(&v, VMS_OFF_MSCP_ONLINE_E_UNSZ);
	out->volume_ser = vms_wire_get_le32(&v, VMS_OFF_MSCP_ONLINE_E_VSER);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ---- READ / WRITE (transfer) end messages ---- */

static vms_codec_status_t mscp_xfer_end_build(const struct vms_mscp_xfer_end *e,
					      uint8_t base_opcode,
					      uint32_t body_len,
					      uint8_t *frame, uint32_t cap,
					      uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (e == (const struct vms_mscp_xfer_end *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(&w, VMS_OFF_SYSAP_BODY, body_len);
	st = mscp_end_hdr_build(&w, &e->eh, base_opcode);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_le32(&w, VMS_OFF_MSCP_XFER_E_BCNT, e->byte_count);
	vms_wire_put_le32(&w, VMS_OFF_MSCP_XFER_E_FBBK, e->first_bad_block);
	/* abs[88,100) (RESERVED, Table A-7) and, for WRITE, the four trailing
	 * MEASURED-but-ungrounded bytes past P.FBBK are already zero from the
	 * zero-fill above; nothing more is written there (INV-6). */

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = body_len;
	return VMS_CODEC_OK;
}

static vms_codec_status_t mscp_xfer_end_parse(const uint8_t *frame, uint32_t len,
					      uint8_t base_opcode,
					      struct vms_mscp_xfer_end *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_mscp_xfer_end *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, frame, len);
	st = mscp_end_hdr_parse(&v, base_opcode, &out->eh);
	if (st != VMS_CODEC_OK)
		return st;

	out->byte_count = vms_wire_get_le32(&v, VMS_OFF_MSCP_XFER_E_BCNT);
	out->first_bad_block = vms_wire_get_le32(&v, VMS_OFF_MSCP_XFER_E_FBBK);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_mscp_read_end_build(const struct vms_mscp_xfer_end *e,
					   uint8_t *frame, uint32_t cap,
					   uint32_t *written)
{
	return mscp_xfer_end_build(e, VMS_MSCP_OP_READ, VMS_MSCP_READ_END_LEN,
				   frame, cap, written);
}

vms_codec_status_t vms_mscp_read_end_parse(const uint8_t *frame, uint32_t len,
					   struct vms_mscp_xfer_end *out)
{
	return mscp_xfer_end_parse(frame, len, VMS_MSCP_OP_READ, out);
}

vms_codec_status_t vms_mscp_write_end_build(const struct vms_mscp_xfer_end *e,
					    uint8_t *frame, uint32_t cap,
					    uint32_t *written)
{
	return mscp_xfer_end_build(e, VMS_MSCP_OP_WRITE, VMS_MSCP_WRITE_END_LEN,
				   frame, cap, written);
}

vms_codec_status_t vms_mscp_write_end_parse(const uint8_t *frame, uint32_t len,
					    struct vms_mscp_xfer_end *out)
{
	return mscp_xfer_end_parse(frame, len, VMS_MSCP_OP_WRITE, out);
}

/* ------------------------------------------------------------------ *
 * sec 8  The allowlist rows this item contributes
 * ------------------------------------------------------------------ */

const struct vms_wire_allow_entry vms_mscp_allow_rows[] = {
	{ VMS_SYSAP_MSCP_DISK, VMS_MSCP_ALLOW_CATEGORY, VMS_MSCP_OP_SCC,
	  VMS_WIRE_ACT_RESPOND, 1u, "AA-L619A-TK sec 6.16, vms-291 lab-2 capture" },
	{ VMS_SYSAP_MSCP_DISK, VMS_MSCP_ALLOW_CATEGORY, VMS_MSCP_OP_GUS,
	  VMS_WIRE_ACT_RESPOND, 2u, "AA-L619A-TK sec 6.12, 18855/18855 captured" },
	{ VMS_SYSAP_MSCP_DISK, VMS_MSCP_ALLOW_CATEGORY, VMS_MSCP_OP_ONLINE,
	  VMS_WIRE_ACT_RESPOND, 3u, "AA-L619A-TK sec 6.13, vms-291 lab-2 capture" },
	{ VMS_SYSAP_MSCP_DISK, VMS_MSCP_ALLOW_CATEGORY, VMS_MSCP_OP_READ,
	  VMS_WIRE_ACT_RESPOND, 4u, "AA-L619A-TK sec 5.3, vms-291 lab-2 capture" },
	{ VMS_SYSAP_MSCP_DISK, VMS_MSCP_ALLOW_CATEGORY, VMS_MSCP_OP_WRITE,
	  VMS_WIRE_ACT_RESPOND, 5u, "AA-L619A-TK sec 5.3, vms-291 lab-2 capture" },
};

const struct vms_wire_allow_table vms_mscp_allow_table = {
	vms_mscp_allow_rows,
	(uint16_t)(sizeof(vms_mscp_allow_rows) / sizeof(vms_mscp_allow_rows[0]))
};
