/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pe_fake_vc.h - the peer half of the FC-P1.2 virtual-circuit host tests: a
 * station that puts REAL SCS frames on the wire, and a decoder that reads
 * back what the FSM emitted.
 *
 * Same two rules as pe_fake_ops.h, which this extends rather than edits:
 *
 *   1. EVERY stimulus frame is built through the SAME FC-P1.1 codec the code
 *      under test builds through. A hand-laid byte array would drift from the
 *      codec the day either changes, and would let a test pass against a
 *      frame no real port would accept.
 *   2. EVERY assertion about an emitted frame re-parses it through the codec.
 *      Where a field has no typed builder yet (the Con.ID pair at abs 64/68 --
 *      FC-P2.1's harvest), the test uses the codec's OWN named offset constant
 *      and its OWN bounds-checked put primitive, never a literal.
 *
 * The peer is deliberately a VAX-shaped one: SCSSYSTEMID 1025 with the §3
 * decoder-ring hardware MAC, "VMS V7.3" and "VAX " in its formation body, and
 * CLUSTER_CREDITS 10 -- so a test that asserts on the credit window is
 * asserting against the grant a real member was measured to extend (§4(g)).
 */
#ifndef OVMX_PE_FAKE_VC_H
#define OVMX_PE_FAKE_VC_H

#include <string.h>

#include "pe_fake_ops.h"
#include "vms_cluster_codec_vc.h"

/* The SCA content length of the class §4(d) grounds the Con.ID pair on, and
 * the wire length that implies. 190 + 14 == 204 == PE_VC_FRAME_MAX. */
#define FAKE_VC_MSG_SCA 190u
#define FAKE_VC_MSG_LEN (VMS_ETH_HDR_LEN + FAKE_VC_MSG_SCA)

/* ------------------------------------------------------------------ *
 * Frames the peer sends
 * ------------------------------------------------------------------ */

/* Fill the four addresses of a peer->us frame: §4(a).0's two pairs, and they
 * are two DIFFERENT things (hardware MAC at abs 0/6, cluster-LOGICAL address
 * at abs 16/24). */
static void fake_vc_addr(struct vms_scs_addr *a, const struct fake_peer *p,
			 const uint8_t dst_hw[6], const uint8_t dst_lavc[6])
{
	memset(a, 0, sizeof(*a));
	memcpy(a->dst_mac, dst_hw, 6);
	memcpy(a->src_mac, p->hw_mac, 6);
	memcpy(a->dst_logical, dst_lavc, 6);
	memcpy(a->src_logical, p->lavc, 6);
}

/*
 * A 106-byte START or STACK (§4(g) phase 2; the two share one wire shape and
 * differ only in the config round, p. 2-12). `send_seq` is the peer's own
 * counter -- §4(i).A's established member sends a LARGE one on round 0, which
 * a joiner must tolerate and must not copy, so it is a parameter here.
 */
static uint32_t fake_peer_start(const struct fake_peer *p, uint16_t sysid,
				const uint8_t dst_hw[6],
				const uint8_t dst_lavc[6],
				uint16_t config_round, uint16_t send_seq,
				uint16_t recv_ack, uint8_t credits,
				uint8_t *out, uint32_t cap)
{
	struct vms_scs_start_frame s;
	uint32_t written = 0;
	size_t i;

	memset(&s, 0, sizeof(s));
	fake_vc_addr(&s.addr, p, dst_hw, dst_lavc);
	s.recv_ack = recv_ack;
	s.send_seq = send_seq;
	s.incarnation = 1;              /* what IT attributes to US */
	s.config_round = config_round;
	s.scssystemid = sysid;
	memcpy(s.software_version, "VMS V7.3", 8);
	memcpy(s.hardware_type, "VAX ", 4);
	s.credits = credits;
	for (i = 0; i < VMS_SCS_START_NODENAME_LEN; i++)
		s.node_name[i] = (i < 4) ? (uint8_t)"VAX1"[i] : (uint8_t)' ';
	s.incarnation_time = 0x00bc05526906b4a1ull;  /* its boot time  */
	s.message_time     = 0x00bc05526906c000ull;  /* live, distinct */

	if (vms_scs_start_build(&s, out, cap, &written) != VMS_CODEC_OK)
		return 0;
	return written;
}

/* The 46-byte round-2 ACK: no identity body (§4(g) phase 2). */
static uint32_t fake_peer_vc_ack(const struct fake_peer *p,
				 const uint8_t dst_hw[6],
				 const uint8_t dst_lavc[6],
				 uint16_t send_seq, uint16_t recv_ack,
				 uint8_t *out, uint32_t cap)
{
	struct vms_scs_start_frame s;
	uint32_t written = 0;

	memset(&s, 0, sizeof(s));
	fake_vc_addr(&s.addr, p, dst_hw, dst_lavc);
	s.recv_ack = recv_ack;
	s.send_seq = send_seq;
	s.incarnation = 1;
	if (vms_scs_start_build_ack(&s, out, cap, &written) != VMS_CODEC_OK)
		return 0;
	return written;
}

/* The 41-byte 0x48 credit-return: acknowledges `acked_seq` and returns one
 * message's worth of credit (§4(h)(3)). */
static uint32_t fake_peer_credit(const struct fake_peer *p,
				 const uint8_t dst_hw[6],
				 const uint8_t dst_lavc[6], uint16_t acked_seq,
				 uint8_t *out, uint32_t cap)
	__attribute__((unused));
static uint32_t fake_peer_credit(const struct fake_peer *p,
				 const uint8_t dst_hw[6],
				 const uint8_t dst_lavc[6], uint16_t acked_seq,
				 uint8_t *out, uint32_t cap)
{
	struct vms_scs_credit_frame c;
	uint32_t written = 0;

	memset(&c, 0, sizeof(c));
	fake_vc_addr(&c.addr, p, dst_hw, dst_lavc);
	c.acked_seq = acked_seq;
	c.secondary_seq = 0;
	if (vms_scs_credit_build(&c, out, cap, &written) != VMS_CODEC_OK)
		return 0;
	return written;
}

/*
 * A 190-content sequenced application message -- the §4(d) class, the only one
 * whose Con.ID location is independently grounded (17557/17557).
 *
 * Built the way the port itself would have to: the shared SCA header through
 * vms_sca_hdr_build, the three transport sequence fields through
 * vms_scs_seq_stamp (the FC-P1.2 codec entry under test, used here from the
 * SENDER's side), and the Con.ID pair through the codec's own named offsets
 * because FC-P2.1 has not landed a typed builder for it yet.
 */
static uint32_t fake_peer_seqmsg(const struct fake_peer *p,
				 const uint8_t dst_hw[6],
				 const uint8_t dst_lavc[6], uint16_t send_seq,
				 uint16_t recv_ack, uint32_t remote_conid,
				 uint32_t local_conid, uint8_t *out,
				 uint32_t cap)
{
	struct vms_frame_info fi;
	struct vms_sca_hdr h;
	vms_wire_buf_t w;
	uint32_t written = 0;

	if (cap < FAKE_VC_MSG_LEN)
		return 0;
	memset(out, 0, FAKE_VC_MSG_LEN);

	memset(&h, 0, sizeof(h));
	memcpy(h.eth_dst, dst_hw, 6);
	memcpy(h.eth_src, p->hw_mac, 6);
	memcpy(h.dst_lavc, dst_lavc, 6);
	memcpy(h.src_lavc, p->lavc, 6);
	h.sca_len_field = (uint16_t)(FAKE_VC_MSG_SCA - 2u);
	h.connect_flag = 0x0001u;
	h.word30 = (uint16_t)(VMS_SCS_MT_MSG |
			      ((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	if (vms_sca_hdr_build(&h, out, cap, &written) != VMS_CODEC_OK)
		return 0;

	if (vms_frame_classify(out, FAKE_VC_MSG_LEN, &fi) != VMS_CODEC_OK)
		return 0;
	if (vms_scs_seq_stamp(out, FAKE_VC_MSG_LEN, &fi, recv_ack,
			      send_seq) != VMS_CODEC_OK)
		return 0;

	vms_wire_buf_init(&w, out, FAKE_VC_MSG_LEN);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, remote_conid);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_LOCAL, local_conid);
	if (!vms_wire_buf_ok(&w))
		return 0;
	return FAKE_VC_MSG_LEN;
}

/* ------------------------------------------------------------------ *
 * Reading back what the FSM emitted -- always through the codec
 * ------------------------------------------------------------------ */

struct fake_vc_decoded {
	struct vms_frame_info fi;
	int      ok;
	int      is_start;      /* the 0x41 class                        */
	int      is_credit;     /* the 0x48 class                        */
	int      is_seqmsg;     /* a sequenced application message       */
	uint8_t  msgtype;       /* abs 30, through vms_scs_msgtype       */
	uint16_t recv_ack;
	uint16_t send_seq;
	uint16_t config_round;  /* 0x41 only                             */
	uint16_t incarnation;   /* 0x41 only: the §4(i).B echo           */
	int      is_ack;        /* 0x41 only: the 46-byte class          */
	struct vms_scs_start_frame start;
	uint8_t  eth_dst[6];    /* which peer this frame went to         */
	/*
	 * The abs 36..55 TRANSPORT COUNTER SPAN (spec §4(d)/§4(h)(4)) --
	 * E63's regression surface. Read at the codec's OWN named offsets,
	 * never a literal, because a test that hand-wrote 40 and 48 would
	 * drift from the port the day either moved.
	 */
	uint16_t span_msg_count, span_lan_ovrhd;
	uint16_t span_ack1, span_seq_mirror, span_ack2;
	uint16_t span_zero1, span_zero2, span_zero3;
	uint16_t span_const1, span_const2;
	uint32_t len;
};

/* Every 16-bit position of the abs 36..55 span, in one place. */
static void fake_vc_read_span(const uint8_t *b, uint32_t len,
			      struct fake_vc_decoded *d)
{
	vms_wire_view_t v;

	vms_wire_view_init(&v, b, len);
	if (!vms_wire_view_ok(&v))
		return;
	d->span_msg_count  = vms_wire_get_le16(&v, VMS_OFF_SCS_MSG_COUNT);
	d->span_lan_ovrhd  = vms_wire_get_le16(&v, VMS_OFF_SCS_LAN_OVRHD);
	d->span_ack1       = vms_wire_get_le16(&v, VMS_OFF_SCS_ACK_MIRROR1);
	d->span_zero1      = vms_wire_get_le16(&v, VMS_OFF_SCS_SPAN_ZERO1);
	d->span_seq_mirror = vms_wire_get_le16(&v, VMS_OFF_SCS_SEQ_MIRROR);
	d->span_zero2      = vms_wire_get_le16(&v, VMS_OFF_SCS_SPAN_ZERO2);
	d->span_ack2       = vms_wire_get_le16(&v, VMS_OFF_SCS_ACK_MIRROR2);
	d->span_zero3      = vms_wire_get_le16(&v, VMS_OFF_SCS_SPAN_ZERO3);
	d->span_const1     = vms_wire_get_le16(&v, VMS_OFF_SCS_SPAN_CONST1);
	d->span_const2     = vms_wire_get_le16(&v, VMS_OFF_SCS_SPAN_CONST2);
}

static struct fake_vc_decoded fake_vc_decode(const struct fake_pe *f,
					     uint32_t index)
{
	struct fake_vc_decoded d;
	const uint8_t *b;
	uint8_t fmt = 0;

	memset(&d, 0, sizeof(d));
	if (index >= f->n_frames)
		return d;
	b = f->frame[index].b;
	d.len = f->frame[index].len;
	if (d.len >= VMS_OFF_ETH_DST + 6u)
		memcpy(d.eth_dst, b + VMS_OFF_ETH_DST, 6);
	if (vms_frame_classify(b, d.len, &d.fi) != VMS_CODEC_OK)
		return d;
	if (d.fi.family != VMS_FFAM_SCS)
		return d;
	if (d.len >= VMS_OFF_SCS_SPAN_END)
		fake_vc_read_span(b, d.len, &d);
	if (vms_scs_msgtype(b, d.len, &d.fi, &d.msgtype, &fmt) != VMS_CODEC_OK)
		return d;

	if (d.fi.cls == VMS_FCLS_SCS_START) {
		if (vms_scs_start_parse(b, d.len, &d.fi, &d.start) !=
		    VMS_CODEC_OK)
			return d;
		d.is_start = 1;
		d.is_ack = d.start.is_ack;
		d.recv_ack = d.start.recv_ack;
		d.send_seq = d.start.send_seq;
		d.config_round = d.start.config_round;
		d.incarnation = d.start.incarnation;
		d.ok = 1;
		return d;
	}
	if (d.fi.cls == VMS_FCLS_SCS_CREDIT) {
		struct vms_scs_credit_frame c;

		if (vms_scs_credit_parse(b, d.len, &d.fi, &c) != VMS_CODEC_OK)
			return d;
		d.is_credit = 1;
		d.recv_ack = c.acked_seq;   /* abs 32: what it acknowledges */
		d.send_seq = 0;             /* GROUNDED 622/622             */
		d.ok = 1;
		return d;
	}
	if (vms_scs_seq(b, d.len, &d.fi, &d.recv_ack, &d.send_seq) !=
	    VMS_CODEC_OK)
		return d;
	d.is_seqmsg = 1;
	d.ok = 1;
	return d;
}

/* The last frame of a kind the FSM emitted, or an all-zero decode. `kind` is
 * one of the three is_* predicates, selected by an enum so a failing test
 * prints which class it was looking for. */
enum fake_vc_kind { FAKE_VC_ANY, FAKE_VC_START, FAKE_VC_CREDIT, FAKE_VC_SEQ };

static int fake_vc_is_kind(const struct fake_vc_decoded *d,
			   enum fake_vc_kind k)
{
	switch (k) {
	case FAKE_VC_START:  return d->is_start;
	case FAKE_VC_CREDIT: return d->is_credit;
	case FAKE_VC_SEQ:    return d->is_seqmsg;
	default:             return 1;
	}
}

static struct fake_vc_decoded fake_vc_last(const struct fake_pe *f,
					   enum fake_vc_kind k)
{
	struct fake_vc_decoded best;
	uint32_t i;

	memset(&best, 0, sizeof(best));
	for (i = 0; i < f->n_frames; i++) {
		struct fake_vc_decoded d = fake_vc_decode(f, i);

		if (d.ok && fake_vc_is_kind(&d, k))
			best = d;
	}
	return best;
}

static unsigned fake_vc_count(const struct fake_pe *f, enum fake_vc_kind k)
	__attribute__((unused));
static unsigned fake_vc_count(const struct fake_pe *f, enum fake_vc_kind k)
{
	unsigned i, n = 0;

	for (i = 0; i < f->n_frames; i++) {
		struct fake_vc_decoded d = fake_vc_decode(f, i);

		if (d.ok && fake_vc_is_kind(&d, k))
			n++;
	}
	return n;
}

#endif /* OVMX_PE_FAKE_VC_H */
