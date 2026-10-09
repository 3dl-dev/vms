// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_dlm.c - cat-0x02 (DLM) typed codec entries (FC-P4.5).
 *
 * Read vms_cluster_codec_dlm.h first: it draws the GROUNDED/OBSERVED line
 * field by field, records the vms-c03 supersession (the "completion 0x04 +
 * commit 0x03" pair was a phantom; 0x03 is $DEQ, 0x05 is BLKAST -- rd
 * vms-ea1 -- and 0x06 carries the value block), and carries the fc8540ae hard-lesson doc
 * comment that motivates the lock-id refusals below.
 *
 * Pure, like the parent TU and the HELLO family file: no state, no
 * allocation, no substrate call, no libc beyond the vms_wire_* primitives
 * the parent TU exports.
 */

#include "vms_cluster_codec_dlm.h"
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN: the directory answer echoes a whole body */

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

/* Defined with the directory-hash section below (the parent span lives there);
 * the ENQ parser needs it to tell a ROOT request from a sub-resource one. */
static int dlm_is_root(vms_wire_view_t *v);

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
	out->req_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_REQ_LKID);
	out->master_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_MASTER_LKID);
	/* body[128:132]: the SENDER's own directory hash for the name (rd
	 * vms-4fb, Davis p. 6-50); the flag is what tells the caller it may be
	 * learned at all, because "0" and "absent" are different facts. */
	out->dir_hash = vms_wire_get_le32(&v, VMS_OFB_DLM_DIR_HASH);
	out->dir_hash_valid = 1u;
	/*
	 * The identity that QUALIFIES the name (rd vms-b5b0). Read for both
	 * opcodes -- the bytes are there either way -- but marked VALID only for
	 * an op-0x01 ROOT request: see the struct's note on op-0x07's stale
	 * name/identity span.
	 */
	out->res_group = vms_wire_get_le16(&v, VMS_OFB_DLM_RES_GROUP);
	out->res_acmode = vms_wire_get_u8(&v, VMS_OFB_DLM_RES_MODE);
	if (!vms_wire_view_ok(&v))
		return v.err;
	out->res_ident_valid = (op == VMS_DLM_WIREOP_ENQ && dlm_is_root(&v))
			       ? 1u : 0u;
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
	/*
	 * body[44:48] IS NOT OPTIONAL on a frame that names a resource (rd
	 * vms-b5b0): the UIC group and the access mode are what say WHICH
	 * resource of that name is meant, and a zero group on a frame for a
	 * group-qualified resource makes the receiving directory scan for a
	 * resource nobody has. The engine always holds both (they are part of
	 * the key its resource block was created under), so a caller that
	 * cannot state them is a caller with no resource -- refused here, the
	 * same shape as the codec's refusal of lock id 0.
	 */
	if (!req->res_ident_valid)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT, VMS_DLM_CAT_REQUEST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, opcode);
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, req->mode);
	vms_wire_put_le32(&w, VMS_OFF_DLM_REQ_LKID, req->req_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_MASTER_LKID, req->master_lkid);
	/*
	 * body[128:132] ONLY when the caller holds a WIRE-LEARNED hash for this
	 * root name. No `else` branch, on purpose: a zero written here would be
	 * a hash nobody derived, the directory node would scan the wrong chain,
	 * miss the name, and install US as master of a resource somebody else
	 * already masters -- the campaign's 35/s grant storm (FC-P4.1 §3).
	 */
	if (req->dir_hash_valid)
		vms_wire_put_le32(&w, VMS_OFF_DLM_DIR_HASH, req->dir_hash);
	/*
	 * body[44:46] + body[46]: the identity that qualifies the name, and only
	 * when the caller says it holds one. A zero group on a frame for a
	 * group-qualified resource would make the directory node scan for a
	 * resource nobody has -- the same class of error as a zero hash.
	 */
	vms_wire_put_le16(&w, VMS_OFF_DLM_RES_GROUP, req->res_group);
	vms_wire_put_u8(&w, VMS_OFF_DLM_RES_MODE, req->res_acmode);
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

	out->valblk_present = 0;

	lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_REQ_LKID);
	out->master_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_MASTER_LKID);
	mode = vms_wire_get_u8(&v, VMS_OFB_DLM_MODE);
	if (!vms_wire_view_ok(&v))
		return v.err;

	/*
	 * THE GRANT RECORD, CHECKED FIRST, AND IT IS ON EVERY GRANT (rd
	 * vms-b5b0; 38 of 38 real grants). body[28]==0x10 AND
	 * body[32:36]=={01 00 fa 00} -- whose body[34] is the 0xfa GRANTED
	 * outcome -- is what says "granted"; it has to be read before the name
	 * because a grant clears body[47] and leaves body[48:56] as its own
	 * buffer, which dlm_name_get would read as a bogus length.
	 *
	 * IT IS THE GRANT'S RECORD, NOT A "HAS A VALUE BLOCK" MARKER (which is
	 * what this codec used to call it, vms-727). Every grant carries it,
	 * including grants for resources whose value block is all zero -- so it
	 * cannot mark the block's PRESENCE. What it does mark is the grant, and
	 * body[36:52] inside it IS the master resource's value block: proven by
	 * the dlm-grant-valblk specimen, a real c2-seq grant that returns the
	 * exact 16 bytes the requester had written. So `valblk_present` stays 1
	 * whenever the record is there, and the engine records the master's
	 * block -- zeros included, because for a resource whose block is zero
	 * that is the data and not a placeholder.
	 *
	 * AND A GRANT CARRIES NO MODE: body[30] is cleared in 38 of 38. The
	 * requester asked for a mode and a grant means that mode, which only the
	 * requester's own LKB holds -- so `granted_mode` is reported as 0 and
	 * `granted_mode_present` as 0, and the engine grants what the lock
	 * requested.
	 */
	{
		uint8_t  f   = vms_wire_get_u8(&v, VMS_OFB_DLM_GRANT_FLAG);
		uint32_t rec = vms_wire_get_le32(&v, VMS_OFB_DLM_GRANT_REC);

		if (!vms_wire_view_ok(&v))
			return v.err;
		if (f == VMS_DLM_GRANT_FLAG_VAL && rec == VMS_DLM_GRANT_REC_VAL) {
			vms_wire_get_bytes(&v, VMS_OFB_DLM_VALBLK,
					   VMS_DLM_VALBLK_WIRE_LEN, out->valblk);
			if (!vms_wire_view_ok(&v))
				return v.err;
			out->outcome = VMS_DLM_ENQ_GRANTED;
			out->req_lkid = lkid;
			out->granted_mode = 0;
			out->granted_mode_present = 0;
			out->name_len = 0;      /* a grant carries no name */
			out->valblk_present = 1;
			return VMS_CODEC_OK;
		}
	}

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
		out->req_lkid = lkid;      /* echoed, the requester's own handle */
		out->granted_mode = 0;
		out->granted_mode_present = 0;
	} else {
		/*
		 * A grant WITHOUT the grant record above. No real grant looks
		 * like this (38 of 38 carry it), so this arm survives only for
		 * the shapes OVMX's own older builders produced and for a
		 * capture that has not been seen yet. The mode is reported as
		 * the wire's, flagged present, because that is what such a
		 * frame asserts.
		 */
		out->outcome = VMS_DLM_ENQ_GRANTED;
		out->req_lkid = lkid;      /* the requester's own handle */
		out->granted_mode = mode;
		out->granted_mode_present = 1;
		out->name_len = 0;         /* spec: grant does not echo the name */
	}
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_response_build_grant(const uint8_t *req_body,
						    uint32_t req_len,
						    uint32_t master_lkid,
						    const uint8_t *valblk,
						    uint8_t *frame, uint32_t cap,
						    uint32_t *written)
{
	vms_wire_view_t v;
	vms_wire_buf_t w;
	uint8_t cat, op, req_name_len;
	uint32_t i;

	/* A GRANT that hands out lock-id 0 is not a grant -- the executive's own
	 * DLM never assigns lkid 0 to an established lock (the fc8540ae lesson
	 * in this file's header). */
	if (master_lkid == VMS_DLM_LKID_UNSET)
		return VMS_CODEC_E_INVAL;
	if (req_body == (const uint8_t *)0 || req_len < VMS_CM_BODY_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_view_init(&v, req_body, req_len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	/* The request's OWN name length: it says how much of the name span the
	 * grant has to clear (below). Read from the request, never assumed. */
	req_name_len = vms_wire_get_u8(&v, VMS_OFB_DLM_NAME_LEN);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (req_name_len > VMS_DLM_NAME_MAX)
		req_name_len = VMS_DLM_NAME_MAX;
	if (cat != VMS_DLM_CAT_REQUEST ||
	    (op != VMS_DLM_WIREOP_ENQ && op != VMS_DLM_WIREOP_CONVERT))
		return VMS_CODEC_E_CLASS;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/*
	 * THE REQUEST, ECHOED. Everything the master does not rewrite is the
	 * requester's own bytes -- including the directory hash at body[128:132]
	 * and the identity at body[44:48], which is exactly what 38 of 38 real
	 * grants do. body[0:8] is the CM's envelope and the wrapper owns it.
	 */
	vms_wire_put_bytes(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN, req_body);
	if (!vms_wire_buf_ok(&w))
		return w.err;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT,
			vms_wire_response_category(VMS_DLM_CAT_REQUEST));
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_WIREOP_ENQ);
	/* body[20:24]: the handle THIS master assigned. body[24:28] is left as
	 * the requester's own, echoed -- the correlation. */
	vms_wire_put_le32(&w, VMS_OFF_DLM_MASTER_LKID, master_lkid);
	/* The grant record, on every grant (38/38). */
	vms_wire_put_u8(&w, VMS_OFF_DLM_GRANT_FLAG, VMS_DLM_GRANT_FLAG_VAL);
	vms_wire_put_u8(&w, VMS_OFF_DLM_GRANT_FLAG + 1u, 0); /* body[29], 38/38 */
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE, 0);          /* cleared, 38/38 */
	vms_wire_put_u8(&w, VMS_OFF_DLM_MODE + 1u, 0);     /* body[31], 38/38 */
	vms_wire_put_le32(&w, VMS_OFF_DLM_GRANT_REC, VMS_DLM_GRANT_REC_VAL);
	/*
	 * body[36:52] -- THE MASTER RESOURCE'S VALUE BLOCK, the span the op-0x06
	 * write grounds and the dlm-grant-valblk specimen proves a real grant
	 * returns (it hands back the exact 16 bytes the requester had written).
	 * The caller's block, or zeros when it holds none -- and zeros are the
	 * data for a resource whose block is zero, not a placeholder. It is also
	 * what makes the byte-for-byte reproduction of the real grant land:
	 * body[36] and body[40] in those 38 grants are this block's content.
	 */
	/*
	 * THE REQUEST'S IDENTITY AND NAME ARE NOT ECHOED BACK, AND THE TWO
	 * LAYOUTS OVERLAP -- which is the trap this builder exists to avoid:
	 *
	 *   a REQUEST   body[44:46] group, body[46] mode, body[47] name length,
	 *               body[48..48+len) the name
	 *   a GRANT     body[36:52] the value block, which COVERS body[44:52]
	 *
	 * On the reference pair the master cleared body[44:52] and the name's
	 * tail body[52:58] -- a 10-byte name -- and echoed everything beyond it
	 * (that tail is the requester's own stale buffer, "ION_DATABASE" in both
	 * frames, verbatim). Clearing a fixed span instead, or writing a zero at
	 * body[47] "because a grant carries no name", destroys five bytes of the
	 * value block; writing the block LAST is what keeps both rules true at
	 * once.
	 *
	 * body[52:54] is the SCS-layer word this codec does not own (see the
	 * header): left zero, never minted.
	 */
	vms_wire_put_u8(&w, VMS_OFF_DLM_RES_GROUP, 0);
	vms_wire_put_u8(&w, VMS_OFF_DLM_RES_GROUP + 1u, 0);
	vms_wire_put_u8(&w, VMS_OFF_DLM_RES_MODE, 0);
	vms_wire_put_u8(&w, VMS_OFF_DLM_NAME_LEN, 0);
	for (i = 0u; i < req_name_len; i++)
		vms_wire_put_u8(&w, VMS_OFF_DLM_NAME + i, 0u);

	/*
	 * body[36:52] -- THE MASTER RESOURCE'S VALUE BLOCK, written LAST so it
	 * owns every byte of its own span. The op-0x06 write crossing grounds
	 * this span, and the dlm-grant-valblk specimen proves a real grant
	 * returns it (it hands back the exact 16 bytes the requester had
	 * written). The caller's block, or zeros when it holds none -- and for a
	 * resource whose block is zero, zero is the data.
	 */
	for (i = 0u; i < VMS_DLM_VALBLK_WIRE_LEN; i++)
		vms_wire_put_u8(&w, VMS_OFF_DLM_VALBLK + i,
				valblk != (const uint8_t *)0 ? valblk[i] : 0u);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

/*
 * A CONVERT (op 0x07) the master QUEUED (rd vms-cab). GROUNDED from real
 * VAX<->VAX traffic (2026-10-09 lab captures ev11/ev13/e8b-refire/m4): a real
 * VMS master answers a queued conversion AT ONCE with the request echoed, the
 * category made a response (0x82), and the outcome byte body[34] = 0xfb. Every
 * sampled pair differs from its request in exactly body[0:4] (the CM
 * envelope, the wrapper's), body[8] (category), body[34] (outcome) and
 * body[52:54] (the SCS-layer word this codec does not own: left zero, as the
 * grant leaves it). The op stays 0x07. Without this answer the requesting
 * VAX process waits in RWSCS for ever, and cannot even be deleted
 * (measured: run ci6-evac-13). The later grant is a separate message.
 */
vms_codec_status_t vms_dlm_convert_response_build_queued(const uint8_t *req_body,
							 uint32_t req_len,
							 uint8_t *frame,
							 uint32_t cap,
							 uint32_t *written)
{
	vms_wire_view_t v;
	vms_wire_buf_t w;
	uint8_t cat, op;

	if (req_body == (const uint8_t *)0 || req_len < VMS_CM_BODY_LEN)
		return VMS_CODEC_E_SHORT;
	vms_wire_view_init(&v, req_body, req_len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (cat != VMS_DLM_CAT_REQUEST || op != VMS_DLM_WIREOP_CONVERT)
		return VMS_CODEC_E_CLASS;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN, req_body);
	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT,
			vms_wire_response_category(VMS_DLM_CAT_REQUEST));
	vms_wire_put_u8(&w, VMS_OFF_DLM_GRANT_REC + 2u, VMS_DLM_REPLY_QUEUED);
	vms_wire_put_u8(&w, VMS_OFF_SYSAP_BODY + 52u, 0u);
	vms_wire_put_u8(&w, VMS_OFF_SYSAP_BODY + 53u, 0u);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_enq_response_build_deny(uint32_t req_pid_echo,
						   uint32_t master_lkid,
						   uint8_t res_acmode,
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
	/* body[46]: the ACCESS MODE, echoed from the request this denies (rd
	 * vms-b5b0 -- it is not the constant 0x03 this used to write). A reply
	 * that renamed the resource's domain would deny a lock on one resource
	 * and name another. */
	vms_wire_put_u8(&w, VMS_OFF_DLM_RES_MODE, res_acmode);
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
 * The directory hash at body[128:132] -- read only, never built alone.
 * See the header for the grounding (rd vms-4fb).
 * ------------------------------------------------------------------ */

/* Nonzero iff the body's parent span is all zero: a ROOT resource. */
static int dlm_is_root(vms_wire_view_t *v)
{
	uint8_t span[VMS_DLM_PARENT_SPAN_LEN];
	uint32_t i;

	vms_wire_get_bytes(v, VMS_OFB_DLM_PARENT_SPAN, VMS_DLM_PARENT_SPAN_LEN,
			   span);
	if (!vms_wire_view_ok(v))
		return 0;
	for (i = 0u; i < VMS_DLM_PARENT_SPAN_LEN; i++) {
		if (span[i] != 0u)
			return 0;
	}
	return 1;
}

vms_codec_status_t vms_dlm_dir_hash_parse_body(const uint8_t *body, uint32_t len,
					  uint32_t *out)
{
	vms_wire_view_t v;
	uint8_t cat, op;
	uint32_t hash;

	if (out == (uint32_t *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	/* ONLY a REQUEST (0x02) op-0x01 -- the frame the value is grounded on
	 * -- and only for a ROOT: a sub-resource's value is a property of its
	 * parent as well. Not the 0x82 answer: it rewrites body[28:40], parent
	 * span included, and an answer from a MASTER carries no name at all, so
	 * neither "root" nor the name could be read off it. */
	if (cat != VMS_DLM_CAT_REQUEST || op != VMS_DLM_WIREOP_ENQ)
		return VMS_CODEC_E_CLASS;
	if (!dlm_is_root(&v)) {
		if (!vms_wire_view_ok(&v))
			return v.err;
		return VMS_CODEC_E_CLASS;
	}

	hash = vms_wire_get_le32(&v, VMS_OFB_DLM_DIR_HASH);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*out = hash;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * The directory role (rd vms-8219) -- see the header for the grounding.
 * ------------------------------------------------------------------ */

/* Which frames name a resource FOR A DIRECTORY, and is this one of them? */
static int dlm_names_for_directory(vms_wire_view_t *v, uint8_t op)
{
	if (op == VMS_DLM_WIREOP_ENQ || op == VMS_DLM_WIREOP_DIR_LOOKUP_TR)
		return dlm_is_root(v);
	return op == VMS_DLM_WIREOP_DIR_REMOVE || op == VMS_DLM_WIREOP_REBUILD;
}

int vms_dlm_shape_fit_for_any_member(const uint8_t *body, uint32_t len)
{
	vms_wire_view_t v;
	uint8_t cat, op;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return 0;
	if (cat != VMS_DLM_CAT_REQUEST)
		return 0;
	return op == VMS_DLM_WIREOP_ENQ || op == VMS_DLM_WIREOP_CONVERT ||
	       op == VMS_DLM_WIREOP_CONVERT_VALBLK ||
	       op == VMS_DLM_WIREOP_DEQ;
}

vms_codec_status_t vms_dlm_res_ident_parse_body(const uint8_t *body,
						uint32_t len,
						struct vms_dlm_res_ident *out)
{
	vms_wire_view_t v;
	struct vms_dlm_res_ident id;
	uint8_t cat, op;

	if (out == (struct vms_dlm_res_ident *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (cat != VMS_DLM_CAT_REQUEST || !dlm_names_for_directory(&v, op))
		return vms_wire_view_ok(&v) ? VMS_CODEC_E_CLASS : v.err;

	id.group = vms_wire_get_le16(&v, VMS_OFB_DLM_RES_GROUP);
	id.mode = vms_wire_get_u8(&v, VMS_OFB_DLM_RES_MODE);
	id.name_len = vms_wire_get_u8(&v, VMS_OFB_DLM_NAME_LEN);
	id.hash = vms_wire_get_le32(&v, VMS_OFB_DLM_DIR_HASH);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (id.name_len == 0u || id.name_len >= VMS_DLM_NAME_MAX)
		return op == VMS_DLM_WIREOP_DIR_REMOVE && id.name_len == 0u ?
		       VMS_CODEC_E_CLASS : VMS_CODEC_E_RANGE;
	vms_wire_get_bytes(&v, VMS_OFB_DLM_NAME, id.name_len, id.name);
	if (!vms_wire_view_ok(&v))
		return v.err;
	*out = id;
	return VMS_CODEC_OK;
}

/* The echoed identity of an ANSWER body: the same four positions a request
 * carries it in, read without the request-only class checks. */
static vms_codec_status_t dlm_answer_ident(vms_wire_view_t *v,
					   struct vms_dlm_res_ident *id)
{
	id->group = vms_wire_get_le16(v, VMS_OFB_DLM_RES_GROUP);
	id->mode = vms_wire_get_u8(v, VMS_OFB_DLM_RES_MODE);
	id->name_len = vms_wire_get_u8(v, VMS_OFB_DLM_NAME_LEN);
	id->hash = vms_wire_get_le32(v, VMS_OFB_DLM_DIR_HASH);
	if (!vms_wire_view_ok(v))
		return v->err;
	if (id->name_len == 0u || id->name_len >= VMS_DLM_NAME_MAX)
		return VMS_CODEC_E_CLASS;
	vms_wire_get_bytes(v, VMS_OFB_DLM_NAME, id->name_len, id->name);
	return vms_wire_view_ok(v) ? VMS_CODEC_OK : v->err;
}

vms_codec_status_t vms_dlm_dir_answer_parse_body(const uint8_t *body,
						 uint32_t len,
						 struct vms_dlm_dir_answer *out)
{
	vms_wire_view_t v;
	struct vms_dlm_dir_answer a;
	vms_codec_status_t st;
	uint8_t cat, op;

	if (out == (struct vms_dlm_dir_answer *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (!vms_wire_is_response(cat) ||
	    (cat & 0x7fu) != VMS_DLM_CAT_REQUEST ||
	    op != VMS_DLM_WIREOP_ENQ)
		return VMS_CODEC_E_CLASS;

	a.status = vms_wire_get_u8(&v, VMS_OFB_DLM_DIR_STATUS);
	a.mode = vms_wire_get_u8(&v, VMS_OFB_DLM_MODE);
	a.req_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_REQ_LKID);
	a.master_csid = vms_wire_get_le32(&v, VMS_OFB_DLM_DIR_MASTER);
	if (!vms_wire_view_ok(&v))
		return v.err;
	/* Signal 1: the outcome byte at body[34]. */
	if (a.status != VMS_DLM_DIR_YOU_MASTER &&
	    a.status != VMS_DLM_DIR_REDIRECT)
		return VMS_CODEC_E_CLASS;
	/* Signal 2: the NAME is echoed (a grant omits it). */
	st = dlm_answer_ident(&v, &a.id);
	if (st != VMS_CODEC_OK)
		return st;
	/* A redirect that names nobody is not a redirect. */
	if (a.status == VMS_DLM_DIR_REDIRECT && a.master_csid == 0u)
		return VMS_CODEC_E_CLASS;
	if (a.status == VMS_DLM_DIR_YOU_MASTER)
		a.master_csid = 0u;
	*out = a;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_dir_answer_build(const uint8_t *req_body,
					    uint32_t req_len, uint8_t status,
					    uint32_t master_csid,
					    uint8_t *frame, uint32_t cap,
					    uint32_t *written)
{
	struct vms_dlm_res_ident id;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t i;

	if (status != VMS_DLM_DIR_YOU_MASTER && status != VMS_DLM_DIR_REDIRECT)
		return VMS_CODEC_E_INVAL;
	if (status == VMS_DLM_DIR_REDIRECT && master_csid == 0u)
		return VMS_CODEC_E_INVAL;
	if (req_body == (const uint8_t *)0 || req_len < VMS_CM_BODY_LEN)
		return VMS_CODEC_E_SHORT;
	if (req_body[VMS_OFB_DLM_OP] != VMS_DLM_WIREOP_ENQ &&
	    req_body[VMS_OFB_DLM_OP] != VMS_DLM_WIREOP_DIR_LOOKUP_TR)
		return VMS_CODEC_E_CLASS;
	st = vms_dlm_res_ident_parse_body(req_body, req_len, &id);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN, req_body);
	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT,
			(uint8_t)(VMS_DLM_CAT_REQUEST | VMS_WIRE_RESPONSE_BIT));
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_WIREOP_ENQ);
	for (i = VMS_DLM_DIR_ANSWER_LO; i < VMS_DLM_DIR_ANSWER_HI; i++)
		vms_wire_put_u8(&w, VMS_OFF_SYSAP_BODY + i, 0u);
	vms_wire_put_u8(&w, VMS_OFF_DLM_DIR_STATUS, status);
	if (status == VMS_DLM_DIR_REDIRECT)
		vms_wire_put_le32(&w, VMS_OFF_DLM_DIR_MASTER, master_csid);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN;
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
	inv2 = vms_wire_get_le16(&v, VMS_OFB_DLM_REBUILD_TYPE);
	if (!vms_wire_view_ok(&v))
		return v.err;
	/* body[12:14] is the constant invariant; body[14:16] is the rebuild-TYPE
	 * (JOIN 0x0003 / REJOIN 0x0004), NOT a constant -- gating on only 0x0003
	 * wrongly rejected every rejoin-rebuild frame (vms-20c, caught by the
	 * byte-identical twin-test against real captured op-0x0d frames). */
	if (inv1 != VMS_DLM_REBUILD_INV1_CONST)
		return VMS_CODEC_E_CLASS;
	if (inv2 != VMS_DLM_REBUILD_TYPE_JOIN && inv2 != VMS_DLM_REBUILD_TYPE_REJOIN)
		return VMS_CODEC_E_CLASS;
	out->rebuild_type = inv2;

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

vms_codec_status_t
vms_dlm_rebuild_request_build(const struct vms_dlm_rebuild_record *rec,
			     uint8_t *frame, uint32_t cap, uint32_t *written)
{
	vms_wire_buf_t w;

	if (rec == (const struct vms_dlm_rebuild_record *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	/* SCAFFOLD (vms-20c): the record's body is emitted VERBATIM into abs
	 * [72,204) -- the byte-identical round-trip that grounds the op-0x0d
	 * layout against the captured real-VMS frames. The survivor-side SENDER
	 * replaces this verbatim copy with a live-state field assembly once §6
	 * pins the send-field offsets (mode/lkid/csid); until then those bytes
	 * stay in the honest verbatim region, never named or fabricated. */
	vms_wire_put_bytes(&w, VMS_OFF_SYSAP_BODY, VMS_DLM_REBUILD_ECHO_LEN,
			   rec->body);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_DLM_REBUILD_ECHO_LEN;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * op 0x03 $DEQ / op 0x05 BLKAST / op 0x06 CONVERT-with-VALBLK
 * -- GROUNDED, vms-c03 capture set (BLKAST re-read by rd vms-ea1). Read the header's section comment
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
 * frames whose lock-id span is zero or stale (the op-0x04 directory removals
 * among them); handing one up as "a release of lock 0" would be manufacturing
 * a referent. Refused instead.
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
	  VMS_WIRE_ACT_CONSUME, 0u, "vms-c03 dlm-blk2-20260911.pcap rec 48 (vms-ea1)" },
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
					  uint32_t *out)
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

/* ------------------------------------------------------------------ *
 * op 0x0e DLKSRCH -- distributed deadlock search (H11, vms-d55).
 *
 * OVMX-DERIVED (Rule 8): every field is pure FORWARDING / ACCUMULATOR
 * state. This codec carries the record verbatim; it decides NOTHING.
 * The executive arm reads live res->granted / ENUM_WAITS at each hop and
 * makes every grant/abort decision from THAT, never from these frame
 * values (INV-6, ⭐⭐). So the codec neither validates a lock referent
 * nor refuses a zero id -- which fields are meaningful is the flag's
 * business and the arm's, not the wire format's.
 * ------------------------------------------------------------------ */
vms_codec_status_t vms_dlm_dlksrch_parse_body(const uint8_t *body, uint32_t len,
					      struct vms_dlm_dlksrch_record *out)
{
	vms_wire_view_t v;
	uint8_t cat, op;

	if (out == (struct vms_dlm_dlksrch_record *)0)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, body, len);
	cat = vms_wire_get_u8(&v, VMS_OFB_DLM_CAT);
	op = vms_wire_get_u8(&v, VMS_OFB_DLM_OP);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (vms_wire_is_response(cat) || (cat & 0x7fu) != VMS_DLM_CAT_REQUEST)
		return VMS_CODEC_E_CLASS;
	if (op != VMS_DLM_WIREOP_DLKSRCH)
		return VMS_CODEC_E_CLASS;

	out->flag           = vms_wire_get_u8(&v, VMS_OFB_DLM_DLK_FLAG);
	out->initiator_csid = vms_wire_get_le32(&v, VMS_OFB_DLM_DLK_INITIATOR_CSID);
	out->initiator_lkid = vms_wire_get_le32(&v, VMS_OFB_DLM_DLK_INITIATOR_LKID);
	out->blocked_csid   = vms_wire_get_le32(&v, VMS_OFB_DLM_DLK_BLOCKED_CSID);
	out->blocked_lkid   = vms_wire_get_le32(&v, VMS_OFB_DLM_DLK_BLOCKED_LKID);
	out->victim_csid    = vms_wire_get_le32(&v, VMS_OFB_DLM_DLK_VICTIM_CSID);
	out->victim_lkid    = vms_wire_get_le32(&v, VMS_OFB_DLM_DLK_VICTIM_LKID);
	out->ttl            = vms_wire_get_u8(&v, VMS_OFB_DLM_DLK_TTL);
	if (!vms_wire_view_ok(&v))
		return v.err;

	return VMS_CODEC_OK;
}

vms_codec_status_t vms_dlm_dlksrch_build(const struct vms_dlm_dlksrch_record *rec,
					 uint8_t *frame, uint32_t cap,
					 uint32_t *written)
{
	vms_wire_buf_t w;

	if (rec == (const struct vms_dlm_dlksrch_record *)0)
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_u8(&w, VMS_OFF_DLM_CAT, VMS_DLM_CAT_REQUEST);
	vms_wire_put_u8(&w, VMS_OFF_DLM_OP, VMS_DLM_WIREOP_DLKSRCH);
	vms_wire_put_u8(&w, VMS_OFF_DLM_DLK_FLAG, rec->flag);
	vms_wire_put_le32(&w, VMS_OFF_DLM_DLK_INITIATOR_CSID, rec->initiator_csid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_DLK_INITIATOR_LKID, rec->initiator_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_DLK_BLOCKED_CSID, rec->blocked_csid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_DLK_BLOCKED_LKID, rec->blocked_lkid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_DLK_VICTIM_CSID, rec->victim_csid);
	vms_wire_put_le32(&w, VMS_OFF_DLM_DLK_VICTIM_LKID, rec->victim_lkid);
	vms_wire_put_u8(&w, VMS_OFF_DLM_DLK_TTL, rec->ttl);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = vms_wire_buf_len(&w);
	return VMS_CODEC_OK;
}
