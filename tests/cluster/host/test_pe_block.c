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
 * 5. WRITE's two-frame form: byte-identical headers
 * ------------------------------------------------------------------ */
static void test_write_two_frame_headers_are_byte_identical(void)
{
	struct blk_env e;
	struct pe_blk_xfer x;
	struct vms_blk_hdr req;
	uint32_t reqlen;
	uint8_t reqhdr[VMS_BLK_HDR_LEN];
	int rc;

	printf("-- WRITE two-frame: the request and response headers are byte-identical\n");
	open_circuit(&e);
	xfer_init(&x, e.vol_name, 0u, 512u, 512u);

	/* The data-bearing half. */
	rc = pe_blk_send(&e.fsm, &x, 0u, NULL);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK, "the request went out");
	reqlen = e.fake.frame[0].len;
	ct_check_eq_u32(reqlen, VMS_BLK_DATA_OFF + 512u,
			"the request is header + data");
	memcpy(reqhdr, e.fake.frame[0].b + VMS_BLK_HDR_OFF, VMS_BLK_HDR_LEN);
	ct_check_eq_u32((unsigned long)vms_blk_hdr_parse(e.fake.frame[0].b,
							 reqlen, &req),
			VMS_CODEC_OK, "and it parses");

	/* The header-only half, echoed from what was parsed off the wire. */
	rc = pe_blk_send_ack(&e.fsm, VAX1_SYSID, &req);
	ct_check_eq_u32((unsigned long)rc, PE_BLK_OK, "the response went out");
	ct_check_eq_u32(e.fake.n_frames, 2, "two frames total");
	ct_check_eq_u32(e.fake.frame[1].len, VMS_BLK_DATA_OFF,
			"the response carries NO data -- only that distinguishes it");
	ct_check(memcmp(e.fake.frame[1].b + VMS_BLK_HDR_OFF, reqhdr,
			VMS_BLK_HDR_LEN) == 0,
		 "the two 28-byte headers are BYTE-IDENTICAL (the recorded TRAP 2)");
}

/* ------------------------------------------------------------------ *
 * 6. The two ungrounded words: honest zero, or a value we observed
 * ------------------------------------------------------------------ */

/* A block-transfer frame FROM the peer, built the way the port itself builds
 * one -- through the codec, never a hand-laid array. */
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
	test_write_two_frame_headers_are_byte_identical();
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
