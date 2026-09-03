// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_blk.c - the SCA block-data-transfer header (FC-P6.1).
 *
 * Read vms_cluster_codec_blk.h first -- in particular the paragraph on the
 * `+4`/`+6` words, which this file carries through and never interprets.
 *
 * Pure, like every sibling harvest TU: no state, no allocation, no substrate
 * call, no libc; every access through the parent TU's bounded vms_wire_get_
 * and vms_wire_put_ primitives.
 */

#include "vms_cluster_codec_blk.h"
#include "vms_cluster_codec_scs.h"   /* VMS_OFF_SCSCTRL_FMTWORD + its constant */

/* ------------------------------------------------------------------ *
 * sec 1  The 28 bytes, one function each way
 * ------------------------------------------------------------------ */

/*
 * The single place in OVMX that knows where a block-transfer header's fields
 * sit. `at` is the header's own start; every field offset is expressed
 * relative to the class's own VMS_BLK_HDR_OFF so the two call sites (the
 * standalone frame at abs 56, the piggybacked trailer at the end of an end
 * message) share ONE layout and cannot drift apart.
 */
static void blk_hdr_put(vms_wire_buf_t *w, uint32_t at,
			const struct vms_blk_hdr *h)
{
	const uint32_t base = at;

	vms_wire_put_le32(w, base + (VMS_OFF_BLK_DEST_CONID - VMS_BLK_HDR_OFF),
			  h->dest_conid);
	/* +4 / +6: carried through EXACTLY as the caller supplied them. This
	 * codec has no opinion about what they mean and no default for them
	 * (vms_cluster_codec_blk.h, "THE TWO UNGROUNDED WORDS"). */
	vms_wire_put_le16(w, base + (VMS_OFF_BLK_OBS_W4 - VMS_BLK_HDR_OFF),
			  h->obs_w4);
	vms_wire_put_le16(w, base + (VMS_OFF_BLK_OBS_W6 - VMS_BLK_HDR_OFF),
			  h->obs_w6);
	vms_wire_put_le32(w, base + (VMS_OFF_BLK_REMAINING - VMS_BLK_HDR_OFF),
			  h->bytes_remaining);
	vms_wire_put_le32(w, base + (VMS_OFF_BLK_SRC_NAME - VMS_BLK_HDR_OFF),
			  h->src_name);
	vms_wire_put_le32(w, base + (VMS_OFF_BLK_DST_OFFSET - VMS_BLK_HDR_OFF),
			  h->dst_offset);
	vms_wire_put_le32(w, base + (VMS_OFF_BLK_DST_NAME - VMS_BLK_HDR_OFF),
			  h->dst_name);
	vms_wire_put_le32(w, base + (VMS_OFF_BLK_SRC_OFFSET - VMS_BLK_HDR_OFF),
			  h->src_offset);
}

static void blk_hdr_get(vms_wire_view_t *v, uint32_t at,
			struct vms_blk_hdr *h)
{
	const uint32_t base = at;

	h->dest_conid = vms_wire_get_le32(v, base +
			(VMS_OFF_BLK_DEST_CONID - VMS_BLK_HDR_OFF));
	h->obs_w4 = vms_wire_get_le16(v, base +
			(VMS_OFF_BLK_OBS_W4 - VMS_BLK_HDR_OFF));
	h->obs_w6 = vms_wire_get_le16(v, base +
			(VMS_OFF_BLK_OBS_W6 - VMS_BLK_HDR_OFF));
	h->bytes_remaining = vms_wire_get_le32(v, base +
			(VMS_OFF_BLK_REMAINING - VMS_BLK_HDR_OFF));
	h->src_name = vms_wire_get_le32(v, base +
			(VMS_OFF_BLK_SRC_NAME - VMS_BLK_HDR_OFF));
	h->dst_offset = vms_wire_get_le32(v, base +
			(VMS_OFF_BLK_DST_OFFSET - VMS_BLK_HDR_OFF));
	h->dst_name = vms_wire_get_le32(v, base +
			(VMS_OFF_BLK_DST_NAME - VMS_BLK_HDR_OFF));
	h->src_offset = vms_wire_get_le32(v, base +
			(VMS_OFF_BLK_SRC_OFFSET - VMS_BLK_HDR_OFF));
}

vms_codec_status_t vms_blk_hdr_build_at(const struct vms_blk_hdr *h,
					uint8_t *frame, uint32_t cap,
					uint32_t at)
{
	vms_wire_buf_t w;

	if (h == (const struct vms_blk_hdr *)0 || frame == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (cap < at || (cap - at) < VMS_BLK_HDR_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_buf_init(&w, frame, cap);
	blk_hdr_put(&w, at, h);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_blk_hdr_build(const struct vms_blk_hdr *h,
				     uint8_t *frame, uint32_t cap)
{
	return vms_blk_hdr_build_at(h, frame, cap, VMS_BLK_HDR_OFF);
}

vms_codec_status_t vms_blk_hdr_parse_at(const uint8_t *frame, uint32_t len,
					uint32_t at, struct vms_blk_hdr *out)
{
	vms_wire_view_t v;

	if (frame == (const uint8_t *)0 || out == (struct vms_blk_hdr *)0)
		return VMS_CODEC_E_INVAL;
	if (len < at || (len - at) < VMS_BLK_HDR_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_view_init(&v, frame, len);
	blk_hdr_get(&v, at, out);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_blk_hdr_parse(const uint8_t *frame, uint32_t len,
				     struct vms_blk_hdr *out)
{
	return vms_blk_hdr_parse_at(frame, len, VMS_BLK_HDR_OFF, out);
}

/* ------------------------------------------------------------------ *
 * sec 2  A whole frame
 * ------------------------------------------------------------------ */

static void blk_view_clear(struct vms_blk_view *out)
{
	uint8_t *p = (uint8_t *)&out->hdr;
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(out->hdr); i++)
		p[i] = 0;
	out->data = (const uint8_t *)0;
	out->data_len = 0;
}

int vms_blk_frame_structural_ok(const uint8_t *frame, uint32_t len)
{
	vms_wire_view_t v;
	uint16_t fmt;

	if (frame == (const uint8_t *)0 || len < VMS_BLK_DATA_OFF)
		return 0;

	vms_wire_view_init(&v, frame, len);
	fmt = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_FMTWORD);
	if (!vms_wire_view_ok(&v))
		return 0;
	/* In an SCS message abs 58 is the envelope's GROUNDED format word; in a
	 * block-transfer frame it is the upper half of the 32-bit destination
	 * connection ID. See the header: this is a NEGATIVE and is a
	 * precondition, never a class test. */
	return (fmt != VMS_SCSCTRL_FMTWORD_CONST) ? 1 : 0;
}

vms_codec_status_t vms_blk_frame_parse(const uint8_t *frame, uint32_t len,
				       const struct vms_frame_info *fi,
				       struct vms_blk_view *out)
{
	vms_codec_status_t st;

	if (out == (struct vms_blk_view *)0)
		return VMS_CODEC_E_INVAL;
	blk_view_clear(out);

	if (fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	/* The only class claim this codec can honestly make: the frame really
	 * is a sequenced SCS frame. Nothing narrower exists -- see
	 * vms_blk_frame_structural_ok()'s doc comment. */
	if (fi->family != (uint8_t)VMS_FFAM_SCS ||
	    (fi->caps & (uint8_t)VMS_FCAP_SEQ) == 0u)
		return VMS_CODEC_E_CLASS;

	st = vms_blk_hdr_parse(frame, len, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;

	/* The frame's REAL length is the only bound there is. data_len 0 is
	 * WRITE's header-only half, not an error. */
	if (len > VMS_BLK_DATA_OFF) {
		out->data = frame + VMS_BLK_DATA_OFF;
		out->data_len = len - VMS_BLK_DATA_OFF;
	}
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * sec 3  The READ end-message piggyback
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_blk_trailer_build(const struct vms_blk_hdr *h,
					 const uint8_t *data, uint32_t data_len,
					 uint8_t *frame, uint32_t cap,
					 uint32_t frame_len,
					 uint32_t *total_out)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t total;

	if (data == (const uint8_t *)0 && data_len != 0u)
		return VMS_CODEC_E_INVAL;
	if (frame_len > cap)
		return VMS_CODEC_E_SHORT;
	if ((cap - frame_len) < VMS_BLK_HDR_LEN ||
	    (cap - frame_len - VMS_BLK_HDR_LEN) < data_len)
		return VMS_CODEC_E_SHORT;

	st = vms_blk_hdr_build_at(h, frame, cap, frame_len);
	if (st != VMS_CODEC_OK)
		return st;

	total = frame_len + VMS_BLK_HDR_LEN;
	if (data_len != 0u) {
		vms_wire_buf_init(&w, frame, cap);
		vms_wire_put_bytes(&w, total, data_len, data);
		if (!vms_wire_buf_ok(&w))
			return w.err;
		total += data_len;
	}
	if (total_out != (uint32_t *)0)
		*total_out = total;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_blk_trailer_parse(const uint8_t *frame,
					 uint32_t frame_len,
					 uint32_t inner_frame_len,
					 struct vms_blk_view *out)
{
	vms_codec_status_t st;

	if (out == (struct vms_blk_view *)0 || frame == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	blk_view_clear(out);

	/* No trailer is an honest, common outcome, not an error: a READ whose
	 * data divided evenly into whole streamed chunks piggybacks nothing. */
	if (frame_len <= inner_frame_len)
		return VMS_CODEC_OK;
	if ((frame_len - inner_frame_len) < VMS_BLK_HDR_LEN)
		return VMS_CODEC_E_SHORT;

	st = vms_blk_hdr_parse_at(frame, frame_len, inner_frame_len, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;

	if ((frame_len - inner_frame_len) > VMS_BLK_HDR_LEN) {
		out->data = frame + inner_frame_len + VMS_BLK_HDR_LEN;
		out->data_len = frame_len - inner_frame_len - VMS_BLK_HDR_LEN;
	}
	return VMS_CODEC_OK;
}
