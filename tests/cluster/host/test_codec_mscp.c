// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_mscp.c - MSCP-over-SCS codec entries, rung R1 (FC-P6.2).
 *
 * No fixture corpus exists for this family (the vms-291 lab-2 serving
 * capture is a host-only pcap artifact, per src/vmsscs/include/
 * scs_mscp_srv.h's own "NOT in git" note) -- this is the PORT of the
 * `test_scs_mscp_srv`/`test_scs_mscp` vectors the plan item names, not a
 * fixture-file test:
 *
 *   1. THE ONE BYTE-EXACT GOLDEN VECTOR this project holds in-tree: the SCC
 *      end message body from af2-firsttimer-established-20260728.pcap,
 *      already committed (not host-only) in
 *      tests/vmsscs/test_scs_mscp_srv.c's `golden_scc_end[]`. Transcribed
 *      here (public, already-committed bytes -- not code) to prove this
 *      INDEPENDENT codec reproduces the same real VAX server answer.
 *   2. Field-level ports of that file's GUS/ONLINE/READ/WRITE assertions
 *      (lengths, endcodes, the GUS tail constant, the status major/sub
 *      split, the P.UNFL echo rule) -- round-trip build/parse, since no
 *      byte-exact fixture exists for those classes either upstream.
 *   3. The self-contained classifier's own collision case: a 94-content
 *      COMMAND and a 94-content WRITE END must resolve to different
 *      classes from the SAME length.
 *   4. The allowlist rows this item contributes validate structurally.
 */
#include "cluster_test.h"
#include "vms_cluster_codec_mscp.h"

#include <string.h>

/* ---- shared frame-buffer scaffolding ---------------------------------- */

static void mk_link(struct vms_mscp_link *l, uint16_t recv_ack,
		    uint16_t send_seq, uint16_t credit, uint32_t remote_conid,
		    uint32_t local_conid)
{
	static const uint8_t dst[6] = { 0x08, 0x00, 0x2b, 0x11, 0x22, 0x33 };
	static const uint8_t src[6] = { 0x08, 0x00, 0x2b, 0x44, 0x55, 0x66 };

	memset(l, 0, sizeof(*l));
	memcpy(l->hdr.eth_dst, dst, 6);
	memcpy(l->hdr.eth_src, src, 6);
	memcpy(l->hdr.dst_lavc, dst, 6);
	memcpy(l->hdr.src_lavc, src, 6);
	l->hdr.connect_flag = 0x0001;
	l->recv_ack = recv_ack;
	l->send_seq = send_seq;
	l->credit = credit;
	l->remote_conid = remote_conid;
	l->local_conid = local_conid;
}

/* Write the abs[0,72) link prefix for a frame that will total
 * `sca_content_len` SCA-content bytes. Returns 1 on success. */
static int build_link_prefix(uint8_t *frame, uint32_t cap,
			     uint16_t sca_content_len)
{
	struct vms_mscp_link l;
	uint32_t written = 0;
	vms_codec_status_t st;

	mk_link(&l, 0x1111, 0x2222, VMS_MSCP_ENV_CREDIT_OBSERVED, 0xaaaa5501u,
	       0xbbbb5502u);
	st = vms_mscp_link_build(&l, sca_content_len, frame, cap, &written);
	return st == VMS_CODEC_OK && written == VMS_OFF_SYSAP_BODY;
}

static struct vms_frame_info classify(const uint8_t *frame, uint32_t len)
{
	struct vms_frame_info fi;

	memset(&fi, 0, sizeof(fi));
	(void)vms_frame_classify(frame, len, &fi);
	return fi;
}

/*
 * test_measured_lengths_are_literal - pins each of the five body-length
 * constants (and the GUS tail constant) against a LITERAL number, not just
 * against itself. Every other test in this file compares a built frame's
 * length against VMS_MSCP_*_END_LEN, which only proves the builder wrote as
 * many bytes as the constant says -- a self-consistency check that would
 * stay green at any value the constant held (the exact failure mode
 * tests/vmsscs/test_scs_mscp_srv_mutants.py's own doc comment names: "a
 * test that only checks self-consistency against the constant it is
 * testing passes at any value" -- GUS 52 vs Table A-7's 48 and WRITE's 36
 * vs READ's 32 were BOTH wrong-and-green before the vms-291 capture
 * corrected them). This is this codec's own pin on the same measurement.
 */
static void test_measured_lengths_are_literal(void)
{
	printf("-- pin the five measured lengths (and the GUS tail) against "
	      "literal numbers, not merely against themselves\n");
	ct_check_eq_u32(VMS_MSCP_SCC_END_LEN, 28u, "SCC end body == 28");
	ct_check_eq_u32(VMS_MSCP_ONLINE_END_LEN, 44u, "ONLINE end body == 44");
	ct_check_eq_u32(VMS_MSCP_READ_END_LEN, 32u, "READ end body == 32");
	ct_check_eq_u32(VMS_MSCP_WRITE_END_LEN, 36u, "WRITE end body == 36");
	ct_check_eq_u32(VMS_MSCP_GUS_END_LEN, 52u,
			"GUS end body == 52, NOT Table A-7's 48");
	ct_check_eq_u32(VMS_MSCP_GUS_TAIL_OBSERVED, 0x006eu,
			"the GUS tail constant == 0x006e");
	ct_check_eq_u32(VMS_MSCP_CMD_SCA_LEN, 94u, "command SCA content == 94");
}

/* ---- 1: the byte-exact SCC end golden vector --------------------------- */

/* Transcribed from tests/vmsscs/test_scs_mscp_srv.c's golden_scc_end[] --
 * a real VAX SET CONTROLLER CHARACTERISTICS end message,
 * af2-firsttimer-established-20260728.pcap, already committed in-tree. */
static const uint8_t golden_scc_end_body[28] = {
	0x02, 0x00, 0xa3, 0x81, 0x02, 0x00, 0x00, 0x00,
	0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xa0,
	0x14, 0x00, 0x47, 0x05, 0x01, 0x04, 0x00, 0x00,
	0x00, 0x00, 0x04, 0x01,
};

static void test_scc_end_golden(void)
{
	struct vms_mscp_scc_end e;
	uint8_t frame[256];
	uint32_t written = 0, len;
	int ok;

	printf("-- SCC end message: byte-exact against the af2 golden vector\n");
	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x81a30002u; /* SCS_MSCP_CMD_REF(class=2, msgid=0x81a3) */
	e.eh.hdr.unit = 0x0002u;        /* sec 6.16: RESERVED on SCC, echoed     */
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, VMS_MSCP_SUB_NORMAL);
	e.version = 0;
	e.ctlr_flags = VMS_MSCP_SCC_CNTF_OBSERVED;
	e.ctlr_timeout = 20u;
	e.rsvd18 = VMS_MSCP_SCC_RSVD18_OBSERVED;
	e.ctlr_id = (uint64_t)0x0104000000000401ULL;

	memset(frame, 0xAA, sizeof(frame));
	ok = build_link_prefix(frame, sizeof(frame),
			       VMS_MSCP_END_SCA_LEN(VMS_MSCP_SCC_END_LEN));
	ct_check(ok, "link prefix builds");
	ok = ok && vms_mscp_scc_end_build(&e, frame, sizeof(frame), &written)
			   == VMS_CODEC_OK;
	ct_check(ok, "SCC end body builds");
	len = VMS_OFF_SYSAP_BODY + written;
	ct_check_eq_u32(len, VMS_MSCP_END_FRAME_LEN(VMS_MSCP_SCC_END_LEN),
			"total frame length is 14+58+28 = 100");

	if (ok) {
		int same = memcmp(frame + VMS_OFF_SYSAP_BODY, golden_scc_end_body,
				  sizeof(golden_scc_end_body)) == 0;
		ct_check(same, "the built SCC end body is BYTE-IDENTICAL to the "
			      "real VAX server's answer (af2-firsttimer-established)");
		if (!same) {
			uint32_t i;
			printf("    built:  ");
			for (i = 0; i < sizeof(golden_scc_end_body); i++)
				printf("%02x ", frame[VMS_OFF_SYSAP_BODY + i]);
			printf("\n    golden: ");
			for (i = 0; i < sizeof(golden_scc_end_body); i++)
				printf("%02x ", golden_scc_end_body[i]);
			printf("\n");
		}
	}

	/* Parse it back and confirm the split/derived fields. */
	{
		struct vms_mscp_scc_end p;
		vms_codec_status_t st = vms_mscp_scc_end_parse(frame, len, &p);

		ct_check(st == VMS_CODEC_OK, "parses back");
		ct_check_eq_u32(p.eh.hdr.opcode, VMS_MSCP_OP_SCC | VMS_MSCP_END_BIT,
				"endcode == 0x84 (OP.SCC|OP.END)");
		ct_check_eq_u32(p.eh.status_major, VMS_MSCP_ST_SUCCESS,
				"status_major == SUCCESS");
		ct_check_eq_u32(p.ctlr_flags, VMS_MSCP_SCC_CNTF_OBSERVED,
				"ctlr_flags round-trips as the OBSERVED 0xa004");
		ct_check_eq_u32((unsigned long)p.ctlr_id,
				(unsigned long)(uint64_t)0x0104000000000401ULL,
				"P.CNTI round-trips");
	}
}

static void test_scc_cmd_roundtrip(void)
{
	struct vms_mscp_scc_cmd c, p;
	uint8_t frame[256];
	uint32_t written = 0, len;
	struct vms_frame_info fi;
	enum vms_mscp_class cls = VMS_MSCP_CLS_UNKNOWN;

	printf("-- SCC command: build, classify, round-trip\n");
	memset(&c, 0, sizeof(c));
	c.hdr.cmd_ref = 0x81a30002u;
	c.hdr.unit = 2u;
	c.version = 0;
	c.ctlr_flags = VMS_MSCP_CF_ATTN_MSGS | VMS_MSCP_CF_MISC_ERRLOG |
		       VMS_MSCP_CF_THIS_HOST;
	c.host_timeout = 0;
	c.time = 0x0102030405060708ULL;

	ct_check(build_link_prefix(frame, sizeof(frame), VMS_MSCP_CMD_SCA_LEN),
		 "link prefix builds");
	ct_check(vms_mscp_scc_cmd_build(&c, frame, sizeof(frame), &written)
			 == VMS_CODEC_OK,
		 "SCC command body builds");
	len = VMS_OFF_SYSAP_BODY + written;
	ct_check_eq_u32(len, VMS_MSCP_CMD_FRAME_LEN, "total length is 108 (14+94)");

	fi = classify(frame, len);
	/* FC-P2.1b (spec §4(h)(1b)) grounded a dedicated CONID-capable class
	 * for the 94-content op-10 shape; a 94-content MSCP command now lands
	 * there instead of the VMS_FCLS_SCS_SEQ catch-all -- see mscp_seq_ok()
	 * in vms_cluster_codec_mscp.c, which accepts both. */
	ct_check(fi.family == VMS_FFAM_SCS && fi.cls == VMS_FCLS_SCS_APPLMSG94,
		 "the shared registry classifies it VMS_FCLS_SCS_APPLMSG94");
	ct_check((fi.caps & VMS_FCAP_CONID) != 0,
		 "and grants CONID -- the frame really does carry the Con.ID "
		 "pair MSCP already bakes at [50:58] (vms_mscp_link_build)");
	ct_check(vms_mscp_classify(frame, len, &fi, &cls) == VMS_CODEC_OK &&
			 cls == VMS_MSCP_CLS_CMD,
		 "this item's own classifier resolves VMS_MSCP_CLS_CMD");

	ct_check(vms_mscp_scc_cmd_parse(frame, len, &p) == VMS_CODEC_OK,
		 "parses back");
	ct_check_eq_u32(p.hdr.cmd_ref, c.hdr.cmd_ref, "cmd_ref round-trips");
	ct_check_eq_u32(p.ctlr_flags, c.ctlr_flags, "ctlr_flags round-trips");
	ct_check((unsigned long)p.time == (unsigned long)c.time,
		 "P.TIME round-trips through the split u64 write/read");
}

/* ---- 2: GUS / ONLINE / READ / WRITE field-level ports ------------------ */

static void test_gus_end_fields(void)
{
	struct vms_mscp_gus_end e, p;
	uint8_t frame[256];
	uint32_t written = 0, len;

	printf("-- GUS end message: measured 52-byte length + the tail constant\n");
	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x1u;
	e.eh.hdr.unit = 5u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_AVAILABLE, 0);
	e.unit_flags = VMS_MSCP_UF_WRITE_PROT_SW;
	e.unit_id = 0xdeadbeefu;
	e.media_id = 0x2452u;
	e.shadow_unit = 5u;

	ct_check(build_link_prefix(frame, sizeof(frame),
				   VMS_MSCP_END_SCA_LEN(VMS_MSCP_GUS_END_LEN)),
		 "link prefix builds");
	ct_check(vms_mscp_gus_end_build(&e, frame, sizeof(frame), &written)
			 == VMS_CODEC_OK,
		 "GUS end body builds");
	len = VMS_OFF_SYSAP_BODY + written;
	ct_check_eq_u32(len, VMS_MSCP_END_FRAME_LEN(VMS_MSCP_GUS_END_LEN),
			"a GUS end message is 52 bytes -- NOT Table A-7's 48, which "
			"no real VAX server ever emits");

	ct_check(vms_mscp_gus_end_parse(frame, len, &p) == VMS_CODEC_OK,
		 "parses back");
	ct_check_eq_u32(p.eh.hdr.opcode, VMS_MSCP_OP_GUS | VMS_MSCP_END_BIT,
			"the GUS endcode is 0x83");
	ct_check_eq_u32(p.eh.status_major, VMS_MSCP_ST_AVAILABLE,
			"status_major == AVAILABLE");

	{
		uint16_t tail = (uint16_t)(frame[VMS_OFF_MSCP_GUS_E_TAIL] |
					   (frame[VMS_OFF_MSCP_GUS_E_TAIL + 1] << 8));
		uint16_t tail2 = (uint16_t)(frame[VMS_OFF_MSCP_GUS_E_TAIL + 2] |
					    (frame[VMS_OFF_MSCP_GUS_E_TAIL + 3] << 8));

		ct_check_eq_u32(tail, VMS_MSCP_GUS_TAIL_OBSERVED,
				"body[48:50] carries the OBSERVED 0x006e");
		ct_check_eq_u32(tail2, 0, "body[50:52] is left zero, never invented");
	}
	ct_check_eq_u32(p.unit_flags, VMS_MSCP_UF_WRITE_PROT_SW,
			"UF.WPS round-trips at Table A-7 offset 14");
	ct_check((unsigned long)p.unit_id == 0xdeadbeefu,
		 "P.UNTI round-trips at Table A-7 offset 20");
	ct_check_eq_u32(p.media_id, 0x2452u, "P.MEDI round-trips at offset 28");
	ct_check_eq_u32(p.shadow_unit, 5u, "P.SHUN round-trips at offset 32");
}

static void test_online_end_and_unfl_echo(void)
{
	struct vms_mscp_online_end e, p;
	uint8_t frame[256];
	uint32_t written = 0, len;
	uint16_t composed;

	printf("-- ONLINE end message: measured 44-byte length + P.UNFL echo rule\n");

	composed = vms_mscp_online_unfl_compose(0x8000u, VMS_MSCP_UF_WRITE_PROT_SW);
	ct_check_eq_u32(composed, (0x8000u | VMS_MSCP_UF_WRITE_PROT_SW),
			"host-originated bit 15 is echoed AND the unit's own UF.WPS "
			"survives -- a host cannot clear write protection by asking");

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x100u;
	e.eh.hdr.unit = 1u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, VMS_MSCP_SUB_NORMAL);
	e.unit_flags = composed;
	e.unit_size = 4096u;
	e.volume_ser = 0xaabbccddu;

	ct_check(build_link_prefix(frame, sizeof(frame),
				   VMS_MSCP_END_SCA_LEN(VMS_MSCP_ONLINE_END_LEN)),
		 "link prefix builds");
	ct_check(vms_mscp_online_end_build(&e, frame, sizeof(frame), &written)
			 == VMS_CODEC_OK,
		 "ONLINE end body builds");
	len = VMS_OFF_SYSAP_BODY + written;
	ct_check_eq_u32(len, VMS_MSCP_END_FRAME_LEN(VMS_MSCP_ONLINE_END_LEN),
			"an ONLINE end message is 44 bytes");

	ct_check(vms_mscp_online_end_parse(frame, len, &p) == VMS_CODEC_OK,
		 "parses back");
	ct_check_eq_u32(p.eh.hdr.opcode, VMS_MSCP_OP_ONLINE | VMS_MSCP_END_BIT,
			"the ONLINE endcode is 0x89");
	ct_check_eq_u32(p.unit_flags, composed, "P.UNFL round-trips the composed word");
	ct_check_eq_u32(p.unit_size, 4096u,
			"P.UNSZ round-trips at Table A-7 offset 36");
	ct_check_eq_u32(p.volume_ser, 0xaabbccddu,
			"P.VSER round-trips at Table A-7 offset 40");
}

static void test_read_write_end_lengths(void)
{
	struct vms_mscp_xfer_end re, we, rp, wp;
	uint8_t rframe[256], wframe[256];
	uint32_t rwritten = 0, wwritten = 0, rlen, wlen;

	printf("-- READ/WRITE end messages: 32 vs 36, NOT the same length\n");

	memset(&re, 0, sizeof(re));
	re.eh.hdr.cmd_ref = 0x1feu;
	re.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0);
	re.byte_count = 512u;

	ct_check(build_link_prefix(rframe, sizeof(rframe),
				   VMS_MSCP_END_SCA_LEN(VMS_MSCP_READ_END_LEN)),
		 "link prefix builds (READ)");
	ct_check(vms_mscp_read_end_build(&re, rframe, sizeof(rframe), &rwritten)
			 == VMS_CODEC_OK,
		 "READ end body builds");
	rlen = VMS_OFF_SYSAP_BODY + rwritten;
	ct_check_eq_u32(rlen, VMS_MSCP_END_FRAME_LEN(VMS_MSCP_READ_END_LEN),
			"a READ end message is 32 bytes (Table A-7 generic end)");
	ct_check(vms_mscp_read_end_parse(rframe, rlen, &rp) == VMS_CODEC_OK,
		 "READ end parses back");
	ct_check_eq_u32(rp.eh.hdr.opcode, VMS_MSCP_OP_READ | VMS_MSCP_END_BIT,
			"the READ endcode is 0xa1");
	ct_check_eq_u32(rp.byte_count, 512u, "P.BCNT round-trips");

	memset(&we, 0, sizeof(we));
	we.eh.hdr.cmd_ref = 0x300u;
	we.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_WRITE_PROT,
				       VMS_MSCP_SUB_WP_SOFTWARE);

	ct_check(build_link_prefix(wframe, sizeof(wframe),
				   VMS_MSCP_END_SCA_LEN(VMS_MSCP_WRITE_END_LEN)),
		 "link prefix builds (WRITE)");
	ct_check(vms_mscp_write_end_build(&we, wframe, sizeof(wframe), &wwritten)
			 == VMS_CODEC_OK,
		 "WRITE end body builds");
	wlen = VMS_OFF_SYSAP_BODY + wwritten;
	ct_check_eq_u32(wlen, VMS_MSCP_END_FRAME_LEN(VMS_MSCP_WRITE_END_LEN),
			"a WRITE end message is 36 bytes -- MEASURED, four MORE than "
			"READ's 32; the two are not the same length");
	ct_check(vms_mscp_write_end_parse(wframe, wlen, &wp) == VMS_CODEC_OK,
		 "WRITE end parses back");
	ct_check_eq_u32(wp.eh.hdr.opcode, VMS_MSCP_OP_WRITE | VMS_MSCP_END_BIT,
			"the WRITE endcode is 0xa2");
	ct_check_eq_u32(wp.eh.status,
			VMS_MSCP_STATUS(VMS_MSCP_ST_WRITE_PROT,
				       VMS_MSCP_SUB_WP_SOFTWARE),
			"Write Protected / Software sub-code composes to 0x1006");
}

static void test_xfer_and_other_cmd_roundtrips(void)
{
	struct vms_mscp_gus_cmd g, gp;
	struct vms_mscp_online_cmd o, op;
	struct vms_mscp_xfer_cmd rd, rp, wr, wp;
	uint8_t f1[256], f2[256], f3[256], f4[256];
	uint32_t w1 = 0, w2 = 0, w3 = 0, w4 = 0;
	static const uint8_t buf_desc[12] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12
	};

	printf("-- GUS/ONLINE/READ/WRITE COMMAND round trips\n");

	memset(&g, 0, sizeof(g));
	g.hdr.cmd_ref = 0x2u;
	g.hdr.unit = 0u;
	g.modifiers = VMS_MSCP_MOD_NEXT_UNIT;
	ct_check(build_link_prefix(f1, sizeof(f1), VMS_MSCP_CMD_SCA_LEN),
		 "link prefix builds (GUS)");
	ct_check(vms_mscp_gus_cmd_build(&g, f1, sizeof(f1), &w1) == VMS_CODEC_OK,
		 "GUS command builds");
	ct_check(vms_mscp_gus_cmd_parse(f1, VMS_OFF_SYSAP_BODY + w1, &gp)
			 == VMS_CODEC_OK &&
			 gp.modifiers == VMS_MSCP_MOD_NEXT_UNIT,
		 "GUS command round-trips MD.NXU");

	memset(&o, 0, sizeof(o));
	o.hdr.cmd_ref = 0x103u;
	o.hdr.unit = 1u;
	o.unit_flags = 0x8000u;
	ct_check(build_link_prefix(f2, sizeof(f2), VMS_MSCP_CMD_SCA_LEN),
		 "link prefix builds (ONLINE)");
	ct_check(vms_mscp_online_cmd_build(&o, f2, sizeof(f2), &w2) == VMS_CODEC_OK,
		 "ONLINE command builds");
	ct_check(vms_mscp_online_cmd_parse(f2, VMS_OFF_SYSAP_BODY + w2, &op)
			 == VMS_CODEC_OK &&
			 op.unit_flags == 0x8000u,
		 "ONLINE command round-trips the host's requested P.UNFL (bit 15)");

	memset(&rd, 0, sizeof(rd));
	rd.hdr.cmd_ref = 0xabcu;
	rd.hdr.opcode = VMS_MSCP_OP_READ;
	rd.byte_count = 512u;
	memcpy(rd.buffer_desc, buf_desc, 12);
	rd.lbn = 3u;
	ct_check(build_link_prefix(f3, sizeof(f3), VMS_MSCP_CMD_SCA_LEN),
		 "link prefix builds (READ cmd)");
	ct_check(vms_mscp_xfer_cmd_build(&rd, f3, sizeof(f3), &w3) == VMS_CODEC_OK,
		 "READ command builds");
	ct_check(vms_mscp_xfer_cmd_parse(f3, VMS_OFF_SYSAP_BODY + w3, &rp)
			 == VMS_CODEC_OK &&
			 rp.lbn == 3u && rp.byte_count == 512u &&
			 memcmp(rp.buffer_desc, buf_desc, 12) == 0,
		 "READ command round-trips P.BCNT/P.BUFF/P.LBN");

	memset(&wr, 0, sizeof(wr));
	wr.hdr.cmd_ref = 0xdefu;
	wr.hdr.opcode = VMS_MSCP_OP_WRITE;
	wr.byte_count = 1024u;
	memcpy(wr.buffer_desc, buf_desc, 12);
	wr.lbn = 9u;
	ct_check(build_link_prefix(f4, sizeof(f4), VMS_MSCP_CMD_SCA_LEN),
		 "link prefix builds (WRITE cmd)");
	ct_check(vms_mscp_xfer_cmd_build(&wr, f4, sizeof(f4), &w4) == VMS_CODEC_OK,
		 "WRITE command builds");
	ct_check(vms_mscp_xfer_cmd_parse(f4, VMS_OFF_SYSAP_BODY + w4, &wp)
			 == VMS_CODEC_OK &&
			 wp.hdr.opcode == VMS_MSCP_OP_WRITE && wp.lbn == 9u,
		 "WRITE command round-trips and keeps its opcode distinct from READ");
}

/* ---- 3: the classifier's own collision case ---------------------------- */

static void test_classify_94_collision(void)
{
	struct vms_mscp_scc_cmd c;
	struct vms_mscp_xfer_end we;
	uint8_t cmd_frame[256], end_frame[256];
	uint32_t cmd_written = 0, end_written = 0, cmd_len, end_len;
	struct vms_frame_info fi_cmd, fi_end;
	enum vms_mscp_class cls_cmd = VMS_MSCP_CLS_UNKNOWN;
	enum vms_mscp_class cls_end = VMS_MSCP_CLS_UNKNOWN;

	printf("-- the 94-byte collision: a COMMAND and a WRITE END share a "
	      "content length; only the opcode's END bit tells them apart\n");

	memset(&c, 0, sizeof(c));
	c.hdr.cmd_ref = 1u;
	ct_check(build_link_prefix(cmd_frame, sizeof(cmd_frame),
				   VMS_MSCP_CMD_SCA_LEN),
		 "link prefix builds (cmd)");
	ct_check(vms_mscp_scc_cmd_build(&c, cmd_frame, sizeof(cmd_frame),
					&cmd_written) == VMS_CODEC_OK,
		 "a 94-content SCC command builds");
	cmd_len = VMS_OFF_SYSAP_BODY + cmd_written;

	memset(&we, 0, sizeof(we));
	we.eh.hdr.cmd_ref = 2u;
	ct_check(build_link_prefix(end_frame, sizeof(end_frame),
				   VMS_MSCP_END_SCA_LEN(VMS_MSCP_WRITE_END_LEN)),
		 "link prefix builds (write end)");
	ct_check(vms_mscp_write_end_build(&we, end_frame, sizeof(end_frame),
					  &end_written) == VMS_CODEC_OK,
		 "a 94-content WRITE end builds");
	end_len = VMS_OFF_SYSAP_BODY + end_written;

	ct_check_eq_u32(cmd_len, end_len,
			"both frames really are the same total length (94 content)");

	fi_cmd = classify(cmd_frame, cmd_len);
	fi_end = classify(end_frame, end_len);
	(void)vms_mscp_classify(cmd_frame, cmd_len, &fi_cmd, &cls_cmd);
	(void)vms_mscp_classify(end_frame, end_len, &fi_end, &cls_end);

	ct_check(cls_cmd == VMS_MSCP_CLS_CMD,
		 "the command frame classifies VMS_MSCP_CLS_CMD");
	ct_check(cls_end == VMS_MSCP_CLS_WRITE_END,
		 "the WRITE-end frame classifies VMS_MSCP_CLS_WRITE_END, NOT CMD, "
		 "despite the identical content length");
}

/* ---- 4: status split + allowlist ---------------------------------------- */

static void test_status_split(void)
{
	uint16_t w;

	printf("-- status major/sub split (sec 5.6)\n");
	w = VMS_MSCP_STATUS(VMS_MSCP_ST_OFFLINE, 1u); /* Unit-Offline, no volume */
	ct_check_eq_u32(vms_mscp_status_major(w), VMS_MSCP_ST_OFFLINE,
			"major extracts Unit-Offline (3) out of a composed word");
	ct_check_eq_u32(vms_mscp_status_subcode(w), 1u,
			"subcode extracts 1 (no volume mounted) separately");
	ct_check(vms_mscp_status_major(3u) == VMS_MSCP_ST_OFFLINE,
		 "a bare status==3 (no sub-code shifted in) still reads major 3 -- "
		 "the point being callers must use the split, not ==, to test it");
}

static void test_allowlist(void)
{
	printf("-- the allowlist rows this item contributes\n");
	ct_check(vms_wire_allow_table_validate(&vms_mscp_allow_table) == VMS_CODEC_OK,
		 "the table validates structurally (no dup keys, every row cites "
		 "the spec, RESPOND rows carry a nonzero recipe)");
	ct_check(vms_wire_allow_find(&vms_mscp_allow_table, VMS_SYSAP_MSCP_DISK,
				     VMS_MSCP_ALLOW_CATEGORY,
				     VMS_MSCP_OP_SCC) != NULL,
		 "SCC resolves");
	ct_check(vms_wire_allow_find(&vms_mscp_allow_table, VMS_SYSAP_MSCP_DISK,
				     VMS_MSCP_ALLOW_CATEGORY,
				     VMS_MSCP_OP_WRITE) != NULL,
		 "WRITE resolves");
	ct_check(vms_wire_allow_find(&vms_mscp_allow_table, VMS_SYSAP_MSCP_DISK,
				     VMS_MSCP_ALLOW_CATEGORY, 0x7fu) == NULL,
		 "an ungrounded opcode resolves to NOTHING -- silence, per spec §4(p)");
}

int main(void)
{
	printf("test_codec_mscp: MSCP-over-SCS codec entries (FC-P6.2)\n");

	test_measured_lengths_are_literal();
	test_scc_end_golden();
	test_scc_cmd_roundtrip();
	test_gus_end_fields();
	test_online_end_and_unfl_echo();
	test_read_write_end_lengths();
	test_xfer_and_other_cmd_roundtrips();
	test_classify_94_collision();
	test_status_split();
	test_allowlist();

	return ct_summary("test_codec_mscp");
}
