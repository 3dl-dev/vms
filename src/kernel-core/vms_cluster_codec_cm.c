// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_cm.c - connection-manager (CM / VMS$VAXcluster SYSAP)
 * typed codec entries (FC-P3.1).
 *
 * Read vms_cluster_codec_cm.h first for the field map, the scope note (this
 * file owns the 132-byte SYSAP body; the wrapping SCS envelope is a minimal
 * honest stand-in pending FC-P1.1/P2.1) and the honesty rule this file
 * follows: every response recipe copies REAL received bytes (an echo) or
 * asserts OVMX's own caller-supplied state; nothing is templated or
 * invented. Pure, like the parent TU and vms_cluster_codec_hello.c: no
 * state, no allocation, no substrate call, no libc -- every byte move goes
 * through the vms_wire_get_*, vms_wire_put_* and vms_wire_put_zero
 * primitives the parent TU exports.
 */

#include "vms_cluster_codec_cm.h"

/* ------------------------------------------------------------------ *
 * Shared BODY gate (E73)
 *
 * What a SYSAP is handed on receive is its own 132 bytes and nothing below
 * them (design sec 3.2.4: SCS "calls scs_sysap_ops.message(ctx, local_conid,
 * frame + 72, inner_len - 16)"; vms_scs_fsm.c h_rx_appl_msg does exactly
 * that). The gate this file used to apply -- vms_frame_classify() plus
 * fi->cls == VMS_FCLS_SCS_MSG -- reads the ETHERTYPE at abs 12 and therefore
 * refuses EVERY real inbound CM message: see the VMS_OFB_CM_* block in the
 * header for the live run in which it lost VAX2's op-0x03 membership COMMIT.
 *
 * So the gate is a LENGTH gate now, and the length is exact: spec sec 4(d)
 * makes the VMS$VAXcluster class a fixed 190-content frame whose SYSAP body
 * is 132 bytes, and there is no shorter grounded CM message. Half a body is
 * refused rather than parsed out of whatever follows it in memory.
 * ------------------------------------------------------------------ */

static vms_codec_status_t cm_check_body(const uint8_t *body, uint32_t len)
{
	if (body == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (len < VMS_CM_BODY_LEN)
		return VMS_CODEC_E_SHORT;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 2: the SYSAP transaction envelope (sec 4(j))
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_cm_envelope_parse(const uint8_t *body, uint32_t len,
					 struct vms_cm_envelope *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_cm_envelope *)0)
		return VMS_CODEC_E_INVAL;
	st = cm_check_body(body, len);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, body, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	out->send_msg = vms_wire_get_le16(&v, VMS_OFB_CM_SEND_MSG);
	out->ack_msg  = vms_wire_get_le16(&v, VMS_OFB_CM_ACK_MSG);
	out->txn      = vms_wire_get_le16(&v, VMS_OFB_CM_TXN);
	out->token    = vms_wire_get_le16(&v, VMS_OFB_CM_TOKEN);
	out->category = vms_wire_get_u8(&v, VMS_OFB_CM_CATEGORY);
	out->opcode   = vms_wire_get_u8(&v, VMS_OFB_CM_OPCODE);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_body_kind(const uint8_t *body, uint32_t len,
				    uint8_t *out_category, uint8_t *out_opcode)
{
	vms_wire_view_t v;
	uint8_t category, opcode;

	vms_wire_view_init(&v, body, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	category = vms_wire_get_u8(&v, VMS_OFB_CM_CATEGORY);
	opcode   = vms_wire_get_u8(&v, VMS_OFB_CM_OPCODE);
	if (!vms_wire_view_ok(&v))
		return v.err;

	if (out_category != (uint8_t *)0)
		*out_category = category;
	if (out_opcode != (uint8_t *)0)
		*out_opcode = opcode;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 3: the abs [0,72) span -- NOT this file's business since FC-P3.15.
 * See the header's sec 3 note: the demoted `vms_cm_link`/its builder now
 * live at tests/cluster/host/vms_frame_compose.h, test-only.
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * sec 4: opcode-specific body parsers
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_cm_open_parse(const uint8_t *body, uint32_t len,
				     struct vms_cm_open *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_cm_open *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_cm_envelope_parse(body, len, &out->env);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, body, len);
	out->epoch = vms_wire_get_le32(&v, VMS_OFB_CM_EPOCH);
	out->role  = vms_wire_get_u8(&v, VMS_OFB_CM_ROLE);
	out->cls   = vms_wire_get_u8(&v, VMS_OFB_CM_CLASS);
	out->has_bitmap = (out->env.opcode == VMS_CM_OP_XITION_ADD);
	out->bitmap = out->has_bitmap
			      ? vms_wire_get_u8(&v, VMS_OFB_CM_BITMAP)
			      : 0;

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_open_bitmap_span(const uint8_t *body, uint32_t len,
					   uint8_t *out)
{
	struct vms_cm_envelope env;
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_cm_envelope_parse(body, len, &env);
	if (st != VMS_CODEC_OK)
		return st;

	/* Only the op-0x09 ADD open carries a membership bitmap (spec sec
	 * 4(p)); on any other opcode this span is somebody else's payload. */
	if (env.category != VMS_CM_CAT_CONFIG ||
	    env.opcode != VMS_CM_OP_XITION_ADD)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	vms_wire_get_bytes(&v, VMS_OFB_CM_BITMAP_SPAN, VMS_CM_BITMAP_SPAN_LEN,
			   out);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_barrier_parse(const uint8_t *body, uint32_t len,
					struct vms_cm_barrier *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_cm_barrier *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_cm_envelope_parse(body, len, &out->env);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, body, len);
	out->epoch = vms_wire_get_le32(&v, VMS_OFB_CM_EPOCH);
	out->step  = vms_wire_get_le32(&v, VMS_OFB_CM_STEP);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_params_parse(const uint8_t *body, uint32_t len,
				       struct vms_cm_params *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;

	if (out == (struct vms_cm_params *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_cm_envelope_parse(body, len, &out->env);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, body, len);
	out->votes    = vms_wire_get_le16(&v, VMS_OFB_CM_VOTES);
	out->param_f1 = vms_wire_get_le32(&v, VMS_OFB_CM_PARAM_F1);
	out->param_f2 = vms_wire_get_le32(&v, VMS_OFB_CM_PARAM_F2);
	vms_wire_get_bytes(&v, VMS_OFB_CM_VERSION, VMS_CM_VERSION_LEN,
			   out->version);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_model_parse(const uint8_t *body, uint32_t len,
				      struct vms_cm_model *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint8_t namelen;

	if (out == (struct vms_cm_model *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_cm_envelope_parse(body, len, &out->env);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, body, len);
	namelen = vms_wire_get_u8(&v, VMS_OFB_CM_MODEL_LEN);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (namelen > VMS_CM_MODEL_MAX)
		return VMS_CODEC_E_RANGE;
	out->namelen = namelen;
	vms_wire_get_bytes(&v, VMS_OFB_CM_MODEL_NAME, namelen, out->name);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_dlm_rebuild_parse(const uint8_t *body, uint32_t len,
					    struct vms_cm_dlm_rebuild *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint8_t resnamelen;

	if (out == (struct vms_cm_dlm_rebuild *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_cm_envelope_parse(body, len, &out->env);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, body, len);
	out->l1_len = vms_wire_get_u8(&v, VMS_OFB_CM_DLM_L1_LEN);
	resnamelen = vms_wire_get_u8(&v, VMS_OFB_CM_DLM_RESNAMLEN);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (resnamelen > VMS_CM_DLM_RESNAME_MAX)
		return VMS_CODEC_E_RANGE;
	out->resnamelen = resnamelen;
	vms_wire_get_bytes(&v, VMS_OFB_CM_DLM_RESNAME, resnamelen,
			   out->resname);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 5: response recipes (spec sec 4(p)/(q)/(r)/(u))
 * ------------------------------------------------------------------ */

/* Shared allowlist lookup + recipe-id check: every recipe builder REFUSES
 * to fire unless the request's (category, opcode) is a GROUNDED RESPOND
 * row for THAT recipe -- defence in depth alongside the FSM layer that
 * will also consult vms_cm_allow_table() before calling here. */
static vms_codec_status_t cm_recipe_allowed(const struct vms_cm_envelope *req,
					    uint16_t want_recipe)
{
	const struct vms_wire_allow_entry *e;

	e = vms_wire_allow_find(vms_cm_allow_table(), VMS_SYSAP_VMS_VAXCLUSTER,
				req->category, req->opcode);
	if (e == (const struct vms_wire_allow_entry *)0)
		return VMS_CODEC_E_CLASS;
	if (e->action != VMS_WIRE_ACT_RESPOND || e->recipe != want_recipe)
		return VMS_CODEC_E_CLASS;
	return VMS_CODEC_OK;
}

/* Verbatim-copy a received request BODY into a fresh body-sized wire buffer.
 * Both sides are body-relative since E73 -- the request is the 132 bytes SCS
 * handed the SYSAP, not a frame with 72 bytes of somebody else's headers in
 * front of it. Shared by every echo-based response recipe. */
static vms_codec_status_t cm_copy_request_body(const uint8_t *req_body,
					       uint32_t req_len,
					       uint8_t out[VMS_CM_BODY_LEN])
{
	vms_wire_view_t rv;

	vms_wire_view_init(&rv, req_body, req_len);
	vms_wire_get_bytes(&rv, 0, VMS_CM_BODY_LEN, out);
	if (!vms_wire_view_ok(&rv))
		return rv.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_echo_response_build(const uint8_t *req_body,
					      uint32_t req_len,
					      uint8_t own_class,
					      uint8_t *out_body, uint32_t cap,
					      uint32_t *written)
{
	struct vms_cm_envelope req_env;
	vms_wire_view_t rv;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint8_t body[VMS_CM_BODY_LEN];

	if (req_body == (const uint8_t *)0 || out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	st = vms_cm_envelope_parse(req_body, req_len, &req_env);
	if (st != VMS_CODEC_OK)
		return st;
	st = cm_recipe_allowed(&req_env, VMS_CM_RECIPE_ECHO);
	if (st != VMS_CODEC_OK)
		return st;

	/* Echo the whole 132-byte body VERBATIM first (spec sec 4(p): "The
	 * 0x81 echo takes THREE mutations" -- start from the request). This
	 * is also what carries body[4:8] (txn/token) forward untouched: the
	 * caller's cnxman_envelope_stamp(is_response=1) leaves them alone. */
	st = cm_copy_request_body(req_body, req_len, body);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_view_init(&rv, req_body, req_len);

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, 0, VMS_CM_BODY_LEN, body);

	/* The response bit and the echoed opcode. body[0:8] (send/ack/txn/
	 * token) is deliberately NOT written here -- the caller's stamp call
	 * owns it. */
	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY,
			vms_wire_response_category(req_env.category));
	/* opcode (body[9]) stays echoed -- already correct from the copy. */

	/* body[18] = 0x01 on every grounded opcode EXCEPT 0x0f, which echoes
	 * it (spec sec 4(r): "the 0x0f row... neither is a node setting the
	 * byte" -- skipping the write here leaves the verbatim copy alone). */
	if (req_env.opcode != VMS_CM_OP_0F)
		vms_wire_put_u8(&w, VMS_OFB_CM_RESP_MARK, 0x01);

	/* body[55] = 0x00 is op-0x09-SPECIFIC (spec sec 4(p): "it is the
	 * coordinator's MEMBERSHIP BITMAP, and the responder is refusing to
	 * assert it"). */
	if (req_env.opcode == VMS_CM_OP_XITION_ADD)
		vms_wire_put_u8(&w, VMS_OFB_CM_BITMAP, 0x00);

	/* op 0x12 takes two EXTRA mutations beyond the shared three (spec
	 * sec 4(r)): our own current transition class, and an LE u32 copy of
	 * the request's own epoch field. */
	if (req_env.opcode == VMS_CM_OP_RELAY) {
		uint32_t epoch = vms_wire_get_le32(&rv, VMS_OFB_CM_EPOCH);

		if (!vms_wire_view_ok(&rv))
			return rv.err;
		vms_wire_put_u8(&w, VMS_OFB_CM_CLASS, own_class);
		vms_wire_put_le32(&w, VMS_OFB_CM_RELAY_EPOCH, epoch);
	}

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_close_build(const uint8_t *req_body, uint32_t req_len,
				      const struct vms_cm_node_params *own_params,
				      uint16_t close_state,
				      uint8_t *out_body, uint32_t cap,
				      uint32_t *written)
{
	struct vms_cm_envelope req_env;
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (req_body == (const uint8_t *)0 ||
	    own_params == (const struct vms_cm_node_params *)0 ||
	    out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	/*
	 * body[24:26] is MANDATORY and its meaning is UNGROUNDED, so a caller
	 * with no value has nothing to say and this response cannot be built
	 * (VMS_OFF_CM_CLOSE_STATE: nonzero in 1308/1308 real closes; the one
	 * zero ever put there bugchecked the transition coordinator 0.6 ms
	 * later). Refusing costs a response; emitting cost the cluster.
	 */
	if (close_state == 0u)
		return VMS_CODEC_E_CLASS;

	st = vms_cm_envelope_parse(req_body, req_len, &req_env);
	if (st != VMS_CODEC_OK)
		return st;
	st = cm_recipe_allowed(&req_env, VMS_CM_RECIPE_CLOSE);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* Fresh body, built FROM ZERO -- NOT the request's payload. Spec sec
	 * 4(p): echoing this bugchecked a real VAX (INCONSTATE) because it
	 * carries that peer's own live Con.IDs and cluster id. Only the
	 * echoed (txn,checksum) token pair, the response bit, the echoed
	 * opcode, and OVMX's own node-parameter block are asserted here --
	 * body[0:4] (send/ack) is the caller's stamp call. */
	vms_wire_put_zero(&w, 0, VMS_CM_BODY_LEN);
	vms_wire_put_le16(&w, VMS_OFB_CM_TXN, req_env.txn);
	vms_wire_put_le16(&w, VMS_OFB_CM_TOKEN, req_env.token);
	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY,
			vms_wire_response_category(req_env.category));
	vms_wire_put_u8(&w, VMS_OFB_CM_OPCODE, req_env.opcode);
	vms_wire_put_le16(&w, VMS_OFB_CM_CLOSE_STATE, close_state);
	vms_wire_put_le32(&w, VMS_OFB_CM_PARAM_F1, own_params->param_f1);
	vms_wire_put_le32(&w, VMS_OFB_CM_PARAM_F2, own_params->param_f2);
	vms_wire_put_bytes(&w, VMS_OFB_CM_VERSION, VMS_CM_VERSION_LEN,
			   own_params->version);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_dlm_op0d_response_build(const uint8_t *req_body,
						  uint32_t req_len,
						  uint8_t *out_body, uint32_t cap,
						  uint32_t *written)
{
	struct vms_cm_envelope req_env;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint8_t body[VMS_CM_BODY_LEN];

	if (req_body == (const uint8_t *)0 || out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	st = vms_cm_envelope_parse(req_body, req_len, &req_env);
	if (st != VMS_CODEC_OK)
		return st;
	st = cm_recipe_allowed(&req_env, VMS_CM_RECIPE_DLM_OP0D);
	if (st != VMS_CODEC_OK)
		return st;

	/* VERBATIM echo, per spec sec 4(p): "reconstructs 1367 of 1367 real
	 * responses byte-for-byte". Deliberately does NOT take the cat-0x01
	 * body[18]/body[55] mutations -- those offsets land inside the L1
	 * region and the lock RESOURCE NAME here (LOCKMGRERR on two real
	 * VAXes when a prior implementation applied them). This is also what
	 * carries body[4:8] forward untouched for the caller's stamp call. */
	st = cm_copy_request_body(req_body, req_len, body);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, 0, VMS_CM_BODY_LEN, body);

	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY,
			vms_wire_response_category(req_env.category));
	/* body[34]: MANDATORY, written unconditionally regardless of what the
	 * request carried there (spec sec 4(p): every response carries 0xf9
	 * even where it lands mid-ASCII in the echoed name). */
	vms_wire_put_u8(&w, VMS_OFB_CM_DLM_RESULT, VMS_CM_DLM_RESULT_OP0D);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_body_build(const uint8_t *req_body, uint32_t req_len,
				     const uint8_t *body, uint32_t body_len,
				     uint8_t *out_body, uint32_t cap,
				     uint32_t *written)
{
	struct vms_cm_envelope req_env;
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (req_body == (const uint8_t *)0 || body == (const uint8_t *)0 ||
	    out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (body_len != VMS_CM_BODY_LEN)
		return VMS_CODEC_E_INVAL;

	st = vms_cm_envelope_parse(req_body, req_len, &req_env);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, 0, VMS_CM_BODY_LEN, body);

	/* The DLM's reply never writes body[0:8] (design sec 3.2.4 ruling E1).
	 * This wrapper echoes the ANSWERED REQUEST's txn/token itself, exactly
	 * as vms_cm_close_build does; the caller's stamp call
	 * (is_response=1) leaves them alone and fills only send/ack. */
	vms_wire_put_le16(&w, VMS_OFB_CM_TXN, req_env.txn);
	vms_wire_put_le16(&w, VMS_OFB_CM_TOKEN, req_env.token);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_cm_barrier_build(uint32_t epoch, uint32_t step,
					uint8_t *out_body, uint32_t cap,
					uint32_t *written)
{
	vms_wire_buf_t w;

	if (out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	/* Spec sec 4(p): the indices run 1...12 with no gaps. Step 0 is not a
	 * barrier step in any capture. */
	if (step == 0u)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* Zero first (including body[0:8] -- the caller's stamp call fills
	 * it), then exactly the four GROUNDED payload fields on top: the tail
	 * is acceptable residue in the reference and one real joiner sends it
	 * all zero (see the header's "THE ZERO TAIL IS GROUNDED" note). */
	vms_wire_put_zero(&w, 0, VMS_CM_BODY_LEN);
	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY, VMS_CM_CAT_CONFIG);
	vms_wire_put_u8(&w, VMS_OFB_CM_OPCODE, VMS_CM_OP_BARRIER);
	vms_wire_put_le32(&w, VMS_OFB_CM_EPOCH, epoch);
	/* body[16:20] is a plain LE u32 step index on op 0x0b/0x0c -- it
	 * ALIASES the role/class byte pair of the transition-open family, so
	 * no role tag is written here (spec sec 4(p)/(r)). */
	vms_wire_put_le32(&w, VMS_OFB_CM_STEP, step);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 5b: THE COORDINATOR'S ORIGINATIONS (FC-P3.12)
 *
 * The contract -- grounded placements only, explicit zeros everywhere else,
 * and the list of fields honestly omitted -- is in the header's sec 5b block.
 * ------------------------------------------------------------------ */

/*
 * Lay down a zeroed body, then the SYSAP category/opcode. Every originating
 * builder below starts here, so there is exactly one place where "the tail
 * is zero, not a template" is true or false. body[0:8] (send/ack/txn/token)
 * is deliberately left at zero: the caller's cnxman_envelope_stamp() call
 * fills it after this builder returns (design sec 3.2.4 ruling E1).
 */
static vms_codec_status_t cm_originate_begin(uint8_t opcode, uint8_t *out_body,
					     uint32_t cap, vms_wire_buf_t *w)
{
	if (out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(w, out_body, cap);
	if (!vms_wire_buf_ok(w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_zero(w, 0, VMS_CM_BODY_LEN);
	vms_wire_put_u8(w, VMS_OFB_CM_CATEGORY, VMS_CM_CAT_CONFIG);
	vms_wire_put_u8(w, VMS_OFB_CM_OPCODE, opcode);
	return VMS_CODEC_OK;
}

static vms_codec_status_t cm_originate_end(vms_wire_buf_t *w, uint32_t *written)
{
	if (!vms_wire_buf_ok(w))
		return w->err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

/* body[16:18] = the role slot and the transition class, the pair sec 4(r)
 * calls the tag. Written together because a transition-open, a GO and a relay
 * differ ONLY in these two bytes and the opcode. */
static void cm_put_tag(vms_wire_buf_t *w, uint8_t role, uint8_t cls)
{
	vms_wire_put_u8(w, VMS_OFB_CM_ROLE, role);
	vms_wire_put_u8(w, VMS_OFB_CM_CLASS, cls);
}

/* sec 4(r)'s class -> transition-open opcode pairing, with zero residuals.
 * Returns 0 for a class this project has never seen open a transition. */
static uint8_t cm_open_opcode_of_class(uint8_t cls)
{
	switch (cls) {
	case VMS_CM_CLASS_ADD:    return VMS_CM_OP_XITION_ADD;    /* tag 0x0240 */
	case VMS_CM_CLASS_REMOVE: return VMS_CM_OP_XITION_REM;    /* tag 0x0340 */
	case VMS_CM_CLASS_DEPART: return VMS_CM_OP_DEPART_XITION; /* tag 0x0440 */
	default:                  return 0u;
	}
}

vms_codec_status_t vms_cm_xition_open_build(uint8_t tr_class, uint32_t epoch,
					    uint8_t bitmap, int has_bitmap,
					    uint8_t *out_body, uint32_t cap,
					    uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint8_t opcode = cm_open_opcode_of_class(tr_class);

	if (opcode == 0u)
		return VMS_CODEC_E_CLASS;
	/* Only the class-0x02 ADD open carries a nodemap (sec 4(p)). Asking for
	 * one on any other class is refused rather than dropped, so a caller
	 * cannot believe it published a membership map that never went out. */
	if (has_bitmap && tr_class != VMS_CM_CLASS_ADD)
		return VMS_CODEC_E_INVAL;

	st = cm_originate_begin(opcode, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_put_le32(&w, VMS_OFB_CM_EPOCH, epoch);
	cm_put_tag(&w, VMS_CM_ROLE_XITION, tr_class);
	if (has_bitmap)
		vms_wire_put_u8(&w, VMS_OFB_CM_BITMAP, bitmap);

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_go_build(uint8_t tr_class, uint32_t epoch,
				   uint8_t *out_body, uint32_t cap,
				   uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (cm_open_opcode_of_class(tr_class) == 0u)
		return VMS_CODEC_E_CLASS;

	/* txn/token stay zero (never written): sec 4(p) "Notifications carry
	 * txn=0 and are NEVER answered". */
	st = cm_originate_begin(VMS_CM_OP_XITION_GO, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_put_le32(&w, VMS_OFB_CM_EPOCH, epoch);
	cm_put_tag(&w, VMS_CM_ROLE_GO, tr_class);

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_release_build(uint32_t epoch, uint32_t step,
					uint8_t *out_body, uint32_t cap,
					uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	/* Sec 4(p): the indices run 1...12 with no gaps. Step 0 is not a
	 * barrier step in any capture. */
	if (step == 0u)
		return VMS_CODEC_E_INVAL;

	st = cm_originate_begin(VMS_CM_OP_BARRIER_REL, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_put_le32(&w, VMS_OFB_CM_EPOCH, epoch);
	/* body[16:20] is a plain LE u32 step index on op 0x0b/0x0c -- it
	 * ALIASES the role/class byte pair, so no tag is written here. */
	vms_wire_put_le32(&w, VMS_OFB_CM_STEP, step);

	return cm_originate_end(&w, written);
}

void vms_cm_notification_zero_txn(uint8_t out_body[VMS_CM_BODY_LEN])
{
	vms_wire_buf_t w;

	if (out_body == (uint8_t *)0)
		return;
	vms_wire_buf_init(&w, out_body, VMS_CM_BODY_LEN);
	if (!vms_wire_buf_ok(&w))
		return;
	/* BOTH cells: txn body[4:6] and token body[6:8]. 125/125 real GOs and
	 * 1104/1104 real RELEASEs carry zero in each (E85 census). */
	vms_wire_put_zero(&w, VMS_OFB_CM_TXN, 4u);
}

vms_codec_status_t vms_cm_relay_build(uint8_t tr_class, uint32_t epoch,
				      uint8_t *out_body, uint32_t cap,
				      uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (cm_open_opcode_of_class(tr_class) == 0u)
		return VMS_CODEC_E_CLASS;

	st = cm_originate_begin(VMS_CM_OP_RELAY, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_put_le32(&w, VMS_OFB_CM_EPOCH, epoch);
	cm_put_tag(&w, VMS_CM_ROLE_RELAY, tr_class);
	/* The subject of the relay is NOT written: no capture isolates a system
	 * identity in this body (header sec 5b). Its bytes stay zero and
	 * FC-P3.12 counts the omission. */

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_commit_build(uint8_t tr_class, uint32_t epoch,
				       uint8_t *out_body, uint32_t cap,
				       uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (cm_open_opcode_of_class(tr_class) == 0u)
		return VMS_CODEC_E_CLASS;

	st = cm_originate_begin(VMS_CM_OP_COMMIT, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_put_le32(&w, VMS_OFB_CM_EPOCH, epoch);
	cm_put_tag(&w, VMS_CM_ROLE_COMMIT, tr_class);

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_step_ack_build(const uint8_t *req_body,
					 uint32_t req_len,
					 uint8_t *out_body, uint32_t cap,
					 uint32_t *written)
{
	struct vms_cm_envelope req_env;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint8_t body[VMS_CM_BODY_LEN];

	if (req_body == (const uint8_t *)0 || out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	st = vms_cm_envelope_parse(req_body, req_len, &req_env);
	if (st != VMS_CODEC_OK)
		return st;
	st = cm_recipe_allowed(&req_env, VMS_CM_RECIPE_STEP_ACK);
	if (st != VMS_CODEC_OK)
		return st;

	/* Verbatim echo of the member's own step request first -- txn and
	 * token ride back untouched (the caller's stamp call leaves them),
	 * which is the whole correlation. */
	st = cm_copy_request_body(req_body, req_len, body);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, 0, VMS_CM_BODY_LEN, body);

	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY,
			vms_wire_response_category(req_env.category));
	/* The role-slot mutation. See the header's sec 5b entry for why this
	 * follows sec 4(r)'s 26-capture census rather than the step-index
	 * reading of the same offset, and why it is safe either way. */
	vms_wire_put_u8(&w, VMS_OFB_CM_ROLE, VMS_CM_ROLE_RELAY);

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_ack_build(uint8_t *out_body, uint32_t cap,
				    uint32_t *written)
{
	vms_wire_buf_t w;

	if (out_body == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, out_body, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* No payload -- spec sec 4(p): "An implementation should send zeros;
	 * do not reproduce another implementation's uninitialised memory."
	 * body[9] (opcode) is likewise not meaningful (sec 4(p)/4(u)): real
	 * VMS acks carry whatever stale buffer content sat there. body[0:4]
	 * (send/ack) is the caller's stamp call; body[4:8] (txn/token) has no
	 * meaning here and stays the zero this pass already put there. */
	vms_wire_put_zero(&w, 0, VMS_CM_BODY_LEN);
	vms_wire_put_u8(&w, VMS_OFB_CM_CATEGORY, VMS_CM_CAT_ACK);
	vms_wire_put_u8(&w, VMS_OFB_CM_OPCODE, 0x00);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_CM_BODY_LEN;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 5c  THE JOINER'S ORIGINATIONS (FC-P3.3)
 *
 * The three cat-0x01 messages a joiner speaks first (spec sec 4(o)'s
 * ordered table: op 0x14 model, op 0x01 parameters, op 0x02 config), plus
 * one READ-ONLY instrumentation helper for the op-0x06 MEMBERSHIP burst.
 * Same rule as sec 5b: write ONLY fields whose PLACEMENT is grounded, from
 * values the caller read out of real executive state; every other byte an
 * EXPLICIT ZERO. None of them writes body[0:8] -- the caller stamps it with
 * cnxman_envelope_stamp() afterwards, is_response=0.
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_cm_model_build(const uint8_t *name, uint8_t namelen,
				      uint8_t *out_body, uint32_t cap,
				      uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (namelen > VMS_CM_MODEL_MAX)
		return VMS_CODEC_E_RANGE;
	if (namelen != 0u && name == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;

	st = cm_originate_begin(VMS_CM_OP_MODEL, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	/* sec 4(j) row 1: "a length-prefixed ASCII string at body[16] (len
	 * 0x15 = 21) + \"VAXserver 3900 Series\"". namelen 0 is a legal,
	 * HONEST value -- a node whose model string the executive could not
	 * read advertises none rather than a plausible-looking one (INV-6). */
	vms_wire_put_u8(&w, VMS_OFB_CM_MODEL_LEN, namelen);
	if (namelen != 0u)
		vms_wire_put_bytes(&w, VMS_OFB_CM_MODEL_NAME, namelen, name);

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_params_build(uint16_t votes,
				       const struct vms_cm_node_params *own_params,
				       uint8_t *out_body, uint32_t cap,
				       uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (own_params == (const struct vms_cm_node_params *)0)
		return VMS_CODEC_E_INVAL;

	st = cm_originate_begin(VMS_CM_OP_PARAMS, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	/* body[22:24] VOTES -- GROUNDED byte-exact across four vote values
	 * {0,1,2} by controlled reconfiguration (sec 4(j) "VOTES -- GROUNDED
	 * across four configurations"). The value is the caller's, read from
	 * SYSGEN; this builder has no default.
	 *
	 * body[72:76]/[76:80]/[88:96] are the NODE-PARAMETER BLOCK, the same
	 * one vms_cm_close_build carries, and it is likewise the caller's --
	 * never a captured "V7.3" (honest-OS-identity ruling: OVMX advertises
	 * its own version and sec 4(L)(6) measured that a real VAX accepts and
	 * DISPLAYS a non-"VMS" string here).
	 *
	 * EXPECTED_VOTES and LOCKDIRWT are ABSENT, and that is an honest
	 * omission rather than an oversight: sec 4(j)'s own RE-gap list says
	 * EXPECTED_VOTES "was held at 1 in every captured configuration, so no
	 * wire contrast exists to locate it", and LOCKDIRWT's offset is plan
	 * row FC-P3.2 (lab). Every ungrounded byte of this body therefore goes
	 * out zero; a caller whose real LOCKDIRWT is nonzero cannot advertise
	 * it and must say so (FC-P3.3 counts and logs exactly that). */
	vms_wire_put_le16(&w, VMS_OFB_CM_VOTES, votes);
	vms_wire_put_le32(&w, VMS_OFB_CM_PARAM_F1, own_params->param_f1);
	vms_wire_put_le32(&w, VMS_OFB_CM_PARAM_F2, own_params->param_f2);
	vms_wire_put_bytes(&w, VMS_OFB_CM_VERSION, VMS_CM_VERSION_LEN,
			   own_params->version);

	return cm_originate_end(&w, written);
}

vms_codec_status_t vms_cm_config_build(uint8_t *out_body, uint32_t cap,
				       uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	st = cm_originate_begin(VMS_CM_OP_CONFIG, out_body, cap, &w);
	if (st != VMS_CODEC_OK)
		return st;

	/* Nothing else. sec 4(o): the admission op-0x02 of vax3-2to3 frame 285
	 * "carries an all-zero topology body and is acked in 0.3 ms", so a
	 * zero body is a MEASURED-sufficient admission trigger. The two spans
	 * sec 4(o) calls "REPLAYED, not decoded" -- body[10:12] = 0x5041 and
	 * twelve 0x20 spaces at body[40:52] -- are deliberately NOT reproduced:
	 * they are another implementation's bytes, they are not constant even
	 * for that implementation (the same node's later 0x02 carries 0x0004
	 * and binary there), and replaying them would assert a value this node
	 * cannot derive. That is the exact failure mode INV-6 exists to stop. */
	return cm_originate_end(&w, written);
}

/* The CSID-shape test (VMS_CM_CSID_SHAPE_* in the header): does `v` look
 * like a real (generation << 16 | SCSSYSTEMID & 0x3FF) CSID rather than one
 * of the burst's other sub-records (ASCII fragments, zero padding) that
 * happen to share the same two candidate offsets? */
static int cm_csid_shape_ok(uint32_t v)
{
	uint32_t hi = (v >> 16) & 0xffffu;
	uint32_t lo = v & 0xffffu;

	return hi != 0u && hi <= VMS_CM_CSID_SHAPE_HI_MAX &&
	       (lo & VMS_CM_CSID_SHAPE_LO_MASK) == 0u;
}

vms_codec_status_t vms_cm_membership_coordinator_csid(const uint8_t *body,
						uint32_t len,
						uint32_t *out_csid)
{
	struct vms_cm_envelope env;
	vms_wire_view_t v;
	vms_codec_status_t st;
	uint32_t a, b;

	if (out_csid == (uint32_t *)0)
		return VMS_CODEC_E_INVAL;
	*out_csid = 0u;

	st = vms_cm_envelope_parse(body, len, &env);
	if (st != VMS_CODEC_OK)
		return st;
	if (env.category != VMS_CM_CAT_CONFIG ||
	    env.opcode != VMS_CM_OP_MEMBERSHIP)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	a = vms_wire_get_le32(&v, VMS_OFB_CM_MEMBERSHIP_CSID_A);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (cm_csid_shape_ok(a)) {
		*out_csid = a;
		return VMS_CODEC_OK;
	}

	vms_wire_view_init(&v, body, len);
	b = vms_wire_get_le32(&v, VMS_OFB_CM_MEMBERSHIP_CSID_B);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (cm_csid_shape_ok(b)) {
		*out_csid = b;
		return VMS_CODEC_OK;
	}

	return VMS_CODEC_E_RANGE;
}

/* ------------------------------------------------------------------ *
 * sec 6: the GROUNDED (SYSAP, category, opcode) allowlist
 * ------------------------------------------------------------------ */

/*
 * Every RESPOND row cites the spec paragraph that grounds it (design
 * §3.9 rule 4 / vms_cluster_codec.h §6). Pairs this project has explicitly
 * measured as ungrounded -- cat 0x02 op 0x01/0x12 (the steady-state DLM
 * lookup/enqueue traffic, whose reply is NOT derivable from the request
 * without a real lock database: the retired strawman's cm_response_shape
 * allowlist (harvested into this codec as data; its own
 * measurement, "no recipe short of 'have a lock database' reconstructs
 * more than 37%") -- are simply ABSENT: vms_wire_allow_find() returns NULL
 * for them, identical in effect to a CONSUME row, which is exactly the
 * "send nothing, log it" the spec's allowlist rule requires (sec 4(p)).
 */
static const struct vms_wire_allow_entry g_cm_allow_rows[] = {
	/* cat 0x01 echo-recipe family (sec 4(p) "0x81 echo takes THREE
	 * mutations"; sec 4(r) "Response recipes by opcode"). */
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_COMMIT,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(p)/4(r): op 0x03 membership commit" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_LOCKRB,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(p)/4(r): op 0x05 lock/resource "
	  "rebuild txn" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_REM,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(r): op 0x08 class-0x03 REMOVE open" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_ADD,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(p)/4(r): op 0x09 class-0x02 ADD open" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_DEPART_XITION,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(r): op 0x0d class-0x04 self-departure "
	  "open (cat 0x01 ONLY -- cat 0x02 op 0x0d is the DLM rebuild record)" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_0F,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(r): op 0x0f, class-0x03 extra step, "
	  "body[18] ECHOED not forced" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_RELAY,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_ECHO,
	  "cluster-protocol-spec.md sec 4(r): op 0x12 coordinator relay" },

	/* The COORDINATOR's side of the barrier (FC-P3.12): op 0x0b arrives
	 * from a member reporting step N, and sec 4(p)'s barrier table has the
	 * coordinator answering it with 0x81/0x0b -- "the coordinator's ack,
	 * NOT the release". A member whose step is never acknowledged keeps
	 * retransmitting it. Its own recipe, not the ECHO family's: sec 4(r)'s
	 * role census puts 0x10 at body[16] on this response and does not list
	 * op 0x0b in its body[18] mutation table. */
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_STEP_ACK,
	  "cluster-protocol-spec.md sec 4(p) barrier table + sec 4(r) role "
	  "census: the coordinator's 0x81/0x0b step acknowledgement" },

	/* cat 0x02: op 0x0d is the ONLY grounded opcode during a join
	 * (sec 4(p): "the ONLY cat-0x02 opcode that occurs during a join"). */
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_DLM, VMS_CM_OP_DLM_REBUILD,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_DLM_OP0D,
	  "cluster-protocol-spec.md sec 4(p): op 0x0d DLM lock-resource "
	  "rebuild record, verbatim echo + body[34]=0xf9" },

	/* cat 0x06: closes the transaction (sec 4(p)/(q)). */
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_MEMBERSHIP, VMS_CM_OP_CLOSE,
	  VMS_WIRE_ACT_RESPOND, VMS_CM_RECIPE_CLOSE,
	  "cluster-protocol-spec.md sec 4(p)/4(q): cat 0x06 op 0x00 close / "
	  "recurring member poll, own node-parameter block" },

	/* GROUNDED silent consumption: notifications carrying txn=0, never
	 * answered by any capture (sec 4(p)/(q)), and the coordinator's
	 * transition-abort notification (sec 4(O.32)/(O.39)). */
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_XITION_GO,
	  VMS_WIRE_ACT_CONSUME, 0,
	  "cluster-protocol-spec.md sec 4(p): op 0x0a barrier GO, txn=0, "
	  "never answered" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_BARRIER_REL,
	  VMS_WIRE_ACT_CONSUME, 0,
	  "cluster-protocol-spec.md sec 4(p)/4(q): op 0x0c barrier release "
	  "(including step 12), txn=0, never answered" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_MEMBERSHIP,
	  VMS_WIRE_ACT_CONSUME, 0,
	  "cluster-protocol-spec.md sec 4(p)/4(u): op 0x06 MEMBERSHIP burst, "
	  "answered only by the opportunistic cat-0x04 ack, never 0x81" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG, VMS_CM_OP_ABORT,
	  VMS_WIRE_ACT_CONSUME, 0,
	  "cluster-protocol-spec.md sec 4(O.32)/4(O.39): transition ABORT "
	  "(role 0x50), a normal per-member barrier notification, never "
	  "answered" },
};

static const struct vms_wire_allow_table g_cm_allow_table = {
	g_cm_allow_rows,
	(uint16_t)(sizeof(g_cm_allow_rows) / sizeof(g_cm_allow_rows[0]))
};

const struct vms_wire_allow_table *vms_cm_allow_table(void)
{
	return &g_cm_allow_table;
}
