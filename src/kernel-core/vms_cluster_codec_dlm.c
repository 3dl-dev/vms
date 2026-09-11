// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_dlm.c - cat-0x02 (DLM) typed codec entries (FC-P4.5).
 *
 * Read vms_cluster_codec_dlm.h first: it draws the GROUNDED/OBSERVED line
 * field by field, records the vms-c03 supersession (the "completion 0x04 +
 * commit 0x03" pair was a phantom; 0x03 is $DEQ, 0x04 is BLKAST, 0x06
 * carries the value block), and carries the fc8540ae hard-lesson doc
 * comment that motivates the lock-id refusals below.
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

	len = vms_wire_get_u8(v, VMS_OFB_DLM_NAME_LEN);
	if (!vms_wire_view_ok(v))
		return v->err;
	if (len > VMS_DLM_NAME_MAX)
		return VMS_CODEC_E_RANGE;
	*len_out = len;
	vms_wire_get_bytes(v, VMS_OFB_DLM_NAME, len, name_out);
	if (!vms_wire_view_ok(v))
		return v->err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_request_parse_body(const uint8_t *body, uint32_t len,
					     uint8_t *opcode_out,
					     struct vms_dlm_enq_request *out)
{
	vms_wire_view_t v;
	uint8_t cat, op;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_enq_request *)0 ||
	    opcode_out == (uint8_t *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_WIREOP_ENQ && op != VMS_DLM_WIREOP_CONVERT)
		return VMS_CODEC_E_CLASS;

	out->mode = vms_wire_get_u8(&v, VMS_OFB_DLM_MODE);
	out->req_pid_or_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_REQ_LKID);
	out->master_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_MASTER_LKID);
	/* body[10:12]: the SENDER's own directory hash for the root name. A
	 * request that carried one is where a receiver LEARNS it (FC-P4.3,
	 * Davis p. 6-50); the flag is what tells the caller it may be learned
	 * at all, because "0" and "absent" are different facts. */
	out->dir_hash = vms_wire_get_le16(&v, VMS_OFB_DLM_DIR_HASH);
	out->dir_hash_valid = 1u;
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
	if (opcode != VMS_DLM_WIREOP_ENQ && opcode != VMS_DLM_WIREOP_CONVERT)
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
	/*
	 * body[10:12] ONLY when the caller holds a WIRE-LEARNED hash for this
	 * root name. No `else` branch, on purpose: a zero written here would be
	 * a hash nobody derived, the directory node would scan the wrong chain,
	 * miss the name, and install US as master of a resource somebody else
	 * already masters -- the campaign's 35/s grant storm (FC-P4.1 §3).
	 */
	if (req->dir_hash_valid)
		vms_wire_put_le16(&w, VMS_OFF_DLM_DIR_HASH, req->dir_hash);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_MARKER, VMS_DLM_NAME_MARKER_CONST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_LEN, req->name_len);
	vms_wire_put_bytes(&w, VMS_OFF_DLM_NAME, req->name_len, req->name);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_response_parse_body(const uint8_t *body, uint32_t len,
					      struct vms_dlm_enq_response *out)
{
	vms_wire_view_t v;
	uint8_t cat, op, mode;
	uint32_t lkid;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_enq_response *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (!vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_WIREOP_ENQ && op != VMS_DLM_WIREOP_CONVERT)
		return VMS_CODEC_E_CLASS;

	lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_REQ_LKID);
	out->master_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_MASTER_LKID);
	mode = vms_wire_get_u8(&v, VMS_OFB_DLM_MODE);
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
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_WIREOP_ENQ);
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
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_WIREOP_ENQ);
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
 * The directory hash at body[10:12] -- read only, never built.
 * See the header for the p. 6-50 grounding and the INFERRED offset.
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_dlm_dir_hash_parse_body(const uint8_t *body, uint32_t len,
					  uint16_t *out)
{
	vms_wire_view_t v;
	uint8_t cat;
	uint16_t hash;

	if (out == (uint16_t *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	if (!vms_wire_view_ok(&v))
		return v.err;
	/* Requests (0x02) and responses (0x82) alike: the value is a property
	 * of the resource name, and every cat-0x02 frame that names one is a
	 * chance to learn it. */
	if ((cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;

	hash = vms_wire_get_le16(&v, VMS_OFB_DLM_DIR_HASH);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*out = hash;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * op 0x0d lock-resource rebuild record -- GROUNDED, spec §4(p)
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_dlm_rebuild_parse_body(const uint8_t *body, uint32_t len,
					 struct vms_dlm_rebuild_record *out)
{
	vms_wire_view_t v;
	uint8_t cat, op;
	uint16_t inv1, inv2;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_rebuild_record *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_WIREOP_REBUILD)
		return VMS_CODEC_E_CLASS;

	inv1 = vms_wire_get_le16(&v, VMS_OFB_DLM_REBUILD_INV1);
	inv2 = vms_wire_get_le16(&v, VMS_OFB_DLM_REBUILD_INV2);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (inv1 != VMS_DLM_REBUILD_INV1_CONST || inv2 != VMS_DLM_REBUILD_INV2_CONST)
		return VMS_CODEC_E_CLASS;

	/* The whole body span, verbatim -- the exact source the response
	 * recipe's "memcpy 132 bytes" copies. Body starts at abs
	 * VMS_OFF_SYSAP_BODY (72). */
	vms_wire_get_bytes(&v, 0u, VMS_DLM_REBUILD_ECHO_LEN,
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
 * op 0x03 $DEQ / op 0x04 BLKAST / op 0x06 CONVERT-with-VALBLK
 * -- GROUNDED, vms-c03 capture set. Read the header's section comment
 *    first: it names the pcap, the frame and the correlating $ENQ for
 *    every offset below, and it says which ONE field is only OBSERVED.
 * ------------------------------------------------------------------ */

/*
 * The shared preamble of all three: gate the category and the opcode, then
 * read the two lock ids -- which are the SAME body[20]/body[24] the ENQ
 * family uses (header section comment: the capture proves it, byte for
 * byte, against the driving $ENQ).
 *
 * THE PARSE-SIDE LOCK-ID REFUSAL. These three messages identify their lock
 * by lock-id and by nothing else, so a zero in either field leaves the
 * message meaning nothing at all. The vms-c03 captures contain real cat-0x02
 * op-0x04 frames with master_lkid == 0; handing one up as "a BLKAST for lock
 * 0" would be manufacturing a referent. Refused instead.
 */
static vms_codec_status_t dlm_lkid_pair_get(vms_wire_view_t *v, uint8_t want_op,
					    uint32_t *req_lkid,
					    uint32_t *master_lkid)
{
	uint8_t cat, op;

	cat = vms_wire_get_u8(v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(v))
		return v->err;
	if (vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != want_op)
		return VMS_CODEC_E_CLASS;

	*req_lkid = vms_wire_get_le32(v, VMS_OFB_DLM_REQ_LKID);
	*master_lkid = vms_wire_get_le32(v, VMS_OFB_DLM_MASTER_LKID);
	if (!vms_wire_view_ok(v))
		return v->err;
	if (*req_lkid == VMS_DLM_LKID_UNSET ||
	    *master_lkid == VMS_DLM_LKID_UNSET)
		return VMS_CODEC_E_RANGE;
	return VMS_CODEC_OK;
}

/* The mirror of the above for a builder: cat/op plus the two lock ids,
 * with the fc8540ae refusal applied before a single byte is written. */
static vms_codec_status_t dlm_lkid_pair_put(vms_wire_buf_t *w, uint8_t op,
					    uint32_t req_lkid,
					    uint32_t master_lkid)
{
	if (req_lkid == VMS_DLM_LKID_UNSET ||
	    master_lkid == VMS_DLM_LKID_UNSET)
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(w, VMS_OFF_DLM_CAT, VMS_DLM_CAT_REQUEST);
	vms_wire_put_u8(w, VMS_OFF_DLM_OP, op);
	vms_wire_put_le32(w, VMS_OFF_DLM_REQ_LKID, req_lkid);
	vms_wire_put_le32(w, VMS_OFF_DLM_MASTER_LKID, master_lkid);
	return vms_wire_buf_ok(w) ? VMS_CODEC_OK : w->err;
}

vms_codec_status_t vms_dlm_deq_parse_body(const uint8_t *body, uint32_t len,
					  struct vms_dlm_deq *out)
{
	vms_wire_view_t v;
	struct vms_dlm_deq d;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_deq *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	st = dlm_lkid_pair_get(&v, VMS_DLM_WIREOP_DEQ, &d.req_lkid,
			       &d.master_lkid);
	if (st != VMS_CODEC_OK)
		return st;

	/* body[30]: the mode the lock is released FROM. The mode byte's
	 * offset is the ac4-grounded one; the vms-c03 DEQ reads 0x00 (NL)
	 * there, matching the mode its own op-0x01 grant assigned. */
	d.mode = vms_wire_get_u8(&v, VMS_OFB_DLM_MODE);
	if (!vms_wire_view_ok(&v))
		return v.err;

	/* NO NAME IS READ. body[46] is not the 0x03 marker on a real DEQ and
	 * body[47:] is uninitialised (header section comment). */
	*out = d;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_deq_build(const struct vms_dlm_deq *d,
				     uint8_t *frame, uint32_t cap,
				     uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (d == (const struct vms_dlm_deq *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	st = dlm_lkid_pair_put(&w, VMS_DLM_WIREOP_DEQ, d->req_lkid,
			       d->master_lkid);
	if (st != VMS_CODEC_OK)
		return st;
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, d->mode);
	/* The name span is deliberately left untouched: a real DEQ carries
	 * no resource name, so writing one would be adding a field the
	 * reference does not have. */

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_blkast_parse_body(const uint8_t *body, uint32_t len,
					     struct vms_dlm_blkast *out)
{
	vms_wire_view_t v;
	struct vms_dlm_blkast b;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_blkast *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	st = dlm_lkid_pair_get(&v, VMS_DLM_WIREOP_BLKAST, &b.req_lkid,
			       &b.master_lkid);
	if (st != VMS_CODEC_OK)
		return st;

	/* body[30:32] -- OBSERVED, NOT PINNED. Reported verbatim, flagged as
	 * present, and NOT interpreted as a lock mode anywhere. */
	vms_wire_get_bytes(&v, VMS_OFB_DLM_BLKAST_MODE_CTX,
			   VMS_DLM_BLKAST_MODE_CTX_LEN, b.mode_ctx);
	if (!vms_wire_view_ok(&v))
		return v.err;
	b.mode_ctx_valid = 1u;

	/* NO NAME IS READ. body[48] on the reference BLKAST holds a stale
	 * 'F11B$aSYSDSK1' that belongs to a different lock entirely. */
	*out = b;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_blkast_build(const struct vms_dlm_blkast *b,
					uint8_t *frame, uint32_t cap,
					uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (b == (const struct vms_dlm_blkast *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	st = dlm_lkid_pair_put(&w, VMS_DLM_WIREOP_BLKAST, b->req_lkid,
			       b->master_lkid);
	if (st != VMS_CODEC_OK)
		return st;

	/*
	 * body[30:32] ONLY when the caller says it holds real executive
	 * values for the pair. No `else` branch, deliberately: this field is
	 * OBSERVED, not pinned, and two bytes written there from nothing
	 * would be exactly the kind of plausible-looking invention that the
	 * directory hash's own no-builder rule exists to prevent.
	 */
	if (b->mode_ctx_valid)
		vms_wire_put_bytes(&w, VMS_OFF_DLM_BLKAST_MODE_CTX,
				   VMS_DLM_BLKAST_MODE_CTX_LEN, b->mode_ctx);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

/*
 * op 0x06 CONVERT-with-VALBLK: parse (read the value block a peer sent) and,
 * since vms-727, build (emit our own holder's value-block flush). body[32:36]
 * is no longer un-pinned: body[34]=0x01 is the cat-0x02 request stamp and
 * body[32]=body[52] is a per-lock SERIAL sourced from the LKB -- see the
 * header's op-0x06 BUILD layout comment.
 */
vms_codec_status_t
vms_dlm_valblk_convert_parse_body(const uint8_t *body, uint32_t len,
				  struct vms_dlm_valblk_convert *out)
{
	vms_wire_view_t v;
	struct vms_dlm_valblk_convert c;
	vms_codec_status_t st;

	if (out == (struct vms_dlm_valblk_convert *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	st = dlm_lkid_pair_get(&v, VMS_DLM_WIREOP_CONVERT_VALBLK, &c.req_lkid,
			       &c.master_lkid);
	if (st != VMS_CODEC_OK)
		return st;

	c.mode = vms_wire_get_u8(&v, VMS_OFB_DLM_MODE);
	c.serial = vms_wire_get_u8(&v, VMS_OFB_DLM_VALBLK_SERIAL);
	vms_wire_get_bytes(&v, VMS_OFB_DLM_VALBLK, VMS_DLM_VALBLK_WIRE_LEN,
			   c.valblk);
	if (!vms_wire_view_ok(&v))
		return v.err;

	*out = c;
	return VMS_CODEC_OK;
}

/*
 * Build an op-0x06 CONVERT-with-VALBLK. Mirrors the DEQ/BLKAST builders: the
 * two lock ids must be real (a value-block flush naming lock 0 is nothing).
 * Every byte written is grounded (vms-727) -- the SERIAL from the LKB, the
 * value block from the LKB, and constants verified byte-for-byte across the
 * five real-wire captures. body[56:88] is zero-filled: the real sender pads
 * it with uninitialised stack, which is not a field to reproduce.
 */
vms_codec_status_t vms_dlm_valblk_convert_build(const struct vms_dlm_valblk_convert *c,
						uint8_t *frame, uint32_t cap,
						uint32_t *written)
{
	vms_wire_buf_t w;

	if (c == (const struct vms_dlm_valblk_convert *)0)
		return VMS_CODEC_E_INVAL;
	if (c->req_lkid == VMS_DLM_LKID_UNSET ||
	    c->master_lkid == VMS_DLM_LKID_UNSET)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT, VMS_DLM_CAT_REQUEST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_WIREOP_CONVERT_VALBLK);
	vms_wire_put_le16(&w, VMS_OFF_DLM_VALBLK_HDR1, VMS_DLM_VALBLK_HDR1_VAL);
	vms_wire_put_le16(&w, VMS_OFF_DLM_VALBLK_HDR2, VMS_DLM_VALBLK_HDR2_VAL);
	vms_wire_put_le32(&w, VMS_OFF_DLM_REQ_LKID, c->req_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_MASTER_LKID, c->master_lkid);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_FLAG, VMS_DLM_VALBLK_FLAG_VAL);
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, c->mode);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_SERIAL, c->serial);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_REQSTAMP, VMS_DLM_VALBLK_REQSTAMP_VAL);
	vms_wire_put_bytes(&w, VMS_OFF_DLM_VALBLK, VMS_DLM_VALBLK_WIRE_LEN,
			   c->valblk);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_SERIAL2, c->serial);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_TAG2, VMS_DLM_VALBLK_TAG2_VAL);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_PAD, VMS_DLM_VALBLK_PAD_VAL);
	vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK_PAD + 1u, VMS_DLM_VALBLK_PAD_VAL);
	/*
	 * body[56:88] is uninitialised sender buffer on the real wire, not a
	 * field. Write clean zeros explicitly (rather than trust a caller-
	 * zeroed scratch) so the emit never leaks our own memory AND so the
	 * length claim below is bounds-checked: this run fails the buffer if
	 * `cap` cannot hold the full op-0x06 body.
	 */
	{
		static const uint8_t zeros[VMS_DLM_VALBLK_BODY_LEN - 56u] = { 0 };
		vms_wire_put_bytes(&w,
				   VMS_OFF_SYSAP_BODY + 56u,
				   (uint32_t)sizeof(zeros), zeros);
	}

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_OFF_SYSAP_BODY + VMS_DLM_VALBLK_BODY_LEN;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * The allowlist rows this item contributes (GROUNDED ops only).
 * ------------------------------------------------------------------ */

const struct vms_wire_allow_entry vms_dlm_allow_rows[] = {
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_ENQ,
	  VMS_WIRE_ACT_RESPOND, 1u, "spec §4(f).1" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_CONVERT,
	  VMS_WIRE_ACT_RESPOND, 2u, "spec §4(f).1" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_REBUILD,
	  VMS_WIRE_ACT_RESPOND, 3u, "spec §4(p) cat 0x02 op 0x0d" },
	/* CONSUME, recipe 0: the vms-c03 capture contains no cat-0x82 reply to
	 * any of these three. Claiming RESPOND would be claiming a response
	 * recipe no reference cluster has ever been seen to use. */
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_DEQ,
	  VMS_WIRE_ACT_CONSUME, 0u, "vms-c03 dlm-deq-20260911.pcap f14" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_BLKAST,
	  VMS_WIRE_ACT_CONSUME, 0u, "vms-c03 dlm-blk2-20260911.pcap f58" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, VMS_DLM_CAT_REQUEST,
	  VMS_DLM_WIREOP_CONVERT_VALBLK,
	  VMS_WIRE_ACT_CONSUME, 0u, "vms-c03 dlm-lvb3-20260911.pcap f14" },
};

const struct vms_wire_allow_table vms_dlm_allow_table = {
	vms_dlm_allow_rows,
	(uint16_t)(sizeof(vms_dlm_allow_rows) / sizeof(vms_dlm_allow_rows[0]))
};

/* ------------------------------------------------------------------ *
 * THE FRAME-ABSOLUTE ENTRIES, over the SAME implementation.
 *
 * Each one checks the class the way a captured frame allows (vms_frame_info)
 * and then slices the SYSAP body off and hands it to the core above. One set
 * of field reads, two ways in -- so a body parsed off the wire and a frame
 * parsed out of a capture can never disagree about where a field is.
 * ------------------------------------------------------------------ */

/* A frame is long enough to have a body at all, and the body it has. */
static vms_codec_status_t dlm_body_of(const uint8_t *frame, uint32_t len,
				      const uint8_t **out_body,
				      uint32_t *out_len)
{
	if (frame == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (len <= (uint32_t)VMS_OFF_SYSAP_BODY)
		return VMS_CODEC_E_SHORT;
	*out_body = frame + VMS_OFF_SYSAP_BODY;
	*out_len = len - (uint32_t)VMS_OFF_SYSAP_BODY;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_request_parse(const uint8_t *frame, uint32_t len,
					     const struct vms_frame_info *fi,
					     uint8_t *opcode_out,
					     struct vms_dlm_enq_request *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_enq_request_parse_body(body, blen, opcode_out, out);
}

vms_codec_status_t vms_dlm_enq_response_parse(const uint8_t *frame, uint32_t len,
					      const struct vms_frame_info *fi,
					      struct vms_dlm_enq_response *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_enq_response_parse_body(body, blen, out);
}

vms_codec_status_t vms_dlm_dir_hash_parse(const uint8_t *frame, uint32_t len,
					  const struct vms_frame_info *fi,
					  uint16_t *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_dir_hash_parse_body(body, blen, out);
}

vms_codec_status_t vms_dlm_rebuild_parse(const uint8_t *frame, uint32_t len,
					 const struct vms_frame_info *fi,
					 struct vms_dlm_rebuild_record *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_rebuild_parse_body(body, blen, out);
}

vms_codec_status_t vms_dlm_deq_parse(const uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     struct vms_dlm_deq *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_deq_parse_body(body, blen, out);
}

vms_codec_status_t vms_dlm_blkast_parse(const uint8_t *frame, uint32_t len,
					const struct vms_frame_info *fi,
					struct vms_dlm_blkast *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_blkast_parse_body(body, blen, out);
}

vms_codec_status_t
vms_dlm_valblk_convert_parse(const uint8_t *frame, uint32_t len,
			     const struct vms_frame_info *fi,
			     struct vms_dlm_valblk_convert *out)
{
	const uint8_t *body;
	uint32_t blen;
	vms_codec_status_t st;

	if (!dlm_class_ok(fi))
		return VMS_CODEC_E_CLASS;
	st = dlm_body_of(frame, len, &body, &blen);
	if (st != VMS_CODEC_OK)
		return st;
	return vms_dlm_valblk_convert_parse_body(body, blen, out);
}
