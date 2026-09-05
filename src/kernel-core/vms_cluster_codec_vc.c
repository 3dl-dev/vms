// SPDX-License-Identifier: GPL-2.0
/*
 * vms_cluster_codec_vc.c - virtual-circuit formation + sequenced-message
 * envelope typed codec entries (FC-P1.1). Read vms_cluster_codec_vc.h
 * first for the field map and the honesty rule this file follows.
 *
 * Pure, like the parent TU: no state, no allocation, no substrate call, no
 * libc. Every byte move goes through the vms_wire_get_ / vms_wire_put_
 * primitive families the parent TU exports; the only local helpers are the
 * two 64-bit quadword accessors the parent does not carry (composed from
 * its own bounds-checked 32-bit primitives, so no new bounds logic is
 * introduced here).
 */

#include "vms_cluster_codec_vc.h"

/* ------------------------------------------------------------------ *
 * Local 64-bit accessors, composed from the parent's 32-bit primitives
 * (no new bounds-checking logic -- buf_span_ok/view_span_ok stay private
 * to the parent TU).
 * ------------------------------------------------------------------ */

static void wire_put_le64(vms_wire_buf_t *w, uint32_t off, uint64_t val)
{
	vms_wire_put_le32(w, off, (uint32_t)(val & 0xffffffffull));
	vms_wire_put_le32(w, off + 4, (uint32_t)(val >> 32));
}

static uint64_t wire_get_le64(vms_wire_view_t *v, uint32_t off)
{
	uint32_t lo = vms_wire_get_le32(v, off);
	uint32_t hi = vms_wire_get_le32(v, off + 4);

	return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* ------------------------------------------------------------------ *
 * Shared addressing + envelope helpers
 * ------------------------------------------------------------------ */

/* Every class in this file carries connect_flag==0x0001 (GROUNDED, spec
 * sec 4(g)/4(h)) -- a fixed protocol constant, not a per-node value. */
#define VMS_SCS_VC_CONNECT_FLAG 0x0001u

static void addr_put_hdr(vms_wire_buf_t *w, const struct vms_scs_addr *a,
			 uint16_t sca_content_len, uint16_t word30)
{
	vms_wire_put_bytes(w, VMS_OFF_ETH_DST, VMS_ETH_ADDR_LEN, a->dst_mac);
	vms_wire_put_bytes(w, VMS_OFF_ETH_SRC, VMS_ETH_ADDR_LEN, a->src_mac);
	vms_wire_put_be16(w, VMS_OFF_ETHERTYPE, VMS_SCA_ETHERTYPE);
	vms_wire_put_le16(w, VMS_OFF_SCA_LEN, (uint16_t)(sca_content_len - 2u));
	vms_wire_put_bytes(w, VMS_OFF_DST_LAVC, VMS_ETH_ADDR_LEN, a->dst_logical);
	vms_wire_put_le16(w, VMS_OFF_CONNECT_FLAG, VMS_SCS_VC_CONNECT_FLAG);
	vms_wire_put_bytes(w, VMS_OFF_SRC_LAVC, VMS_ETH_ADDR_LEN, a->src_logical);
	vms_wire_put_le16(w, VMS_OFF_WORD30, word30);
}

static void addr_get_hdr(vms_wire_view_t *v, struct vms_scs_addr *a)
{
	vms_wire_get_bytes(v, VMS_OFF_ETH_DST, VMS_ETH_ADDR_LEN, a->dst_mac);
	vms_wire_get_bytes(v, VMS_OFF_ETH_SRC, VMS_ETH_ADDR_LEN, a->src_mac);
	vms_wire_get_bytes(v, VMS_OFF_DST_LAVC, VMS_ETH_ADDR_LEN, a->dst_logical);
	vms_wire_get_bytes(v, VMS_OFF_SRC_LAVC, VMS_ETH_ADDR_LEN, a->src_logical);
}

/* ------------------------------------------------------------------ *
 * START / STACK / ACK (spec sec 4(g) phase 2)
 * ------------------------------------------------------------------ */

/*
 * Write the span every START/STACK/ACK frame shares: abs 0-57, i.e. the
 * SCA header, recv_ack/send_seq/incarnation, the GROUNDED constant/zero
 * spans, the send_seq mirror, and the inner-length identity. The caller
 * writes config_round (abs 58) and, for the 106-byte class, everything
 * from abs 60 onward.
 */
static void start_write_common(vms_wire_buf_t *w,
			       const struct vms_scs_start_frame *f,
			       uint16_t sca_content_len)
{
	uint16_t word30 = (uint16_t)(VMS_SCS_MT_START |
				     ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	uint16_t inner_len = (uint16_t)(sca_content_len - 44u);

	addr_put_hdr(w, &f->addr, sca_content_len, word30);

	vms_wire_put_le16(w, VMS_OFF_SCS_RECV_ACK, f->recv_ack);   /* abs 32 */
	vms_wire_put_le16(w, VMS_OFF_SCS_SEND_SEQ, f->send_seq);   /* abs 34 */
	/* The 0x41 class writes the SAME offsets as the sequenced span but by
	 * its OWN grounded rule (census, sec 4(g) phase 2): abs 36 is the
	 * incarnation, and abs 40/48/54 are ZERO on a START where a sequenced
	 * frame carries the ack mirrors. Not seq_stamp_span()'s shape -- the
	 * offsets are shared, the rule is not. */
	vms_wire_put_le16(w, VMS_OFF_START_INCARN, f->incarnation);/* abs 36 */
	vms_wire_put_le16(w, VMS_OFF_SCS_LAN_OVRHD,
			  VMS_NISCS_LAN_OVRHD);                     /* abs 38 */
	vms_wire_put_zero(w, VMS_OFF_SCS_ACK_MIRROR1, 4);           /* abs 40 */
	vms_wire_put_le16(w, VMS_OFF_START_SEQ_MIRROR, f->send_seq);/* abs 44 */
	vms_wire_put_zero(w, VMS_OFF_SCS_SPAN_ZERO2, 6);            /* abs 46 */
	vms_wire_put_le16(w, VMS_OFF_SCS_SPAN_CONST1,
			  VMS_SCS_SEQ_SPAN_CONST1);                 /* abs 52 */
	vms_wire_put_zero(w, VMS_OFF_SCS_SPAN_CONST2, 2);           /* abs 54 */
	vms_wire_put_le16(w, 56, inner_len);                        /* abs 56 */
}

vms_codec_status_t vms_scs_start_build(const struct vms_scs_start_frame *f,
				       uint8_t *frame, uint32_t cap,
				       uint32_t *written)
{
	vms_wire_buf_t w;

	if (f == (const struct vms_scs_start_frame *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	start_write_common(&w, f, VMS_SCS_START_SCA_LEN);
	vms_wire_put_le16(&w, VMS_OFF_START_ROUND, f->config_round);   /* 58 */
	vms_wire_put_le16(&w, VMS_OFF_START_SYSID, f->scssystemid);    /* 60 */
	vms_wire_put_zero(&w, 62, 4);                                   /* 62 */
	vms_wire_put_le16(&w, 66, 0x0001u);                             /* 66 */
	vms_wire_put_le16(&w, 68, 0x0240u);                             /* 68 */
	vms_wire_put_le16(&w, 70, 0x00d8u);                             /* 70 */
	vms_wire_put_bytes(&w, VMS_OFF_START_SWVER, VMS_SCS_START_SWVER_LEN,
			   f->software_version);                        /* 72 */
	wire_put_le64(&w, VMS_OFF_START_INCARNTIME, f->incarnation_time); /*80*/
	vms_wire_put_bytes(&w, VMS_OFF_START_HWTYPE, VMS_SCS_START_HWTYPE_LEN,
			   f->hardware_type);                           /* 88 */
	vms_wire_put_le16(&w, 92, 0x0006u);                             /* 92 */
	vms_wire_put_zero(&w, 94, 1);                                   /* 94 */
	vms_wire_put_u8(&w, VMS_OFF_START_CREDITS, f->credits);         /* 95 */
	vms_wire_put_zero(&w, 96, 6);                                   /* 96 */
	vms_wire_put_le16(&w, 102, 0x0077u);                            /*102 */
	vms_wire_put_bytes(&w, VMS_OFF_START_NODENAME, VMS_SCS_START_NODENAME_LEN,
			   f->node_name);                              /* 104*/
	wire_put_le64(&w, VMS_OFF_START_MSGTIME, f->message_time);      /* 112*/

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_SCS_START_FRAME_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_start_build_ack(const struct vms_scs_start_frame *f,
					   uint8_t *frame, uint32_t cap,
					   uint32_t *written)
{
	vms_wire_buf_t w;

	if (f == (const struct vms_scs_start_frame *)0)
		return VMS_CODEC_E_INVAL;
	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	start_write_common(&w, f, VMS_SCS_START_ACK_SCA_LEN);
	vms_wire_put_le16(&w, VMS_OFF_START_ROUND, VMS_SCS_START_ACK_ROUND);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_SCS_START_ACK_FRAME_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_start_parse(const uint8_t *frame, uint32_t len,
				       const struct vms_frame_info *fi,
				       struct vms_scs_start_frame *out)
{
	vms_wire_view_t v;

	if (out == (struct vms_scs_start_frame *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if (fi->cls != VMS_FCLS_SCS_START)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	/* fi->sca_content already carries the length identity the classifier
	 * validated; 106 vs 46 is what distinguishes START/STACK from ACK
	 * (spec sec 4(g): the ACK carries no identity body). */
	out->is_ack = (uint8_t)(fi->sca_content != VMS_SCS_START_SCA_LEN);
	out->scssystemid = 0;
	out->credits = 0;
	out->incarnation_time = 0;
	out->message_time = 0;

	addr_get_hdr(&v, &out->addr);
	out->recv_ack = vms_wire_get_le16(&v, VMS_OFF_SCS_RECV_ACK);
	out->send_seq = vms_wire_get_le16(&v, VMS_OFF_SCS_SEND_SEQ);
	out->incarnation = vms_wire_get_le16(&v, VMS_OFF_START_INCARN);
	out->config_round = vms_wire_get_le16(&v, VMS_OFF_START_ROUND);

	if (!out->is_ack) {
		out->scssystemid = vms_wire_get_le16(&v, VMS_OFF_START_SYSID);
		vms_wire_get_bytes(&v, VMS_OFF_START_SWVER,
				   VMS_SCS_START_SWVER_LEN,
				   out->software_version);
		out->incarnation_time = wire_get_le64(&v, VMS_OFF_START_INCARNTIME);
		vms_wire_get_bytes(&v, VMS_OFF_START_HWTYPE,
				   VMS_SCS_START_HWTYPE_LEN, out->hardware_type);
		out->credits = vms_wire_get_u8(&v, VMS_OFF_START_CREDITS);
		vms_wire_get_bytes(&v, VMS_OFF_START_NODENAME,
				   VMS_SCS_START_NODENAME_LEN, out->node_name);
		out->message_time = wire_get_le64(&v, VMS_OFF_START_MSGTIME);
	} else {
		uint8_t i;

		for (i = 0; i < VMS_SCS_START_SWVER_LEN; i++)
			out->software_version[i] = 0;
		for (i = 0; i < VMS_SCS_START_HWTYPE_LEN; i++)
			out->hardware_type[i] = 0;
		for (i = 0; i < VMS_SCS_START_NODENAME_LEN; i++)
			out->node_name[i] = 0;
	}

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * The sequenced-message envelope (spec sec 4(g)/4(h)(4))
 * ------------------------------------------------------------------ */

static int is_seq_msgtype(uint8_t mt)
{
	return mt == VMS_SCS_MT_MSG || mt == VMS_SCS_MT_SETUP;
}

vms_codec_status_t
vms_scs_seq_envelope_build(const struct vms_scs_seq_envelope *e,
			   uint8_t *frame, uint32_t cap, uint32_t *written)
{
	vms_wire_buf_t w;
	uint16_t word30;

	if (e == (const struct vms_scs_seq_envelope *)0)
		return VMS_CODEC_E_INVAL;
	if (!is_seq_msgtype(e->msgtype))
		return VMS_CODEC_E_INVAL;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	word30 = (uint16_t)(e->msgtype | ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	/* The envelope does not know the frame's final total length (the
	 * SYSAP body, owned by a later harvest item, follows it) -- write a
	 * placeholder sca_len_field the caller/higher layer MUST overwrite
	 * once the body is appended, exactly as vms_hello_build_padded()
	 * rewrites the length field after the fact. This item's own tests
	 * exercise only the envelope span, never trusting this field. */
	addr_put_hdr(&w, &e->addr, (uint16_t)(VMS_SCS_SEQ_ENVELOPE_LEN), word30);

	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK, e->recv_ack);
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEND_SEQ, e->send_seq);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_SCS_SEQ_ENVELOPE_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t
vms_scs_seq_envelope_parse(const uint8_t *frame, uint32_t len,
			   const struct vms_frame_info *fi,
			   struct vms_scs_seq_envelope *out)
{
	vms_wire_view_t v;
	uint8_t mt, fmt;

	if (out == (struct vms_scs_seq_envelope *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if ((fi->caps & (VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ)) !=
	    (VMS_FCAP_MSGTYPE | VMS_FCAP_SEQ))
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	mt = vms_wire_get_u8(&v, VMS_OFF_SCS_MSGTYPE);
	fmt = vms_wire_get_u8(&v, VMS_OFF_SCS_FORMAT);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (!is_seq_msgtype(mt))
		return VMS_CODEC_E_CLASS;

	addr_get_hdr(&v, &out->addr);
	out->msgtype = mt;
	(void)fmt; /* format is asserted 0x13 by the classifier, not re-stored */
	out->recv_ack = vms_wire_get_le16(&v, VMS_OFF_SCS_RECV_ACK);
	out->send_seq = vms_wire_get_le16(&v, VMS_OFF_SCS_SEND_SEQ);
	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}

/*
 * vms_scs_seq_envelope_fixup_len - see the .h doc comment. The SAME
 * three-line pattern vms_hello_build_padded() already uses to rewrite its
 * padded total: a fresh bounded write view over the SAME buffer, one
 * vms_wire_put_le16() at the one named offset the parent TU exports for
 * this purpose (VMS_OFF_SCA_LEN), nothing else touched.
 */
vms_codec_status_t
vms_scs_seq_envelope_fixup_len(uint8_t *frame, uint32_t cap,
			       uint32_t total_len)
{
	vms_wire_buf_t w;

	if (frame == (uint8_t *)0)
		return VMS_CODEC_E_INVAL;
	if (total_len < VMS_ETH_HDR_LEN + 2u ||
	    total_len - VMS_ETH_HDR_LEN > 0xffffu)
		return VMS_CODEC_E_RANGE;

	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_SHORT;
	vms_wire_put_le16(&w, VMS_OFF_SCA_LEN,
			  (uint16_t)((total_len - VMS_ETH_HDR_LEN) - 2u));
	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * Stamping the transport's sequence fields into somebody else's frame
 * (spec sec 4(h)(4)) -- read the header's rationale before touching this.
 * ------------------------------------------------------------------ */

/* The gate both entries below share: a sequenced APPLICATION/SETUP frame,
 * long enough to hold the whole abs 36..55 counter span. 0x41 and 0x48 are
 * refused here -- each has its own builder and its own rule for those bytes. */
static vms_codec_status_t seq_stamp_gate(const uint8_t *frame, uint32_t len,
					 const struct vms_frame_info *fi,
					 uint8_t *msgtype_out)
{
	vms_wire_view_t v;
	uint8_t mt;

	if (frame == (const uint8_t *)0 || fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if ((fi->caps & VMS_FCAP_SEQ) == 0u)
		return VMS_CODEC_E_CLASS;
	if (len < VMS_SCS_SEQ_STAMP_MIN_LEN)
		return VMS_CODEC_E_SHORT;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;
	mt = vms_wire_get_u8(&v, VMS_OFF_SCS_MSGTYPE);
	if (!vms_wire_view_ok(&v))
		return v.err;
	if (!is_seq_msgtype(mt) && mt != VMS_SCS_MT_ALT)
		return VMS_CODEC_E_CLASS;

	if (msgtype_out != (uint8_t *)0)
		*msgtype_out = mt;
	return VMS_CODEC_OK;
}

/*
 * Write abs 36..55, the transport counter span (header sec "abs 36..55").
 * Every live value is DERIVED from circuit state the caller passed -- the
 * same recv_seq/send_seq that produced abs 32/34, and the same sec 4(i).B
 * incarnation echo the circuit's own 0x41 frames carry -- never a second
 * counter, never a constant, never a byte carried over from another frame.
 */
static void seq_stamp_span(vms_wire_buf_t *w, uint16_t recv_ack,
			   uint16_t send_seq, uint16_t incarnation)
{
	vms_wire_put_le16(w, VMS_OFF_SCS_INCARNATION, incarnation);
	vms_wire_put_le16(w, VMS_OFF_SCS_LAN_OVRHD, VMS_NISCS_LAN_OVRHD);
	vms_wire_put_le16(w, VMS_OFF_SCS_ACK_MIRROR1, recv_ack);
	vms_wire_put_zero(w, VMS_OFF_SCS_SPAN_ZERO1, 2);
	vms_wire_put_le16(w, VMS_OFF_SCS_SEQ_MIRROR, send_seq);
	vms_wire_put_zero(w, VMS_OFF_SCS_SPAN_ZERO2, 2);
	vms_wire_put_le16(w, VMS_OFF_SCS_ACK_MIRROR2, recv_ack);
	vms_wire_put_zero(w, VMS_OFF_SCS_SPAN_ZERO3, 2);
	vms_wire_put_le16(w, VMS_OFF_SCS_SPAN_CONST1, VMS_SCS_SEQ_SPAN_CONST1);
	vms_wire_put_le16(w, VMS_OFF_SCS_SPAN_CONST2, VMS_SCS_SEQ_SPAN_CONST2);
}

vms_codec_status_t vms_scs_seq_stamp(uint8_t *frame, uint32_t len,
				     const struct vms_frame_info *fi,
				     uint16_t recv_ack, uint16_t send_seq,
				     uint16_t incarnation)
{
	vms_wire_buf_t w;
	vms_codec_status_t st;

	/* INV-6: no echo, no frame. Zero at abs 36 appears in 0 of 239,981
	 * reference frames, so there is no honest value to substitute. */
	if (incarnation == 0u)
		return VMS_CODEC_E_INVAL;

	st = seq_stamp_gate(frame, len, fi, (uint8_t *)0);
	if (st != VMS_CODEC_OK)
		return st;

	vms_wire_buf_init(&w, frame, len);
	vms_wire_put_le16(&w, VMS_OFF_SCS_RECV_ACK, recv_ack);
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEND_SEQ, send_seq);
	seq_stamp_span(&w, recv_ack, send_seq, incarnation);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_seq_mark_retransmit(uint8_t *frame, uint32_t len,
					       const struct vms_frame_info *fi)
{
	vms_wire_buf_t w;
	uint8_t mt = 0;
	vms_codec_status_t st = seq_stamp_gate(frame, len, fi, &mt);

	if (st != VMS_CODEC_OK)
		return st;
	if (mt == VMS_SCS_MT_ALT)
		return VMS_CODEC_OK;         /* already marked; idempotent */

	vms_wire_buf_init(&w, frame, len);
	vms_wire_put_u8(&w, VMS_OFF_SCS_MSGTYPE, VMS_SCS_MT_ALT);
	if (!vms_wire_buf_ok(&w))
		return w.err;
	return VMS_CODEC_OK;
}

/* ------------------------------------------------------------------ *
 * Credit-return short (spec sec 4(h)(3))
 * ------------------------------------------------------------------ */

vms_codec_status_t vms_scs_credit_build(const struct vms_scs_credit_frame *c,
					uint8_t *frame, uint32_t cap,
					uint32_t *written)
{
	vms_wire_buf_t w;
	uint16_t word30;

	if (c == (const struct vms_scs_credit_frame *)0)
		return VMS_CODEC_E_INVAL;
	/* Same INV-6 refusal as vms_scs_seq_stamp: abs 36 is the sec 4(i).B
	 * echo on this class too, and it is never zero on the wire. */
	if (c->incarnation == 0u)
		return VMS_CODEC_E_INVAL;
	vms_wire_buf_init(&w, frame, cap);
	if (!vms_wire_buf_ok(&w))
		return VMS_CODEC_E_INVAL;

	word30 = (uint16_t)(VMS_SCS_MT_CREDIT |
			    ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	addr_put_hdr(&w, &c->addr, VMS_SCS_CREDIT_SCA_LEN, word30);

	vms_wire_put_le16(&w, 32, c->acked_seq);      /* abs 32 */
	vms_wire_put_zero(&w, 34, 2);                  /* abs 34: send_seq==0 */
	vms_wire_put_le16(&w, VMS_OFF_SCS_INCARNATION,
			  c->incarnation);             /* abs 36            */
	vms_wire_put_le16(&w, VMS_OFF_SCS_LAN_OVRHD,
			  VMS_NISCS_LAN_OVRHD);        /* abs 38            */
	vms_wire_put_le16(&w, VMS_OFF_SCS_ACK_MIRROR1,
			  c->acked_seq);               /* abs 40: mirror    */
	vms_wire_put_zero(&w, VMS_OFF_SCS_SPAN_ZERO1, 2);/* abs 42          */
	vms_wire_put_le16(&w, VMS_OFF_SCS_SEQ_MIRROR,
			  c->secondary_seq);           /* abs 44: INFERRED  */
	vms_wire_put_zero(&w, VMS_OFF_SCS_SPAN_ZERO2, 2);/* abs 46          */
	vms_wire_put_le16(&w, VMS_OFF_SCS_ACK_MIRROR2,
			  c->acked_seq);               /* abs 48: 3rd repeat*/
	vms_wire_put_zero(&w, VMS_OFF_SCS_SPAN_ZERO3, 2);/* abs 50          */
	vms_wire_put_le16(&w, VMS_OFF_SCS_SPAN_CONST1,
			  VMS_SCS_SEQ_SPAN_CONST1);    /* abs 52            */
	vms_wire_put_zero(&w, VMS_OFF_SCS_SPAN_CONST2, 1);/* abs 54: last   */
	/* Ethernet runt pad, abs 55-59 (spec sec 2: 14+41=55 < 60, GROUNDED
	 * zero -- 928/928 residuals of the sec-2 length identity are exactly
	 * this padding, and the 0x48 short is the class that hits it). */
	vms_wire_put_zero(&w, 55, VMS_SCS_CREDIT_FRAME_LEN - 55u);

	if (!vms_wire_buf_ok(&w))
		return w.err;
	if (written != (uint32_t *)0)
		*written = VMS_SCS_CREDIT_FRAME_LEN;
	return VMS_CODEC_OK;
}

vms_codec_status_t vms_scs_credit_parse(const uint8_t *frame, uint32_t len,
					const struct vms_frame_info *fi,
					struct vms_scs_credit_frame *out)
{
	vms_wire_view_t v;

	if (out == (struct vms_scs_credit_frame *)0 ||
	    fi == (const struct vms_frame_info *)0)
		return VMS_CODEC_E_INVAL;
	if (fi->cls != VMS_FCLS_SCS_CREDIT)
		return VMS_CODEC_E_CLASS;

	vms_wire_view_init(&v, frame, len);
	if (!vms_wire_view_ok(&v))
		return VMS_CODEC_E_INVAL;

	addr_get_hdr(&v, &out->addr);
	out->acked_seq = vms_wire_get_le16(&v, 32);
	out->secondary_seq = vms_wire_get_le16(&v, 44);
	out->incarnation = vms_wire_get_le16(&v, VMS_OFF_SCS_INCARNATION);

	if (!vms_wire_view_ok(&v))
		return v.err;
	return VMS_CODEC_OK;
}
