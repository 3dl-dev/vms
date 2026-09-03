/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_pe_block.c - FC-P6.1 R1: the port's THIRD service. Named buffers, the
 * 28-byte block-transfer header built through the codec, READ streaming with
 * the final chunk piggybacked on the end message, and WRITE's two-frame form.
 *
 * ***  THE ORACLE, AND WHAT IS MISSING FROM IT  ***
 *
 * The plan's done-condition is "R1 byte-exact on the vms291 mount capture
 * frames". Those pcaps (`vms291-mount-A`, `vms291-control-B`,
 * `vms291-boot-C`, lab-2 vaxlab-9 2026-08-06) are HOST-ONLY artifacts, never
 * in git, and they are NOT on this machine: they are absent from
 * docs/clean-room/reference-captures.sha256 and the lab path they lived under
 * does not exist here. They are also not decoded into a `.spec` fixture in
 * tree.
 *
 * So this file asserts against the DECODED SPECIMEN THAT DOES EXIST IN TREE --
 * docs/design-mscp-direction.md, "Phase D part 1's lab capture -- SCA block
 * data transfer, DECODED", which records, from that capture:
 *
 *   (a) the 28-byte header's field table, offset by offset;
 *   (b) the FIVE measured READ-END SCA content lengths -- 118, 194, 448, 630,
 *       1142 -- each of which is EXACTLY (58 + 32) + 28 + tail for tail in
 *       {0, 76, 330, 512, 1024}: a 90-content MSCP READ end message, this
 *       28-byte header, and a data tail. That arithmetic closing on all five
 *       recorded values with no residual is the strongest byte-level check
 *       available without the pcap, and it is what test_read_end_piggyback_
 *       matches_recorded_sca_lengths asserts;
 *   (c) WRITE's two-frame form with BYTE-IDENTICAL 28-byte headers;
 *   (d) that a block-transfer frame FAILS the SCS envelope conformance test.
 *
 * MISSING-CAPTURE FLAG FOR THE LAB LANE: a frame-for-frame byte comparison
 * against vms291 still wants the pcap decoded to a fixture. Raised as
 * docs/cluster-integration-notes.md E14. Nothing in this file pretends to
 * that comparison.
 *
 * THE TWO UNGROUNDED WORDS. `+4` and `+6` are asserted here ONLY as (i) an
 * explicit ZERO when this circuit has observed no value for them, counted in
 * pe_fsm.blk_obs_absent, and (ii) the value LEARNED off a received frame,
 * echoed unchanged. No test here asserts a meaning for either, and no captured
 * 9 or 13 is baked into production (INV-6).
 *
 * Every stimulus and every assertion goes through the codec, never a raw
 * offset (design SS3.9 rule 2) -- with the single deliberate exception of the
 * 28-byte golden in test_block_header_is_byte_exact, which IS the field table
 * and must therefore be written out literally to be worth anything.
 */

#include <string.h>

#include "cluster_test.h"
#include "pe_fake_vc.h"
#include "vms_cluster_codec_blk.h"
#include "vms_cluster_codec_mscp.h"  /* FC-P6.2: a REAL MSCP READ end body */
#include "vms_cluster_codec_scs.h"   /* VMS_SCSCTRL_FMTWORD_CONST */

#define OVMX_SYSID 1030u
#define VAX1_SYSID 1025u

static const uint8_t ovmx_hw[6] = { 0x02, 0x00, 0x00, 0x4f, 0x56, 0x58 };
static const uint8_t vax1_hw[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
static const uint8_t group1[6]  = { 0xab, 0x00, 0x04, 0x01, 0x01, 0x01 };

#define LAB_CREDITS 10u
#define OVMX_BOOT_TIME 0x00bc0552690a0000ull

/* The connection the MSCP$DISK traffic rides in the sec 3 decoder ring. Read
 * by the SYSAP off the connection, threaded down in pe_blk_xfer -- never
 * chosen by the port. */
#define MSCP_DEST_CONID 0x62c50009u

/* The peer's buffer name and offset, as they would arrive inside Table A-6's
 * 12-byte buffer descriptor in the READ/WRITE command. This test plays the
 * SYSAP that read them. */
#define PEER_BUF_NAME   0x0f0e0d0cu
#define PEER_BUF_OFFSET 0u

/* ------------------------------------------------------------------ *
 * The node under test
 * ------------------------------------------------------------------ */

struct blk_upper {
	uint32_t        blocks;
	vms_scs_sysid_t last_from;
	uint32_t        last_name;
	uint32_t        last_offset;
	uint32_t        last_len;
	uint32_t        last_remaining;
	uint32_t        messages;
};

static void bu_message(void *ctx, vms_scs_sysid_t from, vms_conid_t conid,
		       const uint8_t *body, uint32_t len)
{
	struct blk_upper *u = (struct blk_upper *)ctx;

	(void)from; (void)conid; (void)body; (void)len;
	u->messages++;
}

static void bu_datagram(void *ctx, vms_scs_sysid_t from, const uint8_t *body,
			uint32_t len)
{
	(void)ctx; (void)from; (void)body; (void)len;
}

static void bu_vc_up(void *ctx, vms_scs_sysid_t peer)
{
	(void)ctx; (void)peer;
}

static void bu_vc_down(void *ctx, vms_scs_sysid_t peer, uint32_t reason)
{
	(void)ctx; (void)peer; (void)reason;
}

static void bu_block(void *ctx, vms_scs_sysid_t from, uint32_t name,
		     uint32_t offset, uint32_t len, uint32_t remaining)
{
	struct blk_upper *u = (struct blk_upper *)ctx;

	u->blocks++;
	u->last_from = from;
	u->last_name = name;
	u->last_offset = offset;
	u->last_len = len;
	u->last_remaining = remaining;
}

#define VOL_BYTES 4096u

struct blk_env {
	struct pe_fsm       fsm;
	struct pe_ops       ops;
	struct fake_pe      fake;
	struct fake_peer    peer;
	struct blk_upper    upper_rec;
	struct pe_upper_ops upper;
	struct pe_vc        vcs[2];
	uint8_t             buf[VMS_HELLO_PADDED_MAX_FRAME];
	/* The SYSAP's own memory: a served volume's blocks on the send side,
	 * a landing zone on the receive side. The port never allocates. */
	uint8_t             vol[VOL_BYTES];
	uint8_t             sink[VOL_BYTES];
	uint32_t            vol_name;
	uint32_t            sink_name;
	/* The 28 bytes of the last REQUEST DATA injected, kept aside because
	 * `buf` is reused by whatever the port emits in reply. */
	uint8_t             reqhdr[VMS_BLK_HDR_LEN];
};

static uint64_t fake_now_vms(void *ctx)
{
	struct fake_pe *f = (struct fake_pe *)ctx;

	return 0x00bc055269000000ull + (uint64_t)f->now_ms * 10000ull;
}

static void ovmx_lavc(uint8_t out[6])
{
	vms_cluster_lavc_addr_build(OVMX_SYSID, out);
}

static void env_init(struct blk_env *e)
{
	struct pe_identity id;
	uint32_t i;

	memset(e, 0, sizeof(*e));
	fake_pe_ops_init(&e->ops, &e->fake);
	e->ops.now_vms = fake_now_vms;

	memset(&id, 0, sizeof(id));
	memcpy(id.hw_mac, ovmx_hw, 6);
	id.hw_mac_valid = 1;
	memcpy(id.scsnode, "OVMX  ", 6);
	id.scsnode_len = 6;
	memcpy(id.mcast, group1, 6);
	id.mcast_valid = 1;
	id.max_sca_len = 1500;
	memcpy(id.sw_version, "VMX V0.6", 8);
	id.sw_version_valid = 1;
	memcpy(id.hw_type, "X86 ", 4);
	id.hw_type_valid = 1;
	id.cluster_credits = LAB_CREDITS;
	id.cluster_credits_valid = 1;
	id.incarnation_time = OVMX_BOOT_TIME;
	id.incarnation_time_valid = 1;
	id.timvcfail_ms = 16000;
	id.vc_retransmit_ms = 2000;

	(void)pe_fsm_init(&e->fsm, &id, OVMX_SYSID, &e->ops);
	pe_fsm_bind_vcs(&e->fsm, e->vcs, 2);

	e->upper.message = bu_message;
	e->upper.datagram = bu_datagram;
	e->upper.vc_up = bu_vc_up;
	e->upper.vc_down = bu_vc_down;
	e->upper.block_data = bu_block;
	e->upper.ctx = &e->upper_rec;
	pe_fsm_set_upper(&e->fsm, &e->upper);

	pe_fsm_start(&e->fsm);
	fake_peer_init(&e->peer, VAX1_SYSID, vax1_hw, "VAX1");

	/* Distinctive volume content, so "the right bytes went out" is an
	 * assertion and not a hope. */
	for (i = 0; i < VOL_BYTES; i++)
		e->vol[i] = (uint8_t)(i * 7u + 3u);
}

static void rx_frame(struct blk_env *e, uint32_t len)
{
	if (len != 0)
		(void)pe_fsm_rx(&e->fsm, e->buf, len);
}

static void rx_hello(struct blk_env *e, uint8_t word, uint16_t incarnation)
{
	uint8_t dst_lavc[6];

	ovmx_lavc(dst_lavc);
	rx_frame(e, fake_peer_hello(&e->peer, ovmx_hw, dst_lavc, word,
				    incarnation, 0, e->buf, sizeof(e->buf)));
}

static void open_circuit(struct blk_env *e)
{
	uint8_t dst_lavc[6];

	env_init(e);
	ovmx_lavc(dst_lavc);
	rx_hello(e, PE_PFW_VERIFY_B2, 1);
	rx_hello(e, PE_PFW_VERIFY_B4, 1);
	rx_frame(e, fake_peer_start(&e->peer, VAX1_SYSID, ovmx_hw, dst_lavc,
				    0, 1, 0, LAB_CREDITS, e->buf,
				    sizeof(e->buf)));
	rx_frame(e, fake_peer_vc_ack(&e->peer, ovmx_hw, dst_lavc, 1, 0,
				     e->buf, sizeof(e->buf)));
	fake_pe_clear_frames(&e->fake);
	memset(&e->upper_rec, 0, sizeof(e->upper_rec));

	(void)pe_blk_buf_register(&e->fsm, e->vol, VOL_BYTES, PE_BLK_ACC_SRC,
				  &e->vol_name);
	(void)pe_blk_buf_register(&e->fsm, e->sink, VOL_BYTES, PE_BLK_ACC_DST,
				  &e->sink_name);
}

static struct pe_vc *the_vc(struct blk_env *e)
{
	return pe_fsm_vc_at(&e->fsm, 0);
}

/* One transfer, described the way a SYSAP would: everything remote-side READ
 * from the peer's own command, everything local-side from our named buffer. */
static void xfer_init(struct pe_blk_xfer *x, uint32_t local_name,
		      uint32_t local_off, uint32_t length, uint32_t chunk)
{
	memset(x, 0, sizeof(*x));
	x->peer = VAX1_SYSID;
	x->dest_conid = MSCP_DEST_CONID;
	x->local_name = local_name;
	x->local_offset = local_off;
	x->remote_name = PEER_BUF_NAME;
	x->remote_offset = PEER_BUF_OFFSET;
	x->length = length;
	x->chunk = chunk;
}

/* A block-transfer frame FROM the peer, built the way the port itself builds
 * one -- through the codec, never a hand-laid array. `data_len == 0` is the
 * header-only form, which under design SS3.2.6's E41 ruling is a REQUEST DATA. */
static uint32_t fake_peer_block(const struct fake_peer *p,
				const uint8_t dst_hw[6],
				const uint8_t dst_lavc[6], uint16_t send_seq,
				const struct vms_blk_hdr *h,
				const uint8_t *data, uint32_t data_len,
				uint8_t *out, uint32_t cap)
{
	struct vms_scs_seq_envelope env;
	struct vms_frame_info fi;
	uint32_t total = VMS_BLK_DATA_OFF + data_len;

	if (cap < total)
		return 0;
	memset(out, 0, total);

	memset(&env, 0, sizeof(env));
	fake_vc_addr(&env.addr, p, dst_hw, dst_lavc);
	env.msgtype = VMS_SCS_MT_MSG;
	env.recv_ack = 0;
	env.send_seq = send_seq;
	if (vms_scs_seq_envelope_build(&env, out, cap, NULL) != VMS_CODEC_OK)
		return 0;
	if (vms_blk_hdr_build(h, out, cap) != VMS_CODEC_OK)
		return 0;
	if (data_len != 0)
		memcpy(out + VMS_BLK_DATA_OFF, data, data_len);
	if (vms_scs_seq_envelope_fixup_len(out, cap, total) != VMS_CODEC_OK)
		return 0;
	if (vms_frame_classify(out, total, &fi) != VMS_CODEC_OK)
		return 0;
	if (vms_scs_seq_stamp(out, total, &fi, 0u, send_seq) != VMS_CODEC_OK)
		return 0;
	return total;
}

/* ------------------------------------------------------------------ *
 * 1. The 28-byte header, byte-exact against the recorded field table
 * ------------------------------------------------------------------ */
static void test_block_header_is_byte_exact(void)
{
	/*
	 * The ONE literal golden in this file. It IS the field table recorded
	 * in docs/design-mscp-direction.md -- +0 conid, +4/+6 the two observed
	 * words, +8 bytes-remaining, +12 source name, +16 destination offset,
	 * +20 destination name, +24 source offset -- written out little-endian
	 * so a silent field reorder or a width slip reds this test.
	 */
	static const uint8_t want[VMS_BLK_HDR_LEN] = {
		0x09, 0x00, 0xc5, 0x62,   /* +0  dest_conid  0x62c50009 */
		0x09, 0x00,               /* +4  OBSERVED, NOT DECODED  */
		0x02, 0x00,               /* +6  OBSERVED, NOT DECODED  */
		0x00, 0x04, 0x00, 0x00,   /* +8  bytes_remaining 1024   */
		0x44, 0x33, 0x22, 0x11,   /* +12 src_name  0x11223344   */
		0xc8, 0x00, 0x00, 0x00,   /* +16 dst_offset 200         */
		0x88, 0x77, 0x66, 0x55,   /* +20 dst_name  0x55667788   */
		0x64, 0x00, 0x00, 0x00    /* +24 src_offset 100         */
	};
	uint8_t frame[VMS_BLK_DATA_OFF];
	struct vms_blk_hdr h, back;
	uint32_t i;
	int diff = 0;

	printf("-- the 28-byte block header is byte-exact on the field table\n");

	memset(&h, 0, sizeof(h));
	h.dest_conid = 0x62c50009u;
	h.obs_w4 = 0x0009u;
	h.obs_w6 = 0x0002u;
	h.bytes_remaining = 1024u;
	h.src_name = 0x11223344u;
	h.dst_offset = 200u;
	h.dst_name = 0x55667788u;
	h.src_offset = 100u;

	memset(frame, 0xee, sizeof(frame));
	ct_check_eq_u32((unsigned long)vms_blk_hdr_build(&h, frame,
							 sizeof(frame)),
			VMS_CODEC_OK, "vms_blk_hdr_build succeeds");
	for (i = 0; i < VMS_BLK_HDR_LEN; i++) {
		if (frame[VMS_BLK_HDR_OFF + i] != want[i]) {
			printf("     byte +%u: got 0x%02x want 0x%02x\n",
			       i, frame[VMS_BLK_HDR_OFF + i], want[i]);
			diff = 1;
		}
	}
	ct_check(!diff, "all 28 bytes match the recorded field table");

	/* The header sits where an SCS envelope would, and nothing before it
	 * was disturbed -- abs 0..55 is the PORT's span and this codec must
	 * not touch it. */
	{
		int untouched = 1;

		for (i = 0; i < VMS_BLK_HDR_OFF; i++) {
			if (frame[i] != 0xeeu)
				untouched = 0;
		}
		ct_check(untouched,
			 "abs 0..55 (the port's own span) is left untouched");
	}

	ct_check_eq_u32((unsigned long)vms_blk_hdr_parse(frame, sizeof(frame),
							 &back),
			VMS_CODEC_OK, "vms_blk_hdr_parse succeeds");
	ct_check(memcmp(&h, &back, sizeof(h)) == 0,
		 "the header round-trips field for field");
}

/* ------------------------------------------------------------------ *
 * 2. A streamed READ: down-count, advancing offsets, real bytes
 * ------------------------------------------------------------------ */
static void test_read_stream_counts_down_and_carries_real_bytes(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	uint32_t frames = 0;
	uint32_t i;
	int rc;

	printf("-- READ streaming: bytes_remaining counts down, offsets advance\n");
	open_circuit(&e);
	xfer_init(&x, e.vol_name, 0u, 1536u, 512u);

	rc = pe_blk_send(&e.fsm, &x, 0u, &frames);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK, "pe_blk_send returns OK");
	ct_check_eq_u32(frames, 3, "1536 bytes in 512-byte chunks == 3 frames");
	ct_check_eq_u32(e.fake.n_frames, 3, "three frames really went out");

	for (i = 0; i < 3; i++) {
		struct vms_blk_view view;
		struct vms_frame_info fi;
		const uint8_t *b = e.fake.frame[i].b;
		uint32_t len = e.fake.frame[i].len;

		ct_check_eq_u32(len, VMS_BLK_DATA_OFF + 512u,
				"frame length is header + one 512-byte chunk");
		ct_check_eq_u32((unsigned long)vms_frame_classify(b, len, &fi),
				VMS_CODEC_OK, "the frame classifies");
		ct_check_eq_u32((unsigned long)vms_blk_frame_parse(b, len, &fi,
								   &view),
				VMS_CODEC_OK, "it parses as a block transfer");
		ct_check_eq_u32(view.hdr.bytes_remaining, 1536u - i * 512u,
				"bytes_remaining counts DOWN over the transfer");
		ct_check_eq_u32(view.hdr.src_offset, i * 512u,
				"source offset advances by what has gone out");
		ct_check_eq_u32(view.hdr.dst_offset, i * 512u,
				"destination offset advances in step");
		ct_check_eq_u32(view.hdr.src_name, e.vol_name,
				"source name is OUR registered buffer's name");
		ct_check_eq_u32(view.hdr.dst_name, PEER_BUF_NAME,
				"destination name is the PEER's, as it sent it");
		ct_check_eq_u32(view.hdr.dest_conid, MSCP_DEST_CONID,
				"the connection ID is the caller's, not invented");
		ct_check_eq_u32(view.data_len, 512u, "512 data bytes present");
		ct_check(memcmp(view.data, e.vol + i * 512u, 512u) == 0,
			 "the data is the named buffer's real bytes");
	}

	/* Spec sec 4(h)(4): every sequenced frame stamps a send_seq, and this
	 * class is one. Three frames, three contiguous sequences, no hole. */
	{
		struct vms_frame_info fi;
		uint16_t seq[3];
		uint8_t mt = 0, fmt = 0;
		int contiguous;

		for (i = 0; i < 3; i++) {
			(void)vms_frame_classify(e.fake.frame[i].b,
						 e.fake.frame[i].len, &fi);
			(void)vms_scs_seq(e.fake.frame[i].b,
					  e.fake.frame[i].len, &fi, NULL,
					  &seq[i]);
			(void)vms_scs_msgtype(e.fake.frame[i].b,
					      e.fake.frame[i].len, &fi, &mt,
					      &fmt);
		}
		contiguous = (seq[0] == 1u && seq[1] == 2u && seq[2] == 3u);
		ct_check(contiguous,
			 "the three frames consume contiguous sequences 1,2,3");

		/* And the shared VC decoder -- the one every other pe test
		 * reads emitted frames through -- agrees about the last one. */
		{
			struct fake_vc_decoded d = fake_vc_last(&e.fake,
							        FAKE_VC_SEQ);

			ct_check(d.ok,
				 "the shared decoder reads it as a sequenced SCS frame");
			ct_check_eq_u32(d.send_seq, 3u,
					"...with the last chunk's sequence");
			ct_check_eq_u32(d.recv_ack, the_vc(&e)->recv_seq,
					"...and this circuit's real cumulative ack");
		}
		ct_check_eq_u32(mt, VMS_SCS_MT_MSG,
				"msgtype is the sequenced-application marker (spec 4(k))");
		ct_check_eq_u32(the_vc(&e)->send_seq, 4u,
				"the circuit's next sequence advanced past them");
	}

	/* SS8d: block frames are transmitted OUTSIDE the unacked ring, and the
	 * count of that is a measurement, not a belief. */
	ct_check_eq_u32(the_vc(&e)->unacked, 0,
			"no ring entry was taken (FC-P1.2's exclusion)");
	ct_check_eq_u32(e.fsm.blk_tx_unringed, 3,
			"and the un-ringed frames are counted, not hidden");
	ct_check_eq_u32(the_vc(&e)->blk_bytes_tx, 1536u,
			"the byte counter matches what left");
}

/* ------------------------------------------------------------------ *
 * 3. A block frame FAILS the SCS envelope conformance test
 * ------------------------------------------------------------------ */
static void test_block_frame_fails_scs_envelope_conformance(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	vms_wire_view_t v;

	printf("-- a block frame fails the SCS envelope conformance test\n");
	open_circuit(&e);
	xfer_init(&x, e.vol_name, 0u, 512u, 512u);
	(void)pe_blk_send(&e.fsm, &x, 0u, NULL);
	ct_check_eq_u32(e.fake.n_frames, 1, "one frame went out");

	vms_wire_view_init(&v, e.fake.frame[0].b, e.fake.frame[0].len);
	ct_check(vms_wire_get_le16(&v, VMS_OFF_SCSCTRL_FMTWORD) !=
		 VMS_SCSCTRL_FMTWORD_CONST,
		 "abs 58 is NOT the SCS format word (it is the conid's high half)");
	ct_check(vms_blk_frame_structural_ok(e.fake.frame[0].b,
					     e.fake.frame[0].len),
		 "and the codec's structural precondition agrees");
}

/* ------------------------------------------------------------------ *
 * 4. READ's end-message piggyback: the five recorded SCA content lengths
 * ------------------------------------------------------------------ */

/* Build the abs-56-onward body of a REAL MSCP READ end message: SCS's own
 * 56-71 envelope (spec sec 4(d)/(1b)) followed by FC-P6.2's 32-byte READ end
 * body. Returns the body length (48). */
static uint32_t build_read_end_body(uint8_t *body, uint32_t bytes)
{
	uint8_t endframe[VMS_MSCP_END_FRAME_LEN(VMS_MSCP_READ_END_LEN)];
	struct vms_mscp_xfer_end me;
	vms_wire_buf_t w;
	uint32_t written = 0;

	memset(endframe, 0, sizeof(endframe));
	memset(&me, 0, sizeof(me));
	me.eh.hdr.cmd_ref = 0x00c0ffeeu;
	me.eh.hdr.unit = 0u;
	me.eh.hdr.opcode = VMS_MSCP_OP_READ;
	me.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
				       VMS_MSCP_SUB_NORMAL);
	me.byte_count = bytes;
	if (vms_mscp_read_end_build(&me, endframe, sizeof(endframe),
				    &written) != VMS_CODEC_OK)
		return 0;

	/* SCS's own 56-71 span, the layer that owns it (E1). */
	vms_wire_buf_init(&w, endframe, sizeof(endframe));
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_INNERLEN,
			  (uint16_t)(VMS_MSCP_END_SCA_LEN(
					VMS_MSCP_READ_END_LEN) - 44u));
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_FMTWORD,
			  VMS_SCSCTRL_FMTWORD_CONST);
	vms_wire_put_le16(&w, VMS_OFF_SCS_CTRL_TYPE, 10u);   /* APPL_MSG */
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_REMOTE, MSCP_DEST_CONID);
	vms_wire_put_le32(&w, VMS_OFF_SCS_CONID_LOCAL, 0x33580008u);
	if (!vms_wire_buf_ok(&w))
		return 0;

	memcpy(body, endframe + PE_SEND_BODY_OFF,
	       sizeof(endframe) - PE_SEND_BODY_OFF);
	return (uint32_t)(sizeof(endframe) - PE_SEND_BODY_OFF);
}

static void test_read_end_piggyback_matches_recorded_sca_lengths(void)
{
	/* The five READ-END SCA contents MEASURED in the vms291 capture and
	 * recorded in docs/design-mscp-direction.md, with the tail length each
	 * one implies. */
	static const uint16_t recorded_sca[5] = { 118u, 194u, 448u, 630u, 1142u };
	static const uint32_t tail[5]         = { 0u, 76u, 330u, 512u, 1024u };
	uint32_t k;

	printf("-- READ end piggyback reproduces all five recorded SCA contents\n");

	for (k = 0; k < 5; k++) {
		struct blk_env e;
		struct pe_blk_xfer x;
		uint8_t body[PE_VC_FRAME_MAX];
		uint32_t body_len;
		struct vms_frame_info fi;
		struct vms_blk_view trailer;
		uint32_t end_frame_len;
		int rc;

		open_circuit(&e);
		body_len = build_read_end_body(body, tail[k]);
		ct_check_eq_u32(body_len, 48u,
				"the MSCP READ end body-level span is 48 bytes (16 SCS + 32 MSCP)");
		end_frame_len = PE_SEND_BODY_OFF + body_len;

		xfer_init(&x, e.vol_name, 0u, tail[k] == 0u ? 512u : tail[k],
			  512u);
		if (tail[k] != 0u) {
			/* Everything before the tail is streamed first, so the
			 * piggyback really is the transfer's FINAL chunk. */
			x.length = tail[k];
		}
		rc = pe_blk_send_read_end(&e.fsm, &x, tail[k], body, body_len);
		ct_check_eq_u32((unsigned long)rc, PE_BLK_OK,
				"pe_blk_send_read_end returns OK");
		ct_check_eq_u32(e.fake.n_frames, 1,
				"exactly one Ethernet frame carried both messages");

		ct_check_eq_u32((unsigned long)vms_frame_classify(
					e.fake.frame[0].b, e.fake.frame[0].len,
					&fi), VMS_CODEC_OK,
				"the combined frame classifies");
		ct_check_eq_u32(fi.sca_content, recorded_sca[k],
				"its SCA content equals the RECORDED vms291 value");
		ct_check_eq_u32(e.fake.frame[0].len,
				(unsigned long)(VMS_ETH_HDR_LEN +
						recorded_sca[k]),
				"and the wire length is 14 + that content");

		/* TRAP 1's receive side: the trailer is recoverable ONLY with
		 * the frame's REAL length. */
		ct_check_eq_u32((unsigned long)vms_blk_trailer_parse(
					e.fake.frame[0].b, e.fake.frame[0].len,
					end_frame_len, &trailer),
				VMS_CODEC_OK, "the trailer parses off the real length");
		ct_check_eq_u32(trailer.data_len, tail[k],
				"the piggybacked tail is exactly the final chunk");
		ct_check_eq_u32(trailer.hdr.bytes_remaining, tail[k],
				"its bytes_remaining is the tail, the transfer's last");
		if (tail[k] != 0u)
			ct_check(memcmp(trailer.data, e.vol, tail[k]) == 0,
				 "and it carries the buffer's real bytes");

		/* A receiver that bounded the frame by the INNER message's own
		 * declared length sees nothing -- which is TRAP 1 exactly. */
		{
			struct vms_blk_view none;

			ct_check_eq_u32((unsigned long)vms_blk_trailer_parse(
						e.fake.frame[0].b,
						end_frame_len, end_frame_len,
						&none),
					VMS_CODEC_OK,
					"parsing with the DECLARED bound is not an error");
			ct_check_eq_u32(none.data_len, 0u,
					"...it just silently finds no trailer (TRAP 1)");
		}

		/* The MSCP end message itself is untouched by the trailer. */
		{
			struct vms_mscp_xfer_end back;

			ct_check_eq_u32((unsigned long)vms_mscp_read_end_parse(
						e.fake.frame[0].b,
						e.fake.frame[0].len, &back),
					VMS_CODEC_OK,
					"the READ end message still parses");
			ct_check_eq_u32(back.byte_count, tail[k],
					"its P.BCNT survived the piggyback");
			ct_check_eq_u32(back.eh.hdr.cmd_ref, 0x00c0ffeeu,
					"and so did P.CRF");
		}
	}
}

/* ------------------------------------------------------------------ *
 * 5. WRITE = REQUEST DATA (design SS3.2.6, E41), both ends of it
 *
 * The recorded vms291 WRITE pair is "two byte-identical 28-byte headers; only
 * the presence of data distinguishes them". E41 names the two halves: the
 * header-only one is the SERVER's REQUEST DATA (pe_blk_send_request), and the
 * data-bearing one is the answer the host's PORT sends by itself
 * (pe_blk_rx_try's responder arm). These tests drive both.
 * ------------------------------------------------------------------ */

/* The peer's REQUEST DATA: header-only, naming OUR buffer as the SOURCE and its
 * own as the destination. `obs4`/`obs6` are whatever the requester put in the
 * two ungrounded words -- opaque to us and echoed, never interpreted. */
static uint32_t rx_request_data(struct blk_env *e, uint32_t src_name,
				uint32_t src_offset, uint32_t count,
				uint16_t obs4, uint16_t obs6)
{
	struct vms_blk_hdr h;
	uint8_t dst_lavc[6];
	uint32_t len;

	memset(&h, 0, sizeof(h));
	h.dest_conid = MSCP_DEST_CONID;
	h.obs_w4 = obs4;
	h.obs_w6 = obs6;
	h.bytes_remaining = count;
	h.src_name = src_name;          /* OURS: the buffer being asked for  */
	h.src_offset = src_offset;
	h.dst_name = PEER_BUF_NAME;     /* the requester's own landing zone  */
	h.dst_offset = 0u;

	ovmx_lavc(dst_lavc);
	len = fake_peer_block(&e->peer, ovmx_hw, dst_lavc,
			      (uint16_t)(the_vc(e)->recv_seq + 1u), &h, NULL,
			      0u, e->buf, sizeof(e->buf));
	if (len != 0)
		memcpy(e->reqhdr, e->buf + VMS_BLK_HDR_OFF, VMS_BLK_HDR_LEN);
	return len;
}

/*
 * THE PORT ACKS EVERY SEQUENCED FRAME IT TAKES, before anything else happens to
 * it (SS3b(a), vc_deliver runs after vc_send_ack_frame). So the frames a
 * REQUEST DATA provokes are frame[1..]: frame[0] is that ack, and a request the
 * port refuses leaves exactly it and nothing else. These two helpers name that
 * rather than hiding it behind an index.
 */
#define REQ_ACK_FRAMES 1u

static uint32_t answers(const struct blk_env *e)
{
	return e->fake.n_frames > REQ_ACK_FRAMES
	       ? e->fake.n_frames - REQ_ACK_FRAMES : 0u;
}

static const struct fake_pe_frame *answer(const struct blk_env *e, uint32_t i)
{
	return &e->fake.frame[REQ_ACK_FRAMES + i];
}

static void test_request_data_is_answered_byte_exact(void)
{
	struct blk_env e;
	uint8_t want[VMS_BLK_HDR_LEN];
	const uint8_t *ans;
	uint32_t len;
	uint32_t i;
	int diff = 0;

	printf("-- REQUEST DATA: the port answers it ITSELF, byte-exact on the "
	       "recorded WRITE pair\n");
	open_circuit(&e);

	/* The peer asks for 512 bytes of the buffer we registered as a SOURCE. */
	len = rx_request_data(&e, e.vol_name, 0u, 512u, 0x0009u, 0x0002u);
	ct_check(len == VMS_BLK_DATA_OFF,
		 "the request is header-only -- 28 bytes and no data");
	rx_frame(&e, len);

	/* ONE answer frame, and it is the data half. */
	ct_check_eq_u32(answers(&e), 1u,
			"the port answered with exactly one frame");
	ct_check_eq_u32(answer(&e, 0)->len, VMS_BLK_DATA_OFF + 512u,
			"...header + the requested 512 bytes");
	ans = answer(&e, 0)->b + VMS_BLK_HDR_OFF;

	/*
	 * (a) THE RECORDED SHAPE: the two 28-byte headers are byte-identical,
	 * and only the presence of data tells them apart.
	 */
	ct_check(memcmp(ans, e.reqhdr, VMS_BLK_HDR_LEN) == 0,
		 "the answer's 28 bytes are BYTE-IDENTICAL to the request's "
		 "(docs/design-mscp-direction.md's recorded WRITE pair)");

	/*
	 * (b) AND FIELD BY FIELD, laid out little-endian from values that are
	 * REAL on this node -- the connection id and the peer's buffer name it
	 * sent us, the two opaque words it sent us, and the name OUR OWN port
	 * minted. Same discipline as the golden in test 1: a field reorder, a
	 * width slip or an endianness slip reds this, and no captured node's
	 * private value is baked in.
	 */
	memset(want, 0, sizeof(want));
	want[0]  = (uint8_t)(MSCP_DEST_CONID      );
	want[1]  = (uint8_t)(MSCP_DEST_CONID >>  8);
	want[2]  = (uint8_t)(MSCP_DEST_CONID >> 16);
	want[3]  = (uint8_t)(MSCP_DEST_CONID >> 24);
	want[4]  = 0x09; want[5] = 0x00;            /* +4  echoed verbatim */
	want[6]  = 0x02; want[7] = 0x00;            /* +6  echoed verbatim */
	want[8]  = 0x00; want[9] = 0x02;            /* +8  512, counting down */
	want[12] = (uint8_t)(e.vol_name      );     /* +12 OUR source name */
	want[13] = (uint8_t)(e.vol_name >>  8);
	want[14] = (uint8_t)(e.vol_name >> 16);
	want[15] = (uint8_t)(e.vol_name >> 24);
	                                            /* +16 dst offset 0    */
	want[20] = (uint8_t)(PEER_BUF_NAME      );  /* +20 the peer's name */
	want[21] = (uint8_t)(PEER_BUF_NAME >>  8);
	want[22] = (uint8_t)(PEER_BUF_NAME >> 16);
	want[23] = (uint8_t)(PEER_BUF_NAME >> 24);
	                                            /* +24 src offset 0    */
	for (i = 0; i < VMS_BLK_HDR_LEN; i++) {
		if (ans[i] != want[i]) {
			printf("     byte +%u: got 0x%02x want 0x%02x\n",
			       i, ans[i], want[i]);
			diff = 1;
		}
	}
	ct_check(!diff, "every one of the 28 bytes is the field table's");

	/* (c) The DATA is the named buffer's real bytes. This is the whole
	 * point of the service and the only thing that makes a WRITE real. */
	ct_check(memcmp(answer(&e, 0)->b + VMS_BLK_DATA_OFF, e.vol, 512u) == 0,
		 "the payload is the registered buffer's REAL bytes");

	/* (d) NO SYSAP WAS INVOLVED -- E41's own words. */
	ct_check_eq_u32(e.upper_rec.blocks, 0u,
			"no block_data upcall: the PORT answered, not a SYSAP");
	ct_check_eq_u32(e.upper_rec.messages, 0u,
			"and it was not delivered as an SCS message either");

	/* (e) The counters are the audit trail of an automatic behaviour. */
	ct_check_eq_u32(e.fsm.blk_req_rx, 1u, "one request was ours to answer");
	ct_check_eq_u32(e.fsm.blk_req_answered, 1u, "and it was answered whole");
	ct_check_eq_u32(e.fsm.blk_req_unknown_buffer, 0u, "none was unknown");
	ct_check_eq_u32(e.fsm.blk_req_refused, 0u, "none was refused");
	ct_check_eq_u32(e.fsm.blk_rx_unnamed, 0u,
			"and it was NOT mistaken for a delivery naming nothing");

	/* (f) It is a sequenced frame like every other of this class. */
	ct_check_eq_u32(the_vc(&e)->blk_tx, 1u, "counted as a block frame sent");
	ct_check_eq_u32(the_vc(&e)->blk_bytes_tx, 512u, "with its real bytes");
}

static void test_request_data_chunks_and_counts_down(void)
{
	struct blk_env e;
	uint32_t total = 3000u;
	uint32_t done = 0u;
	uint32_t i;

	printf("-- REQUEST DATA: a span past one frame uses READ's chunking, "
	       "+8 counting down\n");
	open_circuit(&e);
	rx_frame(&e, rx_request_data(&e, e.vol_name, 256u, total, 0x000du,
				     0x0007u));

	ct_check(answers(&e) > 1u,
		 "3000 bytes did not fit one frame, so it was chunked");
	for (i = 0; i < answers(&e); i++) {
		struct vms_blk_view view;
		struct vms_frame_info fi;
		const uint8_t *b = answer(&e, i)->b;
		uint32_t flen = answer(&e, i)->len;
		uint32_t n;

		ct_check_eq_u32((unsigned long)vms_frame_classify(b, flen, &fi),
				VMS_CODEC_OK, "the answer frame classifies");
		ct_check_eq_u32((unsigned long)vms_blk_frame_parse(b, flen, &fi,
								   &view),
				VMS_CODEC_OK, "and parses as a block transfer");
		n = view.data_len;
		ct_check_eq_u32(view.hdr.bytes_remaining, total - done,
				"+8 counts DOWN over the whole answer");
		ct_check_eq_u32(view.hdr.src_offset, 256u + done,
				"the SOURCE offset advances from the one asked for");
		ct_check_eq_u32(view.hdr.dst_offset, done,
				"the DESTINATION offset advances in step");
		ct_check_eq_u32(view.hdr.obs_w4, 0x000du,
				"+4 stays the requester's own value on every frame");
		ct_check_eq_u32(view.hdr.obs_w6, 0x0007u, "+6 likewise");
		ct_check(memcmp(view.data, e.vol + 256u + done, n) == 0,
			 "and each chunk is the buffer's real bytes");
		done += n;
	}
	ct_check_eq_u32(done, total, "every requested byte went out");
	ct_check_eq_u32(e.fsm.blk_req_answered, 1u,
			"one request, answered once");
}

static void test_request_data_unknown_buffer_is_dropped(void)
{
	struct blk_env e;

	printf("-- REQUEST DATA: a source buffer this port never minted is "
	       "DROPPED and counted\n");
	open_circuit(&e);
	rx_frame(&e, rx_request_data(&e, 0xdeadbeefu, 0u, 512u, 0u, 0u));

	ct_check_eq_u32(e.fsm.blk_req_unknown_buffer, 1u,
			"the unknown source name is COUNTED");
	ct_check_eq_u32(answers(&e), 0u,
			"and NOTHING was transmitted -- never answered out of "
			"some other buffer");
	ct_check_eq_u32(e.fsm.blk_req_rx, 0u, "it was never ours to answer");
	ct_check_eq_u32(e.fsm.blk_req_answered, 0u, "nothing was answered");
	ct_check_eq_u32(e.upper_rec.blocks, 0u, "no upcall was made");
}

static void test_request_data_refusals_are_whole(void)
{
	struct blk_env e;

	printf("-- REQUEST DATA: a non-source buffer, and a span outside one, "
	       "are refused WHOLE\n");

	/* (a) The buffer is ours, but it is a DESTINATION. A peer that asks us
	 * to read out of a buffer we registered to receive into gets nothing. */
	open_circuit(&e);
	rx_frame(&e, rx_request_data(&e, e.sink_name, 0u, 512u, 0u, 0u));
	ct_check_eq_u32(e.fsm.blk_req_rx, 1u, "the request WAS ours");
	ct_check_eq_u32(e.fsm.blk_req_refused, 1u, "...and was refused");
	ct_check_eq_u32(answers(&e), 0u, "nothing was transmitted");

	/* (b) A span that runs past the end of the buffer. Refused WHOLE --
	 * never clamped to what fits, because a short answer under a full byte
	 * count is a transfer the requester would complete as if it were all
	 * there. */
	open_circuit(&e);
	rx_frame(&e, rx_request_data(&e, e.vol_name, VOL_BYTES - 16u, 512u, 0u,
				     0u));
	ct_check_eq_u32(e.fsm.blk_req_refused, 1u,
			"an out-of-range span is refused");
	ct_check_eq_u32(answers(&e), 0u,
			"and not one partial frame went out");

	/* (c) A request for zero bytes asks for nothing and is answered with
	 * nothing -- refused, not answered with an empty frame that would look
	 * like a second request to the peer. */
	open_circuit(&e);
	rx_frame(&e, rx_request_data(&e, e.vol_name, 0u, 0u, 0u, 0u));
	ct_check_eq_u32(e.fsm.blk_req_refused, 1u, "a zero-byte request is refused");
	ct_check_eq_u32(answers(&e), 0u, "and silent");
}

/* The SEND side: what an MSCP SERVER emits to start a WRITE. */
static void test_send_request_names_our_destination(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	struct vms_blk_hdr h;
	int rc;

	printf("-- pe_blk_send_request: the SERVER's half, roles swapped\n");
	open_circuit(&e);

	/* Our sink is the DESTINATION; the peer's named buffer is the SOURCE. */
	xfer_init(&x, e.sink_name, 128u, 512u, 0u);
	rc = pe_blk_send_request(&e.fsm, &x);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK,
			"the request went out");
	ct_check_eq_u32(e.fake.n_frames, 1u, "one frame");
	ct_check_eq_u32(e.fake.frame[0].len, VMS_BLK_DATA_OFF,
			"header-only -- 28 bytes and no data");

	ct_check_eq_u32((unsigned long)vms_blk_hdr_parse(e.fake.frame[0].b,
							 e.fake.frame[0].len,
							 &h),
			VMS_CODEC_OK, "it parses");
	ct_check_eq_u32(h.src_name, PEER_BUF_NAME,
			"the SOURCE is the PEER's buffer, from its own message");
	ct_check_eq_u32(h.dst_name, e.sink_name,
			"the DESTINATION is OURS, and our port minted the name");
	ct_check_eq_u32(h.dst_offset, 128u, "at the offset we asked for");
	ct_check_eq_u32(h.bytes_remaining, 512u,
			"+8 is the count being requested");
	ct_check_eq_u32(h.dest_conid, MSCP_DEST_CONID,
			"and the connection id is the caller's, not invented");

	/* The INV-6 refusals are the same ones pe_blk_send makes, and the
	 * access check is the mirrored one: our buffer must be writable. */
	xfer_init(&x, e.vol_name, 0u, 512u, 0u);   /* SRC-only buffer */
	ct_check(pe_blk_send_request(&e.fsm, &x) == PE_BLK_PERM,
		 "requesting INTO a send-only buffer is PERM");
	xfer_init(&x, e.sink_name, 0u, 512u, 0u);
	x.remote_name = 0u;
	ct_check(pe_blk_send_request(&e.fsm, &x) == PE_BLK_NONAME,
		 "no peer buffer name => refused, never a zero on the wire");
	xfer_init(&x, e.sink_name, 0u, VOL_BYTES + 1u, 0u);
	ct_check(pe_blk_send_request(&e.fsm, &x) == PE_BLK_RANGE,
		 "asking for more than our own buffer holds is RANGE");
}

/*
 * THE WHOLE PAIR, in one place: this port issues the request and this port
 * answers it, so the assertion is that the 28 bytes that go out and the 28
 * bytes that come back are the same 28 bytes -- the recorded shape, produced by
 * the shipping code at both ends.
 */
static void test_write_pair_round_trips_through_the_port(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	uint8_t reqhdr[VMS_BLK_HDR_LEN];
	struct vms_blk_hdr h;
	uint32_t len;
	uint8_t dst_lavc[6];

	printf("-- the WRITE pair round-trips: our request, answered by a port "
	       "that used the same code\n");
	open_circuit(&e);

	/* 1. The SERVER half: ask for 512 bytes into our sink. */
	xfer_init(&x, e.sink_name, 0u, 512u, 0u);
	ct_check_eq_u32((unsigned long)pe_blk_send_request(&e.fsm, &x),
			PE_BLK_OK, "the request was emitted");
	memcpy(reqhdr, e.fake.frame[0].b + VMS_BLK_HDR_OFF, VMS_BLK_HDR_LEN);

	/* 2. Turn it round: the peer is now asking US for the same span out of
	 * our SOURCE buffer, with the two names in the roles the far side would
	 * see them in. Every field comes from the header we just emitted. */
	ct_check_eq_u32((unsigned long)vms_blk_hdr_parse(e.fake.frame[0].b,
							 e.fake.frame[0].len,
							 &h),
			VMS_CODEC_OK, "the emitted request parses");
	h.src_name = e.vol_name;      /* what OUR port would be asked for   */
	h.dst_name = PEER_BUF_NAME;
	fake_pe_clear_frames(&e.fake);

	ovmx_lavc(dst_lavc);
	len = fake_peer_block(&e.peer, ovmx_hw, dst_lavc,
			      (uint16_t)(the_vc(&e)->recv_seq + 1u), &h, NULL,
			      0u, e.buf, sizeof(e.buf));
	memcpy(reqhdr, e.buf + VMS_BLK_HDR_OFF, VMS_BLK_HDR_LEN);
	rx_frame(&e, len);

	/* 3. The answer. */
	ct_check_eq_u32(answers(&e), 1u, "the port answered");
	ct_check_eq_u32(answer(&e, 0)->len, VMS_BLK_DATA_OFF + 512u,
			"with the data half of the pair");
	ct_check(memcmp(answer(&e, 0)->b + VMS_BLK_HDR_OFF, reqhdr,
			VMS_BLK_HDR_LEN) == 0,
		 "and its 28 bytes are byte-identical to the request's");
	ct_check(memcmp(answer(&e, 0)->b + VMS_BLK_DATA_OFF, e.vol, 512u) == 0,
		 "carrying the source buffer's real bytes");
}

/* ------------------------------------------------------------------ *
 * 6. The two ungrounded words: honest zero, or a value we observed
 * ------------------------------------------------------------------ */

static void test_ungrounded_words_are_zero_or_observed(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	struct vms_blk_hdr h, back;
	uint8_t payload[256];
	uint8_t dst_lavc[6];
	uint32_t len, i;

	printf("-- the +4/+6 words: an explicit counted zero, or a value observed\n");
	open_circuit(&e);

	/* (a) Nothing observed yet: an explicit ZERO, and it is COUNTED. */
	xfer_init(&x, e.vol_name, 0u, 512u, 512u);
	ct_check_eq_u32((unsigned long)pe_blk_send(&e.fsm, &x, 0u, NULL),
			PE_BLK_OK, "a transfer with no observed pair still goes");
	ct_check_eq_u32((unsigned long)vms_blk_hdr_parse(e.fake.frame[0].b,
							 e.fake.frame[0].len,
							 &back),
			VMS_CODEC_OK, "the emitted header parses");
	ct_check_eq_u32(back.obs_w4, 0u, "+4 is an explicit zero, never a 9 or 13");
	ct_check_eq_u32(back.obs_w6, 0u, "+6 is an explicit zero");
	ct_check_eq_u32(e.fsm.blk_obs_absent, 1u,
			"and the honest absence is COUNTED, not hidden");
	ct_check_eq_u32((unsigned long)the_vc(&e)->obs_valid, 0u,
			"the circuit still claims no observed pair");

	/* (b) A real frame arrives carrying a pair. The port LEARNS it. */
	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(0xb0u + i);
	memset(&h, 0, sizeof(h));
	h.dest_conid = MSCP_DEST_CONID;
	h.obs_w4 = 0x000du;      /* whatever the peer put there -- opaque */
	h.obs_w6 = 0x0007u;
	h.bytes_remaining = sizeof(payload);
	h.src_name = PEER_BUF_NAME;
	h.src_offset = 0u;
	h.dst_name = e.sink_name;
	h.dst_offset = 64u;

	ovmx_lavc(dst_lavc);
	len = fake_peer_block(&e.peer, ovmx_hw, dst_lavc, 1u, &h, payload,
			      sizeof(payload), e.buf, sizeof(e.buf));
	ct_check(len != 0, "the peer's block-transfer frame was built");
	fake_pe_clear_frames(&e.fake);
	rx_frame(&e, len);

	ct_check_eq_u32((unsigned long)the_vc(&e)->obs_valid, 1u,
			"the circuit now HAS an observed pair");
	ct_check_eq_u32(the_vc(&e)->obs_w4, 0x000du, "+4 learned verbatim");
	ct_check_eq_u32(the_vc(&e)->obs_w6, 0x0007u, "+6 learned verbatim");

	/* (c) The next frame this port emits carries exactly those bytes, and
	 * the honest-absence counter does NOT advance. */
	{
		uint32_t before = e.fsm.blk_obs_absent;
		uint32_t idx;

		fake_pe_clear_frames(&e.fake);
		ct_check_eq_u32((unsigned long)pe_blk_send(&e.fsm, &x, 0u,
							   NULL),
				PE_BLK_OK, "a second transfer goes out");
		idx = e.fake.n_frames - 1u;
		ct_check_eq_u32((unsigned long)vms_blk_hdr_parse(
					e.fake.frame[idx].b,
					e.fake.frame[idx].len, &back),
				VMS_CODEC_OK, "it parses");
		ct_check_eq_u32(back.obs_w4, 0x000du,
				"+4 is the OBSERVED value, carried through unchanged");
		ct_check_eq_u32(back.obs_w6, 0x0007u,
				"+6 likewise");
		ct_check_eq_u32(e.fsm.blk_obs_absent, before,
				"and no further honest-absence was counted");
	}
}

/* ------------------------------------------------------------------ *
 * 7. Receive: named-buffer landing, bounds, and the discriminator
 * ------------------------------------------------------------------ */
static void test_receive_lands_in_the_named_buffer(void)
{
	struct blk_env e;
	struct vms_blk_hdr h;
	uint8_t payload[512];
	uint8_t dst_lavc[6];
	uint32_t len, i;

	printf("-- receive: the bytes land in the NAMED buffer, bounds-checked\n");
	open_circuit(&e);
	for (i = 0; i < sizeof(payload); i++)
		payload[i] = (uint8_t)(0x5au ^ i);

	memset(&h, 0, sizeof(h));
	h.dest_conid = MSCP_DEST_CONID;
	h.bytes_remaining = sizeof(payload);
	h.src_name = PEER_BUF_NAME;
	h.dst_name = e.sink_name;
	h.dst_offset = 1024u;

	ovmx_lavc(dst_lavc);
	len = fake_peer_block(&e.peer, ovmx_hw, dst_lavc, 1u, &h, payload,
			      sizeof(payload), e.buf, sizeof(e.buf));
	rx_frame(&e, len);

	ct_check_eq_u32(the_vc(&e)->blk_rx, 1u, "one block frame was taken");
	ct_check(memcmp(e.sink + 1024u, payload, sizeof(payload)) == 0,
		 "the bytes are in the buffer at the offset the header named");
	ct_check_eq_u32(e.upper_rec.blocks, 1u, "the upper layer was told");
	ct_check_eq_u32(e.upper_rec.last_name, e.sink_name,
			"...by OUR buffer name");
	ct_check_eq_u32(e.upper_rec.last_offset, 1024u, "...the real offset");
	ct_check_eq_u32(e.upper_rec.last_len, sizeof(payload),
			"...and what actually landed");
	ct_check_eq_u32(e.upper_rec.last_remaining, sizeof(payload),
			"...with the transfer's own down-counter");
	ct_check_eq_u32(e.upper_rec.messages, 0u,
			"and it was NOT also delivered as an SCS message");
	ct_check_eq_u32(e.fsm.blk_rx_range, 0u, "nothing was out of range");
}

static void test_receive_out_of_range_is_dropped_not_clamped(void)
{
	struct blk_env e;
	struct vms_blk_hdr h;
	uint8_t payload[512];
	uint8_t dst_lavc[6];
	uint32_t len;

	printf("-- receive: an offset outside the buffer is DROPPED, never clamped\n");
	open_circuit(&e);
	memset(payload, 0xa7, sizeof(payload));

	memset(&h, 0, sizeof(h));
	h.dest_conid = MSCP_DEST_CONID;
	h.bytes_remaining = sizeof(payload);
	h.src_name = PEER_BUF_NAME;
	h.dst_name = e.sink_name;
	h.dst_offset = VOL_BYTES - 16u;    /* 512 bytes will not fit */

	ovmx_lavc(dst_lavc);
	len = fake_peer_block(&e.peer, ovmx_hw, dst_lavc, 1u, &h, payload,
			      sizeof(payload), e.buf, sizeof(e.buf));
	rx_frame(&e, len);

	ct_check_eq_u32(e.fsm.blk_rx_range, 1u, "the frame was counted as out of range");
	ct_check_eq_u32(the_vc(&e)->blk_rx, 0u, "it was NOT taken");
	ct_check_eq_u32(e.upper_rec.blocks, 0u, "the upper layer was not told");
	ct_check_eq_u32(e.sink[VOL_BYTES - 16u], 0u,
			"and not one byte was written -- no partial apply");
}

static void test_receive_unnamed_buffer_falls_through(void)
{
	struct blk_env e;
	struct vms_blk_hdr h;
	uint8_t payload[256];
	uint8_t dst_lavc[6];
	uint32_t len;

	printf("-- receive: a frame naming no buffer of ours is not stolen\n");
	open_circuit(&e);
	memset(payload, 0x11, sizeof(payload));

	memset(&h, 0, sizeof(h));
	h.dest_conid = MSCP_DEST_CONID;
	h.bytes_remaining = sizeof(payload);
	h.src_name = PEER_BUF_NAME;
	h.dst_name = 0xdeadbeefu;   /* a name this port never minted */
	h.dst_offset = 0u;

	ovmx_lavc(dst_lavc);
	len = fake_peer_block(&e.peer, ovmx_hw, dst_lavc, 1u, &h, payload,
			      sizeof(payload), e.buf, sizeof(e.buf));
	rx_frame(&e, len);

	ct_check_eq_u32(e.fsm.blk_rx_unnamed, 1u,
			"it was counted as naming nothing of ours");
	ct_check_eq_u32(the_vc(&e)->blk_rx, 0u, "and NOT taken as a block transfer");
	ct_check_eq_u32(e.upper_rec.blocks, 0u, "no completion was reported");
}

static void test_ordinary_message_is_untouched(void)
{
	struct blk_env e;
	vms_wire_buf_t w;
	uint8_t dst_lavc[6];
	uint32_t len;

	printf("-- an ordinary 190-content SCS message still reaches the upper layer\n");

	/* (a) The CONFORMANT case: spec sec 4(h)(1b) grounds the 190-content
	 * class as carrying the constant format word 0x0004 at abs 58, so the
	 * block arm's structural precondition rejects it outright and the
	 * message never even reaches the name lookup. */
	open_circuit(&e);
	ovmx_lavc(dst_lavc);
	len = fake_peer_seqmsg(&e.peer, ovmx_hw, dst_lavc, 1, 0,
			       0x62c50009u, 0x33580008u, e.buf, sizeof(e.buf));
	vms_wire_buf_init(&w, e.buf, len);
	vms_wire_put_le16(&w, VMS_OFF_SCSCTRL_FMTWORD, VMS_SCSCTRL_FMTWORD_CONST);
	ct_check(vms_wire_buf_ok(&w), "the fixture carries the grounded 0x0004");
	rx_frame(&e, len);

	ct_check_eq_u32(e.upper_rec.messages, 1u, "message() fired");
	ct_check_eq_u32(e.upper_rec.blocks, 0u, "block_data() did not");
	ct_check_eq_u32(e.fsm.blk_rx_unnamed, 0u,
			"the block arm did not even consider it (abs 58 is 0x0004)");

	/* (b) The case the NEGATIVE test cannot exclude -- spec sec 4(h)(1d)
	 * already names a class that fails the 0x0004 check while being
	 * something else. Here the POSITIVE discriminator earns its keep: the
	 * frame names no buffer of ours, so it is counted and passed straight
	 * on to normal delivery, unmodified. */
	open_circuit(&e);
	len = fake_peer_seqmsg(&e.peer, ovmx_hw, dst_lavc, 1, 0,
			       0x62c50009u, 0x33580008u, e.buf, sizeof(e.buf));
	rx_frame(&e, len);

	ct_check_eq_u32(e.fsm.blk_rx_unnamed, 1u,
			"a non-conformant frame IS considered, and names nothing of ours");
	ct_check_eq_u32(e.upper_rec.messages, 1u,
			"...and is still delivered as an SCS message, untouched");
	ct_check_eq_u32(e.upper_rec.blocks, 0u, "...never as a block transfer");
}

/* ------------------------------------------------------------------ *
 * 8. Named buffers, and the INV-6 refusals
 * ------------------------------------------------------------------ */
static void test_named_buffers(void)
{
	struct blk_env e;
	uint8_t mem[64];
	uint32_t names[PE_BLK_MAX_BUFFERS + 2];
	uint32_t i, n = 0;
	int rc;

	printf("-- named buffers: mint, look up, release, refuse\n");
	env_init(&e);

	rc = pe_blk_buf_register(&e.fsm, mem, sizeof(mem), PE_BLK_ACC_SRC,
				 &names[0]);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK, "register succeeds");
	ct_check(names[0] != 0u, "a live name is never 0");
	ct_check(pe_blk_buf_lookup(&e.fsm, names[0]) != NULL,
		 "and it resolves");

	rc = pe_blk_buf_release(&e.fsm, names[0]);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK, "release succeeds");
	ct_check(pe_blk_buf_lookup(&e.fsm, names[0]) == NULL,
		 "and the name stops resolving");
	ct_check(pe_blk_buf_release(&e.fsm, names[0]) == PE_BLK_NOBUF,
		 "a second release is NOBUF");

	rc = pe_blk_buf_register(&e.fsm, mem, sizeof(mem), PE_BLK_ACC_SRC,
				 &names[1]);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK, "the slot is reusable");
	ct_check(names[1] != names[0],
		 "but the NAME is not immediately reused (a stale transfer misses)");

	/* Fill the table, then prove the overflow is refused and counted. */
	memset(&e, 0, sizeof(e));
	env_init(&e);
	for (i = 0; i < PE_BLK_MAX_BUFFERS; i++) {
		if (pe_blk_buf_register(&e.fsm, mem, sizeof(mem),
					PE_BLK_ACC_SRC, &names[i]) == PE_BLK_OK)
			n++;
	}
	ct_check_eq_u32(n, PE_BLK_MAX_BUFFERS, "the table fills");
	rc = pe_blk_buf_register(&e.fsm, mem, sizeof(mem), PE_BLK_ACC_SRC,
				 &names[i]);
	ct_check(rc == PE_BLK_NOSPACE, "one more is REFUSED, never evicting");
	ct_check_eq_u32(e.fsm.blk_no_slot, 1u, "and the refusal is counted");
}

static void test_inv6_refusals(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	int rc;

	printf("-- INV-6: a peer-supplied field the SYSAP did not have is a REFUSAL\n");
	open_circuit(&e);

	xfer_init(&x, e.vol_name, 0u, 512u, 512u);
	x.remote_name = 0u;
	rc = pe_blk_send(&e.fsm, &x, 0u, NULL);
	ct_check(rc == PE_BLK_NONAME,
		 "no peer buffer name => refused, never a zero on the wire");
	ct_check_eq_u32(e.fake.n_frames, 0u, "and nothing was transmitted");

	xfer_init(&x, e.vol_name, 0u, 512u, 512u);
	x.dest_conid = 0u;
	rc = pe_blk_send(&e.fsm, &x, 0u, NULL);
	ct_check(rc == PE_BLK_NONAME,
		 "no connection ID => refused, never a zero on the wire");
	ct_check_eq_u32(e.fake.n_frames, 0u, "still nothing transmitted");

	/* Our own side is checked just as hard. */
	xfer_init(&x, e.vol_name, 0u, VOL_BYTES + 1u, 512u);
	ct_check(pe_blk_send(&e.fsm, &x, 0u, NULL) == PE_BLK_RANGE,
		 "a transfer longer than our own buffer is RANGE");

	xfer_init(&x, e.sink_name, 0u, 512u, 512u);   /* DST-only buffer */
	ct_check(pe_blk_send(&e.fsm, &x, 0u, NULL) == PE_BLK_PERM,
		 "sending FROM a receive-only buffer is PERM");

	xfer_init(&x, 0x99999999u, 0u, 512u, 512u);
	ct_check(pe_blk_send(&e.fsm, &x, 0u, NULL) == PE_BLK_NOBUF,
		 "an unknown local name is NOBUF");

	{
		struct blk_env e2;

		env_init(&e2);   /* no circuit formed */
		(void)pe_blk_buf_register(&e2.fsm, e2.vol, VOL_BYTES,
					  PE_BLK_ACC_SRC, &e2.vol_name);
		xfer_init(&x, e2.vol_name, 0u, 512u, 512u);
		ct_check(pe_blk_send(&e2.fsm, &x, 0u, NULL) ==
			 PE_BLK_NOCIRCUIT,
			 "no OPEN circuit is NOCIRCUIT");
		ct_check_eq_u32(e2.fake.n_frames, 0u, "and silent");
	}
}

/* A transmit that fails part-way must not consume a sequence for a frame that
 * never went out -- the same no-hole guarantee pe_vc_send_frame carries. */
static void test_failed_transmit_leaves_no_sequence_hole(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	uint32_t frames = 99;
	uint16_t seq_before;

	printf("-- a failed transmit consumes no sequence (no hole)\n");
	open_circuit(&e);
	seq_before = the_vc(&e)->send_seq;
	e.fake.send_fails = 1;

	xfer_init(&x, e.vol_name, 0u, 1024u, 512u);
	ct_check(pe_blk_send(&e.fsm, &x, 0u, &frames) == PE_BLK_TXFAIL,
		 "pe_blk_send reports the transmit failure");
	ct_check_eq_u32(frames, 0u, "no frame is claimed to have gone out");
	ct_check_eq_u32(the_vc(&e)->send_seq, seq_before,
			"and the circuit's sequence did not advance");
}

int main(void)
{
	test_block_header_is_byte_exact();
	test_read_stream_counts_down_and_carries_real_bytes();
	test_block_frame_fails_scs_envelope_conformance();
	test_read_end_piggyback_matches_recorded_sca_lengths();
	test_request_data_is_answered_byte_exact();
	test_request_data_chunks_and_counts_down();
	test_request_data_unknown_buffer_is_dropped();
	test_request_data_refusals_are_whole();
	test_send_request_names_our_destination();
	test_write_pair_round_trips_through_the_port();
	test_ungrounded_words_are_zero_or_observed();
	test_receive_lands_in_the_named_buffer();
	test_receive_out_of_range_is_dropped_not_clamped();
	test_receive_unnamed_buffer_falls_through();
	test_ordinary_message_is_untouched();
	test_named_buffers();
	test_inv6_refusals();
	test_failed_transmit_leaves_no_sequence_hole();
	return ct_summary("test_pe_block");
}
