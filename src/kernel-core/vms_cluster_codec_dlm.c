// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_dlm.c - cat-0x02 (DLM) typed codec entries (FC-P4.5).
 *
 * Read vms_cluster_codec_dlm.h first: it draws the GROUNDED/PROVISIONAL
 * line field by field and carries the fc8540ae hard-lesson doc comment
 * that motivates the lock-id refusal in vms_dlm_completion_build().
 *
 * Pure, like the parent TU and the HELLO family file: no state, no
 * allocation, no substrate call, no libc beyond the vms_wire_* primitives
 * the parent TU exports.
 */

#include "vms_cluster_codec_dlm.h"

/* ------------------------------------------------------------------ *
 * Shared class/category gating
 * ------------------------------------------------------------------ */

/* All cat-0x02 DLM traffic rides the 190-byte SCS_MSG class (spec §4(f)).
 * A caller handing this codec a frame of any other class is asking for a
 * field this class does not ground -- refuse, per the parent TU's own
 * INV-6 rule 2 (vms_cluster_codec.h). */
static int dlm_class_ok(const struct vms_frame_info *fi)
{
	return fi != (const struct vms_frame_info *)0 &&
	       fi->cls == VMS_FCLS_SCS_MSG;
}

/* ------------------------------------------------------------------ *
 * op 0x01 ENQ / op 0x07 CONVERT -- GROUNDED, spec §4(f).1
 * ------------------------------------------------------------------ */

static vms_codec_status_t dlm_name_get(vms_wire_view_t *v, uint8_t *len_out,
				       uint8_t *name_out)
{
	uint8_t len;

	len = vms_wire_get_u8(v, VMS_OFF_DLM_NAME_LEN);
	if (!vms_wire_view_ok(v))
		return v->err;
	if (len > VMS_DLM_NAME_MAX)
		return VMS_CODEC_E_RANGE;
	*len_out = len;
	vms_wire_get_bytes(v, VMS_OFF_DLM_NAME, len, name_out);
	if (!vms_wire_view_ok(v))
		return v->err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_request_parse(const uint8_t *frame, uint32_t len,
					     const struct vms_frame_info *fi,
					     uint8_t *opcode_out,
					     struct vms_dlm_enq_request *out)
{
	vms_wire_view_t v;
	uint8_t cat, op;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_enq_request *)0 ||
	    opcode_out == (uint8_t *)0 || !dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, frame, len);
	cat = vms_wire_get_u8(&v, VMS_OFF_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_OP_ENQ && op != VMS_DLM_OP_CONVERT)
		return VMS_CODEC_E_CLASS;

	out->mode = vms_wire_get_u8(&v, VMS_OFF_DLM_MODE);
	out->req_pid_or_lkid = vms_wire_get_le32(&v, VMS_OFF_DLM_REQ_LKID);
	out->master_lkid = vms_wire_get_le32(&v, VMS_OFF_DLM_MASTER_LKID);
	if (!vms_wire_view_ok(&v))
		return v.err;

	st = dlm_name_get(&v, &out->name_len, out->name);
	if (st != VMS_CODEC_OK)
		return st;

	*opcode_out = op;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_request_build(const struct vms_dlm_enq_request *req,
					     uint8_t opcode,
					     uint8_t *frame, uint32_t cap,
					     uint32_t *written)
{
	vms_wire_buf_t w;

	if (req == (const struct vms_dlm_enq_request *)0)
		return VMS_CODEC_E_INVAL;
	if (opcode != VMS_DLM_OP_ENQ && opcode != VMS_DLM_OP_CONVERT)
		return VMS_CODEC_E_INVAL;
	if (req->name_len > VMS_DLM_NAME_MAX)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT, VMS_DLM_CAT_REQUEST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, opcode);
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, req->mode);
	vms_wire_put_le32(&w, VMS_OFF_DLM_REQ_LKID, req->req_pid_or_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_MASTER_LKID, req->master_lkid);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_MARKER, VMS_DLM_NAME_MARKER_CONST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_LEN, req->name_len);
	vms_wire_put_bytes(&w, VMS_OFF_DLM_NAME, req->name_len, req->name);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_response_parse(const uint8_t *frame, uint32_t len,
					      const struct vms_frame_info *fi,
					      struct vms_dlm_enq_response *out)
{
	vms_wire_view_t v;
	uint8_t cat, op, mode;
	uint32_t lkid;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_enq_response *)0 || !dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, frame, len);
	cat = vms_wire_get_u8(&v, VMS_OFF_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (!vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_OP_ENQ && op != VMS_DLM_OP_CONVERT)
		return VMS_CODEC_E_CLASS;

	lkid = vms_wire_get_le32(&v, VMS_OFF_DLM_REQ_LKID);
	out->master_lkid = vms_wire_get_le32(&v, VMS_OFF_DLM_MASTER_LKID);
	mode = vms_wire_get_u8(&v, VMS_OFF_DLM_MODE);
	if (!vms_wire_view_ok(&v))
		return v.err;

	/*
	 * The grant/deny SHAPE discriminator (spec §4(f).1 "Completion
	 * status"): DENIED clears the mode byte to 0 AND echoes the name;
	 * GRANTED does neither. Testing the mode alone would misclassify a
	 * genuine NL(=0) grant as a denial, so both conditions are required
	 * -- exactly the two-signal test the spec's own byte-diff used.
	 */
	st = dlm_name_get(&v, &out->name_len, out->name);
	if (st != VMS_CODEC_OK)
		return st;

	if (mode == 0 && out->name_len != 0) {
		out->outcome = VMS_DLM_ENQ_DENIED;
		out->req_lkid = lkid;      /* echoed PID placeholder */
		out->granted_mode = 0;
	} else {
		out->outcome = VMS_DLM_ENQ_GRANTED;
		out->req_lkid = lkid;      /* the real assigned lock-id */
		out->granted_mode = mode;
		out->name_len = 0;         /* spec: grant does not echo the name */
	}
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_response_build_grant(uint32_t req_lkid,
						    uint32_t master_lkid,
						    uint8_t granted_mode,
						    uint8_t *frame, uint32_t cap,
						    uint32_t *written)
{
	vms_wire_buf_t w;

	/* A GRANT that hands the requester lock-id 0 is not a grant -- the
	 * executive's own DLM never assigns lkid 0 to an established lock
	 * (see the file header's fc8540ae lesson). */
	if (req_lkid == VMS_DLM_LKID_UNSET)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT,
			vms_wire_response_category(VMS_DLM_CAT_REQUEST));
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_OP_ENQ);
	vms_wire_put_le32(&w, VMS_OFF_DLM_REQ_LKID, req_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_MASTER_LKID, master_lkid);
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, granted_mode);
	/* Name span deliberately untouched: spec grounds "does not echo the
	 * resource name" for the granted shape. */

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_response_build_deny(uint32_t req_pid_echo,
						   uint32_t master_lkid,
						   uint8_t name_len,
						   const uint8_t *name,
						   uint8_t *frame, uint32_t cap,
						   uint32_t *written)
{
	vms_wire_buf_t w;

	if (name_len > VMS_DLM_NAME_MAX || (name_len > 0 && name == (const uint8_t *)0))
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT,
			vms_wire_response_category(VMS_DLM_CAT_REQUEST));
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_OP_ENQ);
	vms_wire_put_le32(&w, VMS_OFF_DLM_REQ_LKID, req_pid_echo);
	vms_wire_put_le32(&w, VMS_OFF_DLM_MASTER_LKID, master_lkid);
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, 0); /* cleared, spec grounded */
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_MARKER, VMS_DLM_NAME_MARKER_CONST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_LEN, name_len);
	vms_wire_put_bytes(&w, VMS_OFF_DLM_NAME, name_len, name);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_req_csid(const struct vms_sca_hdr *hdr, uint16_t *out)
{
	if (hdr == (const struct vms_sca_hdr *)0)
		return VMS_CODEC_E_INVAL;
	return vms_cluster_lavc_sysid(hdr->src_lavc, out);
}

/* ------------------------------------------------------------------ *
 * op 0x0d lock-resource rebuild record -- GROUNDED, spec §4(p)
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_dlm_rebuild_parse(const uint8_t *frame, uint32_t len,
					 const struct vms_frame_info *fi,
					 struct vms_dlm_rebuild_record *out)
{
	vms_wire_view_t v;
	uint8_t cat, op;
	uint16_t inv1, inv2;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_rebuild_record *)0 || !dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, frame, len);
	cat = vms_wire_get_u8(&v, VMS_OFF_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFF_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_OP_REBUILD)
		return VMS_CODEC_E_CLASS;

	inv1 = vms_wire_get_le16(&v, VMS_OFF_DLM_REBUILD_INV1);
	inv2 = vms_wire_get_le16(&v, VMS_OFF_DLM_REBUILD_INV2);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (inv1 != VMS_DLM_REBUILD_INV1_CONST || inv2 != VMS_DLM_REBUILD_INV2_CONST)
		return VMS_CODEC_E_CLASS;

	/* The whole body span, verbatim -- the exact source the response
	 * recipe's "memcpy 132 bytes" copies. Body starts at abs
	 * VMS_OFF_SYSAP_BODY (72). */
	vms_wire_get_bytes(&v, VMS_OFF_SYSAP_BODY, VMS_DLM_REBUILD_ECHO_LEN,
			   out->body);
	if (!vms_wire_view_ok(&v))
		return v.err;

	st = dlm_name_get(&v, &out->name_len, out->name);
	if (st != VMS_CODEC_OK)
		return st;

	return VMS_CODEC_OK;
}

vms_codec_status_t
vms_dlm_rebuild_response_build(const struct vms_dlm_rebuild_record *req,
			       uint16_t own_send_msg, uint16_t ack_of_peer_send,
			       uint8_t *frame, uint32_t cap, uint32_t *written)
{
	vms_wire_buf_t w;
	uint8_t cat;

	if (req == (const struct vms_dlm_rebuild_record *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* Step 1: memcpy(resp_body, req_body, 132) -- VERBATIM echo, spec
	 * §4(p). Everything not explicitly overwritten below (txn/checksum,
	 * opcode, body[12:16], the L1 region, the resource name) rides
	 * through unchanged, exactly as the recipe requires. Do NOT apply
	 * any cat-0x01-style field mutation here (spec's own warning). */
	vms_wire_put_bytes(&w, VMS_OFF_SYSAP_BODY, VMS_DLM_REBUILD_ECHO_LEN,
			   req->body);
	if (!vms_wire_buf_ok(&w))
		return w.err;

	/* Step 2: the four grounded mutations, applied to the copy. */
	vms_wire_put_le16(&w, VMS_OFF_DLM_SEND_MSG, own_send_msg);
	vms_wire_put_le16(&w, VMS_OFF_DLM_ACK_MSG, ack_of_peer_send);
	cat = req->body[VMS_OFF_DLM_CAT - VMS_OFF_SYSAP_BODY]; /* the echoed 0x02 */
	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT, vms_wire_response_category(cat));
	vms_wire_put_u8(&w, VMS_OFF_DLM_RESULT_STAMP, VMS_DLM_RESULT_STAMP_REBUILD);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_DLM_REBUILD_ECHO_LEN;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * op 0x04 / op 0x03 completion + commit -- PROVISIONAL (see header)
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_dlm_completion_build(const struct vms_dlm_completion *c,
					    uint8_t op,
					    uint8_t *frame, uint32_t cap,
					    uint32_t *written)
{
	vms_wire_buf_t w;

	if (c == (const struct vms_dlm_completion *)0)
		return VMS_CODEC_E_INVAL;
	if (op != VMS_DLM_OP_COMPLETE_PROVISIONAL &&
	    op != VMS_DLM_OP_COMMIT_PROVISIONAL)
		return VMS_CODEC_E_INVAL;
	if (c->name_len > VMS_DLM_NAME_MAX)
		return VMS_CODEC_E_INVAL;

	/*
	 * THE HARD-LESSON GATE. A zero here is never a real LKB/RSB handle
	 * (vms_lock.c's own convention; see the file header doc comment).
	 * fc8540ae shipped a completion with master_lkid == a literal
	 * placeholder and bugchecked a real VAX with INVLOCKID; this codec
	 * cannot detect every possible fabricated nonzero value, but it can
	 * and does refuse the one value that structurally can never be a
	 * real assigned lock-id.
	 */
	if (c->master_lkid == VMS_DLM_LKID_UNSET ||
	    c->req_lkid == VMS_DLM_LKID_UNSET)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT, VMS_DLM_CAT_REQUEST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, op);
	vms_wire_put_le32(&w, VMS_OFF_DLM_COMPLETE_STATUS,
			  VMS_DLM_COMPLETE_STATUS_CONST);
	vms_wire_put_le32(&w, VMS_OFF_DLM_COMPLETE_MASTER_LKID, c->master_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_COMPLETE_REQ_LKID, c->req_lkid);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_MARKER, VMS_DLM_NAME_MARKER_CONST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_LEN, c->name_len);
	vms_wire_put_bytes(&w, VMS_OFF_DLM_NAME, c->name_len, c->name);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * The allowlist rows this item contributes (GROUNDED ops only).
 * ------------------------------------------------------------------ */

const struct vms_wire_allow_entry vms_dlm_allow_rows[] = {
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_OP_ENQ,
	  VMS_WIRE_ACT_RESPOND, 1u, "spec §4(f).1" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_OP_CONVERT,
	  VMS_WIRE_ACT_RESPOND, 2u, "spec §4(f).1" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_OP_REBUILD,
	  VMS_WIRE_ACT_RESPOND, 3u, "spec §4(p) cat 0x02 op 0x0d" },
};

const struct vms_wire_allow_table vms_dlm_allow_table = {
	vms_dlm_allow_rows,
	(uint16_t)(sizeof(vms_dlm_allow_rows) / sizeof(vms_dlm_allow_rows[0]))
};
