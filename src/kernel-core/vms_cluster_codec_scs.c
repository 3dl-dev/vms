// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_scs.c - SCS connection/directory codec entries (FC-P2.1).
 *
 * Read vms_cluster_codec_scs.h first. Pure, like the parent TU and like
 * vms_cluster_codec_hello.c: no state, no allocation, no substrate call, no
 * libc, every array copy through the parent TU's vms_wire_get_bytes/
 * vms_wire_put_bytes/vms_wire_put_zero primitives.
 */

#include "vms_cluster_codec_scs.h"

/* GROUNDED sec 4(h)(2): the literal negative-resolution marker, byte-exact,
 * no padding needed (exactly 16 ASCII characters). */
const uint8_t vms_scs_dir_not_present_here[VMS_SCSCTRL_NAME_LEN] = {
	'N', 'O', 'T', ' ', 'P', 'R', 'E', 'S',
	'E', 'N', 'T', ' ', 'H', 'E', 'R', 'E'
};

/* ------------------------------------------------------------------ *
 * Content-length -> which optional spans this class carries.
 * ------------------------------------------------------------------ */

struct ctrl_shape {
	uint8_t scsargs;   /* the MIN_CR/STATUS pair at abs 72-75 */
	uint8_t tail4;
	uint8_t names;
	uint8_t blank;
};

static int shape_for_len(uint16_t content, struct ctrl_shape *s)
{
	s->scsargs = 0;
	s->tail4 = 0;
	s->names = 0;
	s->blank = 0;
	switch (content) {
	case VMS_SCSCTRL_LEN_SHORT:
		return 1;
	case VMS_SCSCTRL_LEN_MARKER:
		s->scsargs = 1;
		return 1;
	case VMS_SCSCTRL_LEN_ECHO:
		s->scsargs = 1;
		s->tail4 = 1;
		return 1;
	case VMS_SCSCTRL_LEN_LOOKUP:
		s->scsargs = 1;
		s->names = 1;
		return 1;
	case VMS_SCSCTRL_LEN_CONNECT:
		s->scsargs = 1;
		s->names = 1;
		s->blank = 1;
		return 1;
	default:
		return 0;
	}
}

static uint16_t content_for_shape(const struct ctrl_shape *s)
{
	uint16_t n = VMS_SCSCTRL_LEN_SHORT;

	if (s->scsargs)
		n = VMS_SCSCTRL_LEN_MARKER;
	if (s->tail4)
		n = VMS_SCSCTRL_LEN_ECHO;
	if (s->names)
		n = s->blank ? VMS_SCSCTRL_LEN_CONNECT : VMS_SCSCTRL_LEN_LOOKUP;
	return n;
}

/* Exactly one of {none, scsargs, scsargs+tail4, scsargs+names,
 * scsargs+names+blank} -- tail4 and names/blank are mutually exclusive, and
 * blank requires names. Anything else is not a shape this codec recognises. */
static int shape_is_valid(const struct ctrl_shape *s)
{
	if (s->tail4 && s->names)
		return 0;
	if (s->blank && !s->names)
		return 0;
	if ((s->tail4 || s->names) && !s->scsargs)
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ *
 * Parse
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_scs_ctrl_parse(const uint8_t *frame, uint32_t len,
				      const struct vms_frame_info *fi,
				      struct vms_scs_ctrl_frame *out)
{
	vms_wire_view_t v;
	vms_codec_status_t st;
	struct ctrl_shape shape;
	uint8_t mt, fmt;

	if (out == (struct vms_scs_ctrl_frame *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if (fi->family != VMS_FFAM_SCS)
		return VMS_CODEC_E_CLASS;

	/* sec 4(m) "the msgtype phase rule": connection-control frames carry
	 * 0x4b/0x5b/0x7b, never the 0x41 START or 0x48 credit-return msgtype.
	 * Every SCS_* class row carries VMS_FCAP_MSGTYPE, so this accessor
	 * always succeeds once fi->family == VMS_FFAM_SCS is already true. */
	st = vms_scs_msgtype(frame, len, fi, &mt, &fmt);
	if (st != VMS_CODEC_OK)
		return st;
	if (fmt != VMS_SCS_FORMAT_V13)
		return VMS_CODEC_E_CLASS;
	if (mt != VMS_SCS_MT_MSG && mt != VMS_SCS_MT_SETUP && mt != VMS_SCS_MT_ALT)
		return VMS_CODEC_E_CLASS;

	if (!shape_for_len(fi->sca_content, &shape))
		return VMS_CODEC_E_SHORT;

	st = vms_sca_hdr_parse(frame, len, &out->hdr);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	out->recv_ack = vms_wire_get_le16(&v, VMS_OFF_SCS_RECV_ACK);
	out->send_seq = vms_wire_get_le16(&v, VMS_OFF_SCS_SEND_SEQ);
	out->incarn = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_INCARN);
	out->lan_ovrhd = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_LANOVRHD);
	out->tail_const1 = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_TAILCONST1);
	out->tail_const2 = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_TAILCONST2);
	/* abs 40/44/48 (recv_ack/send_seq mirrors) are read but not stored --
	 * GROUNDED 100% equal to recv_ack/send_seq (sec 4h(4)), so they are
	 * DERIVED on build, never independent data. */
	out->inner_len = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_INNERLEN);
	out->fmt_word = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_FMTWORD);
	out->op = vms_wire_get_le16(&v, VMS_OFF_SCS_CTRL_TYPE);
	out->credit = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_CREDIT);
	out->conid_remote = vms_wire_get_le32(&v, VMS_OFF_SCS_CONID_REMOTE);
	out->conid_local = vms_wire_get_le32(&v, VMS_OFF_SCS_CONID_LOCAL);

	out->has_scsargs = shape.scsargs;
	if (shape.scsargs) {
		out->min_cr = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_MIN_CR);
		out->status = vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_STATUS);
	}

	out->has_tail4 = shape.tail4;
	if (shape.tail4)
		vms_wire_get_bytes(&v, VMS_OFF_SCSCTRL_TAIL4, 4, out->tail4);

	out->has_names = shape.names;
	if (shape.names) {
		vms_wire_get_bytes(&v, VMS_OFF_SCSCTRL_NAME1,
				   VMS_SCSCTRL_NAME_LEN, out->name1);
		vms_wire_get_bytes(&v, VMS_OFF_SCSCTRL_NAME2,
				   VMS_SCSCTRL_NAME_LEN, out->name2);
	}

	out->has_blank = shape.blank;
	if (shape.blank)
		vms_wire_get_bytes(&v, VMS_OFF_SCSCTRL_BLANK,
				   VMS_SCSCTRL_NAME_LEN, out->blank);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * Build
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_scs_ctrl_build(const struct vms_scs_ctrl_frame *f,
				      uint8_t *frame, uint32_t cap,
				      uint32_t *written)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;
	struct ctrl_shape shape;
	uint16_t content;
	uint32_t hdr_written = 0;
	uint32_t total;

	if (f == (const struct vms_scs_ctrl_frame *)0)
		return VMS_CODEC_E_INVAL;

	shape.scsargs = f->has_scsargs;
	shape.tail4 = f->has_tail4;
	shape.names = f->has_names;
	shape.blank = f->has_blank;
	if (!shape_is_valid(&shape))
		return VMS_CODEC_E_INVAL;
	content = content_for_shape(&shape);

	st = vms_sca_hdr_build(&f->hdr, frame, cap, &hdr_written);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK, f->recv_ack);
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEND_SEQ, f->send_seq);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_INCARN, f->incarn);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_LANOVRHD, f->lan_ovrhd);
	/* The recv_ack/send_seq mirrors (abs 40/44/48) are DERIVED, never an
	 * independent field -- sec 4(h)(4) GROUNDS them 100% equal to the
	 * primary values above; a builder that left them poisoned would emit
	 * a self-inconsistent frame no real peer ever sends. */
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_ACKMIRROR1, f->recv_ack);
	vms_wire_put_zero(&w, VMS_OFF_SCSCTRL_ZERO1, 2);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_SEQMIRROR, f->send_seq);
	vms_wire_put_zero(&w, VMS_OFF_SCSCTRL_ZERO2, 2);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_ACKMIRROR2, f->recv_ack);
	vms_wire_put_zero(&w, VMS_OFF_SCSCTRL_ZERO3, 2);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_TAILCONST1, f->tail_const1);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_TAILCONST2, f->tail_const2);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_INNERLEN, f->inner_len);
	/* The format word is the one baked-in constant this class asserts
	 * (sec 4(h)(1b), GROUNDED across the whole corpus) -- never the
	 * caller's f->fmt_word, exactly as vms_hello_build() bakes its own
	 * discovery markers rather than trusting the caller's struct. */
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_FMTWORD, VMS_SCSCTRL_FMTWORD_CONST);
	vms_wire_put_le16(&w, VMS_OFF_SCS_CTRL_TYPE, f->op);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_CREDIT, f->credit);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, f->conid_remote);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_LOCAL, f->conid_local);

	if (shape.scsargs) {
		vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_MIN_CR, f->min_cr);
		vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_STATUS, f->status);
	}
	if (shape.tail4)
		vms_wire_put_bytes(&w, VMS_OFF_SCSCTRL_TAIL4, 4, f->tail4);
	if (shape.names) {
		vms_wire_put_bytes(&w, VMS_OFF_SCSCTRL_NAME1,
				   VMS_SCSCTRL_NAME_LEN, f->name1);
		vms_wire_put_bytes(&w, VMS_OFF_SCSCTRL_NAME2,
				   VMS_SCSCTRL_NAME_LEN, f->name2);
	}
	if (shape.blank)
		vms_wire_put_bytes(&w, VMS_OFF_SCSCTRL_BLANK,
				   VMS_SCSCTRL_NAME_LEN, f->blank);

	if (!vms_wire_buf_ok(&w))
		return w.err;

	total = (uint32_t)VMS_ETH_HDR_LEN + (uint32_t)content;
	if (written != (uint32_t *)0)
		*written = total;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * The body-level SCS header (abs 56-71) -- design SS3.2.4's E1 seam
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_scs_hdr_build(const struct vms_scs_hdr *h,
				     uint8_t *out, uint32_t cap)
{
	vms_wire_buf_t w;

	if (h == (const struct vms_scs_hdr *)0 || out == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (cap < VMS_SCS_HDR_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_buf_init(&w, out, VMS_SCS_HDR_LEN);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	vms_wire_put_le16(&w, VMS_OFF_SCSHDR_INNERLEN, h->inner_len);
	/* The one baked-in constant of this class (sec 4(h)(1b), GROUNDED
	 * across every SCS length class) -- same discipline as
	 * vms_scs_ctrl_build() above. */
	vms_wire_put_le16(&w, VMS_OFF_SCSHDR_FMTWORD, VMS_SCSCTRL_FMTWORD_CONST);
	vms_wire_put_le16(&w, VMS_OFF_SCSHDR_MTYPE, h->mtype);
	vms_wire_put_le16(&w, VMS_OFF_SCSHDR_CREDIT, h->credit);
	vms_wire_put_le32(&w, VMS_OFF_SCSHDR_CONID_R, h->conid_remote);
	vms_wire_put_le32(&w, VMS_OFF_SCSHDR_CONID_L, h->conid_local);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_hdr_parse(const uint8_t *in, uint32_t len,
				     struct vms_scs_hdr *out)
{
	vms_wire_view_t v;
	uint16_t fmt;

	if (in == (const uint8_t *)0 || out == (struct vms_scs_hdr *)0)
		return VMS_CODEC_E_INVAL;
	if (len < VMS_SCS_HDR_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_view_init(&v, in, VMS_SCS_HDR_LEN);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	fmt = vms_wire_get_le16(&v, VMS_OFF_SCSHDR_FMTWORD);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (fmt != VMS_SCSCTRL_FMTWORD_CONST)
		return VMS_CODEC_E_CLASS;

	out->inner_len    = vms_wire_get_le16(&v, VMS_OFF_SCSHDR_INNERLEN);
	out->mtype        = vms_wire_get_le16(&v, VMS_OFF_SCSHDR_MTYPE);
	out->credit       = vms_wire_get_le16(&v, VMS_OFF_SCSHDR_CREDIT);
	out->conid_remote = vms_wire_get_le32(&v, VMS_OFF_SCSHDR_CONID_R);
	out->conid_local  = vms_wire_get_le32(&v, VMS_OFF_SCSHDR_CONID_L);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* The SYSAP body of an application message starts at abs 72 -- one span
 * past the 16-byte SCS header this file owns (design SS3.2.4). Derived,
 * never re-stated. */
#define SCSHDR_ABS_OFF   VMS_OFF_SCSCTRL_INNERLEN            /* abs 56 */
#define SCSBODY_ABS_OFF  (SCSHDR_ABS_OFF + VMS_SCS_HDR_LEN)  /* abs 72 */

vms_codec_status_t vms_scs_hdr_parse_frame(const uint8_t *frame, uint32_t len,
					   struct vms_scs_hdr *out)
{
	if (frame == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (len < SCSHDR_ABS_OFF + VMS_SCS_HDR_LEN)
		return VMS_CODEC_E_SHORT;
	return vms_scs_hdr_parse(frame + SCSHDR_ABS_OFF,
				 len - SCSHDR_ABS_OFF, out);
}

/*
 * The prefix `inner_len` is measured PAST: the SCA content's first 44 bytes
 * (the codec's own "content - 44" rule, VMS_OFF_SCSCTRL_INNERLEN's comment).
 * Named here so the one place that knows it is this TU.
 */
#define SCSHDR_INNERLEN_BIAS 44u

vms_codec_status_t vms_scs_inner_frame_len(const uint8_t *frame, uint32_t len,
					   uint32_t *out)
{
	struct vms_scs_hdr h;
	vms_codec_status_t st;
	uint32_t inner;

	if (frame == (const uint8_t *)0 || out == (uint32_t *)0)
		return VMS_CODEC_E_INVAL;
	st = vms_scs_hdr_parse_frame(frame, len, &h);
	if (st != VMS_CODEC_OK)
		return st;

	inner = (uint32_t)VMS_ETH_HDR_LEN + SCSHDR_INNERLEN_BIAS +
		(uint32_t)h.inner_len;
	/* A declared length past the bytes that really arrived is not a bound
	 * this codec will hand out: it reports the real length, which reads as
	 * "no trailer" to the caller. */
	*out = inner > len ? len : inner;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_msg_body(const uint8_t *frame, uint32_t len,
				    const uint8_t **body, uint32_t *body_len)
{
	if (frame == (const uint8_t *)0 || body == (const uint8_t **)0 ||
	    body_len == (uint32_t *)0)
		return VMS_CODEC_E_INVAL;
	if (len <= SCSBODY_ABS_OFF)
		return VMS_CODEC_E_SHORT;
	*body = frame + SCSBODY_ABS_OFF;
	*body_len = len - SCSBODY_ABS_OFF;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_msg_body_build(const struct vms_scs_hdr *h,
					  const uint8_t *sysap_body,
					  uint32_t sysap_len,
					  uint8_t *out, uint32_t cap)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	if (out == (uint8_t *)0 || sysap_body == (const uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (cap < VMS_SCS_HDR_LEN)
		return VMS_CODEC_E_SHORT;
	if (sysap_len > cap - VMS_SCS_HDR_LEN)
		return VMS_CODEC_E_INVAL;   /* never a silent truncation */

	st = vms_scs_hdr_build(h, out, cap);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, out, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_bytes(&w, VMS_SCS_HDR_LEN, sysap_len, sysap_body);
	vms_wire_put_zero(&w, VMS_SCS_HDR_LEN + sysap_len,
			  cap - VMS_SCS_HDR_LEN - sysap_len);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * Directory lookup (sec 4(h)(2))
 * ------------------------------------------------------------------ */

static int bytes_all_zero(const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (b[i] != 0)
			return 0;
	}
	return 1;
}

static int bytes_eq(const uint8_t *a, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

/*
 * The ONE reading of the 16-byte result field, so the frame-level and the
 * body-level entry points below cannot classify the same bytes differently.
 */
static void dir_result_read(const uint8_t *result,
			    struct vms_scs_dir_lookup *out)
{
	uint32_t i;

	if (bytes_all_zero(result, VMS_SCSCTRL_NAME_LEN)) {
		out->result_kind = VMS_SCS_DIR_RESULT_EMPTY;
		for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
			out->result[i] = 0;
		return;
	}
	if (bytes_eq(result, vms_scs_dir_not_present_here,
		     VMS_SCSCTRL_NAME_LEN))
		out->result_kind = VMS_SCS_DIR_RESULT_NOT_PRESENT;
	else
		/* sec 4(h)(2) RE gap (c): an affirmative result is real wire
		 * data, honestly carried, with no asserted internal meaning. */
		out->result_kind = VMS_SCS_DIR_RESULT_AFFIRMATIVE;
	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
		out->result[i] = result[i];
}

vms_codec_status_t
vms_scs_dir_lookup_parse(const struct vms_scs_ctrl_frame *f,
			 struct vms_scs_dir_lookup *out)
{
	uint32_t i;

	if (f == (const struct vms_scs_ctrl_frame *)0 ||
	    out == (struct vms_scs_dir_lookup *)0)
		return VMS_CODEC_E_INVAL;
	/* A 110-content CONNECT frame carries the SAME two 16-byte fields but
	 * they are a SYSAP name pair, not a lookup result -- has_blank is
	 * exactly the discriminator (only the 94-content shape lacks it). */
	if (!f->has_names || f->has_blank)
		return VMS_CODEC_E_CLASS;

	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
		out->queried_name[i] = f->name1[i];
	dir_result_read(f->name2, out);
	return VMS_CODEC_OK;
}

vms_codec_status_t
vms_scs_dir_lookup_build(const struct vms_scs_dir_lookup *dl,
			 struct vms_scs_ctrl_frame *f)
{
	uint32_t i;

	if (dl == (const struct vms_scs_dir_lookup *)0 ||
	    f == (struct vms_scs_ctrl_frame *)0)
		return VMS_CODEC_E_INVAL;

	for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
		f->name1[i] = dl->queried_name[i];

	switch (dl->result_kind) {
	case VMS_SCS_DIR_RESULT_EMPTY:
		for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
			f->name2[i] = 0;
		break;
	case VMS_SCS_DIR_RESULT_NOT_PRESENT:
		for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
			f->name2[i] = vms_scs_dir_not_present_here[i];
		break;
	case VMS_SCS_DIR_RESULT_AFFIRMATIVE:
		for (i = 0; i < VMS_SCSCTRL_NAME_LEN; i++)
			f->name2[i] = dl->result[i];
		break;
	default:
		return VMS_CODEC_E_INVAL;
	}

	f->has_names = 1;
	f->has_blank = 0;
	f->has_tail4 = 0;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * The body-level directory message (abs 72-107) -- FC-P2.3
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_scs_dir_msg_parse(const uint8_t *body, uint32_t len,
					 struct vms_scs_dir_msg *out)
{
	vms_wire_view_t v;

	if (body == (const uint8_t *)0 || out == (struct vms_scs_dir_msg *)0)
		return VMS_CODEC_E_INVAL;
	if (len < VMS_SCS_DIRBODY_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_view_init(&v, body, VMS_SCS_DIRBODY_LEN);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	out->marker = vms_wire_get_le32(&v, VMS_OFF_SCSDIRBODY_MARKER);
	vms_wire_get_bytes(&v, VMS_OFF_SCSDIRBODY_NAME, VMS_SCSCTRL_NAME_LEN,
			   out->lookup.queried_name);
	vms_wire_get_bytes(&v, VMS_OFF_SCSDIRBODY_RESULT, VMS_SCSCTRL_NAME_LEN,
			   out->lookup.result);
	if (!vms_wire_view_ok(&v))
		return v.err;

	dir_result_read(out->lookup.result, &out->lookup);
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_dir_msg_build(const struct vms_scs_dir_msg *m,
					 uint8_t *out, uint32_t cap)
{
	struct vms_scs_ctrl_frame scratch;
	vms_wire_buf_t w;
	vms_codec_status_t st;
	uint32_t i;
	uint8_t *p = (uint8_t *)&scratch;

	if (m == (const struct vms_scs_dir_msg *)0 || out == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (cap < VMS_SCS_DIRBODY_LEN)
		return VMS_CODEC_E_SHORT;

	/* Go through vms_scs_dir_lookup_build() rather than re-deciding what a
	 * result field holds: one encoder for the three readings. */
	for (i = 0; i < (uint32_t)sizeof(scratch); i++)
		p[i] = 0u;
	st = vms_scs_dir_lookup_build(&m->lookup, &scratch);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, out, VMS_SCS_DIRBODY_LEN);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;
	vms_wire_put_le32(&w, VMS_OFF_SCSDIRBODY_MARKER, m->marker);
	vms_wire_put_bytes(&w, VMS_OFF_SCSDIRBODY_NAME, VMS_SCSCTRL_NAME_LEN,
			   scratch.name1);
	vms_wire_put_bytes(&w, VMS_OFF_SCSDIRBODY_RESULT, VMS_SCSCTRL_NAME_LEN,
			   scratch.name2);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_dir_ctrl_from_body(const uint8_t *body, uint32_t len,
					      struct vms_scs_ctrl_frame *f)
{
	struct vms_scs_dir_msg m;
	vms_codec_status_t st;

	if (f == (struct vms_scs_ctrl_frame *)0)
		return VMS_CODEC_E_INVAL;

	st = vms_scs_dir_msg_parse(body, len, &m);
	if (st != VMS_CODEC_OK)
		return st;
	st = vms_scs_dir_lookup_build(&m.lookup, f);
	if (st != VMS_CODEC_OK)
		return st;

	/*
	 * THE op-10 OVERLAY OF ABS 72-75. On the connection-control verbs those
	 * four bytes are SCS$W_MIN_CR/SCS$W_STATUS; on the directory message
	 * (sec 4(h)(2a)) the SAME four bytes are the SYSAP's OWN request/
	 * response discriminator -- a different field, of a different layer, at
	 * one offset. struct vms_scs_ctrl_frame is a BYTE-LEVEL view of the
	 * frame, so the SYSAP's 32-bit value is carried through its two halves
	 * rather than reinterpreted: low word to min_cr, high word to status,
	 * which reproduces the identical little-endian longword on the wire.
	 * Nothing here asserts a credit or a status; the discriminator is the
	 * SYSAP's own byte, copied out of the body it was given.
	 */
	f->has_scsargs = 1;
	f->min_cr = (uint16_t)(m.marker & 0xffffu);
	f->status = (uint16_t)((m.marker >> 16) & 0xffffu);
	return VMS_CODEC_OK;
}
