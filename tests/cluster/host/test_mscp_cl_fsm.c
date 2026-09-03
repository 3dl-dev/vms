// SPDX-License-Identifier: GPL-2.0
/*
 * test_mscp_cl_fsm.c - the MSCP disk-client DISCOVERY FSM, rung R1
 * (FC-P3.4).
 *
 * BYTE-EXACT AGAINST THE AF2 SPECIMENS. The two command golden vectors
 * below are TRANSCRIBED from src/vmsscs/include/scs_mscp.h's own golden_scc/
 * golden_gus arrays (already-committed, public bytes -- not code; the same
 * "transcribed here to prove this INDEPENDENT codec reproduces the real
 * answer" discipline test_codec_mscp.c's own SCC-END golden test already
 * uses), themselves captured from af2-firsttimer-established-20260728.pcap.
 * Scope: the MSCP MESSAGE BODY (content[58:94), 36 bytes) only -- the P6.2
 * codec's own struct vms_mscp_link doc names content[22:24) (the
 * strawman's "incarnation" byte) as an UNGROUNDED span this FSM does not
 * reach around (see vms_mscp_cl_fsm.h's own "WHAT IS DELIBERATELY LEFT"
 * note), so a full-envelope byte-exact claim would overstate what P6.2
 * grounds. The GUS end message's cmd_ref/unit/status/unit_flags/media_id
 * fields used below are likewise decoded straight from the real
 * golden_gus_end af2 capture (see the field-by-field derivation in the
 * comments at each use), not invented.
 *
 * THE WALK SHAPE (SCC x2 -> GUS NEXT-UNIT walk -> OFFLINE terminator) is
 * GROUNDED docs/cluster-protocol-spec.md sec 4(n), decoded from BOTH the
 * af2-firsttimer and vax3-2to3-established-20260730.pcap captures ("2x SET
 * CONTROLLER CHARACTERISTICS, then the full GET-UNIT-STATUS NEXT-UNIT walk
 * (10 command/END pairs)" -- vax3-2to3; "each subsequent command uses the
 * previous END's returned unit word + 1 ... ends when an END returns
 * status OFFLINE" -- af2/vax3 shared analysis). No raw vax3 MSCP command
 * bytes are committed in this tree (only the decoded shape), so the walk's
 * PROGRESSION and TERMINATION are exercised here with synthetic-but-valid
 * END messages built through the same FC-P6.2 codec that test_codec_mscp.c
 * already proves reproduces a real server's answer byte-exact -- this file
 * does not re-derive that codec's own correctness, only this FSM's use of
 * it.
 */
#include "cluster_test.h"
#include "vms_mscp_cl_fsm.h"

#include <string.h>

/* ---- shared scaffolding -------------------------------------------------
 * The af2 joiner/member identity this specimen's captures used
 * (src/vmsscs/include/scs_mscp.h's vax1_logical/joiner_logical/
 * VAX1_MSCP_CONID/JOINER_MSCP_CONID, transcribed the same way). Ethernet
 * MACs are NOT part of the golden SCA-content span (see file header scope
 * note) so any value is honest here. */
static const uint8_t eth_dst[6] = { 0x08, 0x00, 0x2b, 0x11, 0x22, 0x33 };
static const uint8_t eth_src[6] = { 0x08, 0x00, 0x2b, 0x44, 0x55, 0x66 };
static const uint8_t vax1_logical[6]   = { 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04 };
static const uint8_t joiner_logical[6] = { 0xaa, 0x00, 0x04, 0x00, 0x1a, 0x04 };
#define VAX1_MSCP_CONID   0x3554000au
#define JOINER_MSCP_CONID 0x8fd20008u

static void mk_link(struct vms_mscp_link *l, uint16_t recv_ack,
		    uint16_t send_seq)
{
	memset(l, 0, sizeof(*l));
	memcpy(l->hdr.eth_dst, eth_dst, 6);
	memcpy(l->hdr.eth_src, eth_src, 6);
	memcpy(l->hdr.dst_lavc, vax1_logical, 6);
	memcpy(l->hdr.src_lavc, joiner_logical, 6);
	l->hdr.connect_flag = 0x0001u; /* content[8:10], observed constant */
	l->recv_ack = recv_ack;
	l->send_seq = send_seq;
	l->credit = VMS_MSCP_ENV_CREDIT_OBSERVED;
	l->remote_conid = VAX1_MSCP_CONID;
	l->local_conid = JOINER_MSCP_CONID;
}

static vms_codec_status_t build_scc_end_frame(uint32_t cmd_ref,
					      uint16_t status,
					      uint8_t *frame, uint32_t cap,
					      uint32_t *len)
{
	struct vms_mscp_link l;
	struct vms_mscp_scc_end e;
	uint32_t link_written = 0, body_written = 0;
	vms_codec_status_t st;

	mk_link(&l, 0x0000u, 0x0000u);
	st = vms_mscp_link_build(&l, VMS_MSCP_END_SCA_LEN(VMS_MSCP_SCC_END_LEN),
				 frame, cap, &link_written);
	if (st != VMS_CODEC_OK)
		return st;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = cmd_ref;
	e.eh.status = status;
	st = vms_mscp_scc_end_build(&e, frame, cap, &body_written);
	if (st != VMS_CODEC_OK)
		return st;
	*len = VMS_OFF_SYSAP_BODY + body_written;
	return VMS_CODEC_OK;
}

static vms_codec_status_t build_gus_end_frame(uint32_t cmd_ref,
					      uint16_t status, uint16_t unit,
					      uint16_t unit_flags,
					      uint32_t media_id,
					      uint8_t *frame, uint32_t cap,
					      uint32_t *len)
{
	struct vms_mscp_link l;
	struct vms_mscp_gus_end e;
	uint32_t link_written = 0, body_written = 0;
	vms_codec_status_t st;

	mk_link(&l, 0x0000u, 0x0000u);
	st = vms_mscp_link_build(&l, VMS_MSCP_END_SCA_LEN(VMS_MSCP_GUS_END_LEN),
				 frame, cap, &link_written);
	if (st != VMS_CODEC_OK)
		return st;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = cmd_ref;
	e.eh.hdr.unit = unit;
	e.eh.status = status;
	e.unit_flags = unit_flags;
	e.media_id = media_id;
	st = vms_mscp_gus_end_build(&e, frame, cap, &body_written);
	if (st != VMS_CODEC_OK)
		return st;
	*len = VMS_OFF_SYSAP_BODY + body_written;
	return VMS_CODEC_OK;
}

/* ---- 1: init seeds the walk correctly ----------------------------------- */

static void test_init_seeds_walk_correctly(void)
{
	struct vms_mscp_cl_fsm f;

	printf("-- init: state INIT, msgid seeds, unit seed is 1 (never 0)\n");
	vms_mscp_cl_fsm_init(&f);
	ct_check_eq_u32(f.state, VMS_MSCP_CL_ST_INIT, "state == INIT");
	ct_check_eq_u32(f.scc_msgid, VMS_MSCP_CL_SCC_MSGID0,
			"SCC msgid seeded 0x81a3");
	ct_check_eq_u32(f.gus_msgid, VMS_MSCP_CL_GUS_MSGID0,
			"GUS msgid seeded 0x7ee2");
	/* sec 4(n): "Seeding the first GUS with unit 0x0000 makes the server
	 * answer OFFLINE immediately ... a silent, plausible-looking
	 * failure." Pinned against the literal 1, not merely against the
	 * macro that could be silently changed to 0. */
	ct_check_eq_u32(f.next_unit, 1u,
			"GUS walk cursor seeds at unit 1, never 0");
	ct_check_eq_u32(VMS_MSCP_CL_GUS_SEED_UNIT, 1u,
			"the seed constant itself is literally 1");
	ct_check(!vms_mscp_cl_fsm_done(&f), "not done at init");
}

/* ---- 2/3: SCC #1, byte-exact against the af2 golden command ------------ */

/* golden_scc[58:94) -- the MSCP MESSAGE body only (36 bytes), transcribed
 * from src/vmsscs/include/scs_mscp.h's golden_scc[94] (af2-firsttimer-
 * established-20260728.pcap). P.CRF = SCS_MSCP_CMD_REF(SCC_CLASS=2,
 * MSGID0=0x81a3) = 0x81a30002; P.CNTF = 0x00d0 (CF.ATN|CF.MSC|CF.THS);
 * P.TIME = 0x00bc021975280bc0, the captured wall clock. */
static const uint8_t golden_scc_cmd_body[36] = {
	0x02, 0x00, 0xa3, 0x81, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x0b, 0x28, 0x75,
	0x19, 0x02, 0xbc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#define GOLDEN_SCC_CTLR_FLAGS   0x00d0u /* CF.ATN|CF.MSC|CF.THS, af2 */
#define GOLDEN_SCC_HOST_TIMEOUT 0x0000u
#define GOLDEN_SCC_TIME         0x00bc021975280bc0ULL

static void test_scc1_byte_exact_af2(struct vms_mscp_cl_fsm *f)
{
	struct vms_mscp_link link;
	uint8_t frame[256];
	uint32_t written = 0;
	vms_codec_status_t st;

	printf("-- SCC #1: byte-exact against the af2 golden command\n");
	/* af2's own recv_ack/send_seq for this exchange (content[18:22)):
	 * 0x0018/0x0019 -- the live connection's own sequenced-message
	 * state, caller-supplied per the header's "never plumb a template"
	 * rule. */
	mk_link(&link, 0x0018u, 0x0019u);
	memset(frame, 0xAA, sizeof(frame));

	st = vms_mscp_cl_fsm_build_scc(f, &link, GOLDEN_SCC_CTLR_FLAGS,
				       GOLDEN_SCC_HOST_TIMEOUT, GOLDEN_SCC_TIME,
				       frame, sizeof(frame), &written);
	ct_check(st == VMS_CODEC_OK, "build_scc (#1) succeeds");
	ct_check_eq_u32(written, VMS_MSCP_CMD_FRAME_LEN,
			"total frame length is 14+94 = 108");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_SCC1_SENT,
			"state advances to SCC1_SENT");
	ct_check_eq_u32(f->pending_cmd_ref, 0x81a30002u,
			"pending P.CRF == SCS_MSCP_CMD_REF(SCC_CLASS, 0x81a3)");
	ct_check_eq_u32(f->scc_msgid, (uint16_t)(VMS_MSCP_CL_SCC_MSGID0 + 1u),
			"SCC message id increments after send");

	{
		int same = memcmp(frame + VMS_OFF_SYSAP_BODY, golden_scc_cmd_body,
				  sizeof(golden_scc_cmd_body)) == 0;
		ct_check(same, "SCC #1 command body is BYTE-IDENTICAL to the "
			      "real VAX joiner's af2 command");
		if (!same) {
			uint32_t i;
			printf("    built:  ");
			for (i = 0; i < sizeof(golden_scc_cmd_body); i++)
				printf("%02x ", frame[VMS_OFF_SYSAP_BODY + i]);
			printf("\n    golden: ");
			for (i = 0; i < sizeof(golden_scc_cmd_body); i++)
				printf("%02x ", golden_scc_cmd_body[i]);
			printf("\n");
		}
	}
}

static void test_build_scc_refused_out_of_state(struct vms_mscp_cl_fsm *f)
{
	struct vms_mscp_link link;
	uint8_t frame[256];
	uint32_t written = 0;
	vms_codec_status_t st;

	printf("-- build_scc refuses a second call while SCC #1 is still "
	      "outstanding\n");
	mk_link(&link, 0x0018u, 0x0019u);
	st = vms_mscp_cl_fsm_build_scc(f, &link, 0, 0, 0, frame, sizeof(frame),
				       &written);
	ct_check(st == VMS_CODEC_E_CLASS,
		 "refused: state SCC1_SENT is not INIT or SCC1_DONE");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_SCC1_SENT,
			"state is unchanged by the refused call");
}

static void test_scc1_end_refuses_mismatched_cmd_ref(void)
{
	struct vms_mscp_cl_fsm f;
	struct vms_mscp_link link;
	uint8_t cmd_frame[256], end_frame[256];
	uint32_t written = 0, end_len = 0;
	vms_codec_status_t st;

	printf("-- on_scc_end refuses an END whose P.CRF does not match the "
	      "outstanding command\n");
	vms_mscp_cl_fsm_init(&f);
	mk_link(&link, 0x0018u, 0x0019u);
	ct_check(vms_mscp_cl_fsm_build_scc(&f, &link, 0, 0, 0, cmd_frame,
					   sizeof(cmd_frame), &written)
			 == VMS_CODEC_OK,
		 "build_scc (#1) succeeds");

	st = build_scc_end_frame(0xdeadbeefu,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0),
				 end_frame, sizeof(end_frame), &end_len);
	ct_check(st == VMS_CODEC_OK, "a well-formed but MISMATCHED SCC-END builds");

	st = vms_mscp_cl_fsm_on_scc_end(&f, end_frame, end_len);
	ct_check(st == VMS_CODEC_E_CLASS,
		 "refused: echoed P.CRF 0xdeadbeef != the outstanding 0x81a30002");
	ct_check_eq_u32(f.state, VMS_MSCP_CL_ST_SCC1_SENT,
			"state does not advance on a refused answer");
}

/* ---- 4/5: consume SCC #1's END, send SCC #2, msgid increments ---------- */

static void test_scc1_end_advances_and_scc2_sent(struct vms_mscp_cl_fsm *f)
{
	uint8_t end_frame[256];
	uint32_t end_len = 0;
	vms_codec_status_t st;

	printf("-- SCC #1 END (status SUCCESS) advances SCC1_SENT -> "
	      "SCC1_DONE\n");
	st = build_scc_end_frame(f->pending_cmd_ref,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS,
						 VMS_MSCP_SUB_NORMAL),
				 end_frame, sizeof(end_frame), &end_len);
	ct_check(st == VMS_CODEC_OK, "SCC #1 END frame builds");

	st = vms_mscp_cl_fsm_on_scc_end(f, end_frame, end_len);
	ct_check(st == VMS_CODEC_OK, "on_scc_end (#1) accepts the matching END");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_SCC1_DONE,
			"state == SCC1_DONE, ready for SCC #2");
}

static void test_scc2_msgid_increments(struct vms_mscp_cl_fsm *f)
{
	struct vms_mscp_link link;
	uint8_t frame[256];
	uint32_t written = 0;
	vms_codec_status_t st;

	printf("-- SCC #2: same class, message id INCREMENTS (sec 5.1 "
	      "uniqueness, sec 4(n) \"increments per command, echoed "
	      "verbatim\")\n");
	mk_link(&link, 0x0018u, 0x0019u);
	st = vms_mscp_cl_fsm_build_scc(f, &link, GOLDEN_SCC_CTLR_FLAGS,
				       GOLDEN_SCC_HOST_TIMEOUT, GOLDEN_SCC_TIME,
				       frame, sizeof(frame), &written);
	ct_check(st == VMS_CODEC_OK, "build_scc (#2) succeeds from SCC1_DONE");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_SCC2_SENT, "state == SCC2_SENT");
	ct_check_eq_u32(f->pending_cmd_ref, 0x81a40002u,
			"P.CRF msgid advanced 0x81a3 -> 0x81a4, class token "
			"unchanged (0x0002)");
}

static void test_scc2_end_advances_to_gus_ready(struct vms_mscp_cl_fsm *f)
{
	uint8_t end_frame[256];
	uint32_t end_len = 0;
	vms_codec_status_t st;

	printf("-- SCC #2 END advances SCC2_SENT -> GUS_READY: both SCCs "
	      "done, the walk may begin\n");
	st = build_scc_end_frame(f->pending_cmd_ref,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0),
				 end_frame, sizeof(end_frame), &end_len);
	ct_check(st == VMS_CODEC_OK, "SCC #2 END frame builds");
	st = vms_mscp_cl_fsm_on_scc_end(f, end_frame, end_len);
	ct_check(st == VMS_CODEC_OK, "on_scc_end (#2) accepts");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_GUS_READY, "state == GUS_READY");
}

static void test_build_gus_refused_before_two_sccs_done(void)
{
	struct vms_mscp_cl_fsm f;
	struct vms_mscp_link link;
	uint8_t frame[256];
	uint32_t written = 0;
	vms_codec_status_t st;

	printf("-- build_gus refuses before both SCCs have completed\n");
	vms_mscp_cl_fsm_init(&f);
	mk_link(&link, 0x001au, 0x001bu);
	st = vms_mscp_cl_fsm_build_gus(&f, &link, frame, sizeof(frame), &written);
	ct_check(st == VMS_CODEC_E_CLASS,
		 "refused: state INIT is not GUS_READY");
}

/* ---- 6: GUS #1, byte-exact against the af2 golden command --------------- */

/* golden_gus[58:94) -- the MSCP MESSAGE body only (36 bytes), transcribed
 * from src/vmsscs/include/scs_mscp.h's golden_gus[94]. P.CRF =
 * SCS_MSCP_CMD_REF(GUS_CLASS=1, MSGID0=0x7ee2) = 0x7ee20001; P.UNIT =
 * 0x0001 (the walk's seed unit); P.MOD = 0x0001 (MD.NXU). */
static const uint8_t golden_gus_cmd_body[36] = {
	0x01, 0x00, 0xe2, 0x7e, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void test_gus1_byte_exact_af2(struct vms_mscp_cl_fsm *f)
{
	struct vms_mscp_link link;
	uint8_t frame[256];
	uint32_t written = 0;
	vms_codec_status_t st;

	printf("-- GUS #1: byte-exact against the af2 golden command\n");
	/* af2's own recv_ack/send_seq for this exchange: 0x001a/0x001b. */
	mk_link(&link, 0x001au, 0x001bu);
	memset(frame, 0xAA, sizeof(frame));

	st = vms_mscp_cl_fsm_build_gus(f, &link, frame, sizeof(frame), &written);
	ct_check(st == VMS_CODEC_OK, "build_gus (#1) succeeds from GUS_READY");
	ct_check_eq_u32(written, VMS_MSCP_CMD_FRAME_LEN,
			"total frame length is 14+94 = 108");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_GUS_SENT, "state == GUS_SENT");
	ct_check_eq_u32(f->pending_cmd_ref, 0x7ee20001u,
			"pending P.CRF == SCS_MSCP_CMD_REF(GUS_CLASS, 0x7ee2)");
	ct_check_eq_u32(f->next_unit, 1u, "queries the seeded unit 1");

	{
		int same = memcmp(frame + VMS_OFF_SYSAP_BODY, golden_gus_cmd_body,
				  sizeof(golden_gus_cmd_body)) == 0;
		ct_check(same, "GUS #1 command body is BYTE-IDENTICAL to the "
			      "real VAX joiner's af2 command");
		if (!same) {
			uint32_t i;
			printf("    built:  ");
			for (i = 0; i < sizeof(golden_gus_cmd_body); i++)
				printf("%02x ", frame[VMS_OFF_SYSAP_BODY + i]);
			printf("\n    golden: ");
			for (i = 0; i < sizeof(golden_gus_cmd_body); i++)
				printf("%02x ", golden_gus_cmd_body[i]);
			printf("\n");
		}
	}
}

/* ---- 7: GUS #1's END, decoded from the af2 golden_gus_end capture ------- */

/* Field-level decode of src/vmsscs/include/scs_mscp.h's golden_gus_end[110]
 * (af2-firsttimer-established): P.CRF echoes 0x7ee20001 (the GUS #1
 * command above), P.UNIT (returned) = 0x4000, P.STS = 0x0004 (UNIT
 * AVAILABLE major, sub-code 0), P.UNFL = 0x8000, P.MEDI = 0x2564105c. */
#define GOLDEN_GUS1_END_UNIT       0x4000u
#define GOLDEN_GUS1_END_UNIT_FLAGS 0x8000u
#define GOLDEN_GUS1_END_MEDIA_ID   0x2564105cu

static void test_gus1_end_af2_advances_walk(struct vms_mscp_cl_fsm *f)
{
	uint8_t end_frame[256];
	uint32_t end_len = 0;
	struct vms_mscp_cl_unit unit;
	int is_terminator = -1;
	vms_codec_status_t st;

	printf("-- GUS #1 END (af2, unit 0x4000, AVAILABLE) advances the "
	      "walk cursor to 0x4001\n");
	memset(&unit, 0xAA, sizeof(unit));
	st = build_gus_end_frame(f->pending_cmd_ref,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_AVAILABLE, 0),
				 GOLDEN_GUS1_END_UNIT, GOLDEN_GUS1_END_UNIT_FLAGS,
				 GOLDEN_GUS1_END_MEDIA_ID, end_frame,
				 sizeof(end_frame), &end_len);
	ct_check(st == VMS_CODEC_OK, "GUS #1 END frame builds");

	st = vms_mscp_cl_fsm_on_gus_end(f, end_frame, end_len, &unit,
					&is_terminator);
	ct_check(st == VMS_CODEC_OK, "on_gus_end (#1) accepts the matching END");
	ct_check_eq_u32((unsigned)is_terminator, 0u,
			"AVAILABLE is not the walk terminator");
	ct_check_eq_u32(unit.unit, GOLDEN_GUS1_END_UNIT,
			"reported unit == the af2 server's own 0x4000");
	ct_check_eq_u32(unit.unit_flags, GOLDEN_GUS1_END_UNIT_FLAGS,
			"reported unit_flags == af2's 0x8000");
	ct_check_eq_u32(unit.media_id, GOLDEN_GUS1_END_MEDIA_ID,
			"reported media_id == af2's 0x2564105c");
	ct_check_eq_u32(unit.status_major, VMS_MSCP_ST_AVAILABLE,
			"reported status_major == AVAILABLE");
	ct_check_eq_u32(f->next_unit, 0x4001u,
			"walk cursor advances to the returned unit + 1 -- "
			"matches src/vmsscs/scs_mscp.c's own \"cmd#2 of the "
			"enumeration: unit 0x4001\" observation");
	ct_check_eq_u32(f->units_found, 1u, "one unit counted");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_GUS_READY,
			"state returns to GUS_READY for the next walk step");
}

static void test_gus2_command_uses_returned_unit(struct vms_mscp_cl_fsm *f)
{
	struct vms_mscp_link link;
	uint8_t frame[256];
	uint32_t written = 0;
	struct vms_mscp_gus_cmd parsed;
	vms_codec_status_t st;

	printf("-- GUS #2: queries unit 0x4001 (the walk cursor), message id "
	      "advances\n");
	mk_link(&link, 0x001cu, 0x001du);
	st = vms_mscp_cl_fsm_build_gus(f, &link, frame, sizeof(frame), &written);
	ct_check(st == VMS_CODEC_OK, "build_gus (#2) succeeds");
	ct_check_eq_u32(f->pending_cmd_ref, 0x7ee30001u,
			"P.CRF msgid advanced 0x7ee2 -> 0x7ee3");

	st = vms_mscp_gus_cmd_parse(frame, written, &parsed);
	ct_check(st == VMS_CODEC_OK, "GUS #2 command parses back");
	ct_check_eq_u32(parsed.hdr.unit, 0x4001u,
			"P.UNIT queried == the previous END's unit + 1");
	ct_check_eq_u32(parsed.modifiers, VMS_MSCP_MOD_NEXT_UNIT,
			"MD.NXU still set on every walk step");
}

/* ---- 8: the walk terminates on status OFFLINE --------------------------- */

static void test_walk_terminates_on_offline(struct vms_mscp_cl_fsm *f)
{
	uint8_t end_frame[256], gus_frame[256];
	uint32_t end_len = 0, gus_written = 0;
	struct vms_mscp_cl_unit unit;
	int is_terminator = -1;
	struct vms_mscp_link link;
	vms_codec_status_t st;

	printf("-- GUS #2 END (a second real unit, synthetic-but-valid) then "
	      "GUS #3 answered OFFLINE terminates the walk\n");

	/* GUS #2's own END: no third af2 specimen is committed in this
	 * tree, so this step is synthetic-but-valid (built through the same
	 * FC-P6.2 codec) rather than a second golden vector -- see the file
	 * header's scope note. */
	memset(&unit, 0, sizeof(unit));
	st = build_gus_end_frame(f->pending_cmd_ref,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_AVAILABLE, 0),
				 0x4002u, 0x0000u, 0x11111111u, end_frame,
				 sizeof(end_frame), &end_len);
	ct_check(st == VMS_CODEC_OK, "GUS #2 END frame builds");
	st = vms_mscp_cl_fsm_on_gus_end(f, end_frame, end_len, &unit,
					&is_terminator);
	ct_check(st == VMS_CODEC_OK && is_terminator == 0,
		 "GUS #2 END accepted, not yet the terminator");
	ct_check_eq_u32(f->next_unit, 0x4003u, "cursor advances to 0x4003");

	/* GUS #3: query unit 0x4003, answered OFFLINE -- sec 4(n)'s
	 * end-of-list terminator, not an error. */
	mk_link(&link, 0x001eu, 0x001fu);
	st = vms_mscp_cl_fsm_build_gus(f, &link, gus_frame, sizeof(gus_frame),
				       &gus_written);
	ct_check(st == VMS_CODEC_OK, "build_gus (#3) succeeds");

	st = build_gus_end_frame(f->pending_cmd_ref,
				 VMS_MSCP_STATUS(VMS_MSCP_ST_OFFLINE,
						 VMS_MSCP_SUB_OFL_NO_VOLUME),
				 0x0000u, 0x0000u, 0x00000000u, end_frame,
				 sizeof(end_frame), &end_len);
	ct_check(st == VMS_CODEC_OK, "GUS #3 OFFLINE END frame builds");

	is_terminator = -1;
	st = vms_mscp_cl_fsm_on_gus_end(f, end_frame, end_len, &unit,
					&is_terminator);
	ct_check(st == VMS_CODEC_OK, "on_gus_end (#3) accepts the OFFLINE END");
	ct_check_eq_u32((unsigned)is_terminator, 1u,
			"status OFFLINE IS the walk terminator");
	ct_check_eq_u32(f->state, VMS_MSCP_CL_ST_DONE, "state == DONE");
	ct_check(vms_mscp_cl_fsm_done(f), "vms_mscp_cl_fsm_done() agrees");
	ct_check_eq_u32(f->units_found, 2u,
			"exactly the two AVAILABLE units were counted, not "
			"the OFFLINE terminator");

	{
		struct vms_mscp_link link2;
		uint8_t frame2[256];
		uint32_t written2 = 0;

		mk_link(&link2, 0x0020u, 0x0021u);
		st = vms_mscp_cl_fsm_build_gus(f, &link2, frame2, sizeof(frame2),
					       &written2);
		ct_check(st == VMS_CODEC_E_CLASS,
			 "build_gus refuses once the walk is DONE");
	}
}

int main(void)
{
	struct vms_mscp_cl_fsm f;

	printf("test_mscp_cl_fsm: MSCP disk-client discovery FSM (FC-P3.4)\n");

	test_init_seeds_walk_correctly();

	vms_mscp_cl_fsm_init(&f);
	test_scc1_byte_exact_af2(&f);
	test_build_scc_refused_out_of_state(&f);
	test_scc1_end_refuses_mismatched_cmd_ref();
	test_scc1_end_advances_and_scc2_sent(&f);
	test_scc2_msgid_increments(&f);
	test_scc2_end_advances_to_gus_ready(&f);
	test_build_gus_refused_before_two_sccs_done();
	test_gus1_byte_exact_af2(&f);
	test_gus1_end_af2_advances_walk(&f);
	test_gus2_command_uses_returned_unit(&f);
	test_walk_terminates_on_offline(&f);

	return ct_summary("test_mscp_cl_fsm");
}
