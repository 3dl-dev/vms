// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_applmsg.c - the length-generic APPL_MSG classify widen, rung
 * R1 (FC-P2.7, design docs/design-faithful-cluster-executive.md §3.2.7,
 * E48 raised by FC-P6.5's mscp_write.c scenario).
 *
 * NO FIXTURE CORPUS FOR THE FIVE MSCP END LENGTHS. The vms291 lab-2 mount
 * capture that measured SCC/READ/WRITE/ONLINE/GUS END (86/90/94/102/110) is
 * a HOST-ONLY pcap artifact -- docs/design-mscp-direction.md's own "Host-only
 * artifacts, never in git" table -- and is not in the clean-room manifest
 * (docs/clean-room/reference-captures.sha256), so a `.spec` fixture citing it
 * would be false provenance (Rule 8). This file instead builds each specimen
 * through the SAME shipping, GROUNDED codec entries real MSCP traffic uses:
 *   - the envelope: vms_sca_hdr_build() (abs[0,32)) + vms_scs_hdr_build()
 *     (abs[56,72), the inner-length/format-word/MTYPE/credit/Con.ID span
 *     vms_scs_fsm.c's msg_transmit_var() -- the real END-message send path --
 *     stamps on every outbound END message);
 *   - the body: the FC-P6.2 MSCP end-message builders (vms_mscp_scc_end_
 *     build() etc), whose five lengths are each MEASURED (src/kernel-core/
 *     vms_cluster_codec_mscp.h's own "MEASURED, vms-291 lab-2 capture" /
 *     "MEASURED, 954/954 captured" / "MEASURED, 18855/18855 captured" cites).
 * No wire byte here is invented: every field traces to a builder this tree
 * already ships and already grounds elsewhere (INV-6).
 */
#include "cluster_test.h"
#include "vms_cluster_codec_mscp.h"
#include "vms_cluster_codec_scs.h"

#include <string.h>

/* ---- frame assembly: three disjoint spans, three existing builders ----- */

static const uint8_t g_eth_dst[6] = { 0x08, 0x00, 0x2b, 0x11, 0x22, 0x33 };
static const uint8_t g_eth_src[6] = { 0x08, 0x00, 0x2b, 0x44, 0x55, 0x66 };

/*
 * Build a complete, classifiable frame at SCA content `content_len`: the
 * shared SCA header (abs[0,32)), the SCS application-message header
 * (abs[56,72) -- inner_len/format-word/MTYPE/credit/Con.ID pair), and
 * whatever the caller already wrote into abs[72, 14+content_len) (the
 * MSCP END body, written by the caller's own vms_mscp_*_end_build() call
 * BEFORE this function runs, since the two spans never overlap). Returns
 * the total wire length, or 0.
 */
static uint32_t stamp_applmsg_envelope(uint8_t *frame, uint32_t cap,
				       uint16_t content_len,
				       uint32_t conid_remote,
				       uint32_t conid_local, uint16_t credit)
{
	struct vms_sca_hdr sca;
	struct vms_scs_hdr sh;
	uint32_t written = 0;

	if (cap < VMS_OFF_SYSAP_BODY)
		return 0u;

	memset(&sca, 0, sizeof(sca));
	memcpy(sca.eth_dst, g_eth_dst, 6);
	memcpy(sca.eth_src, g_eth_src, 6);
	memcpy(sca.dst_lavc, g_eth_dst, 6);
	memcpy(sca.src_lavc, g_eth_src, 6);
	sca.connect_flag = 0x0001u;
	sca.sca_len_field = (uint16_t)(content_len - 2u);
	sca.word30 = (uint16_t)((uint16_t)VMS_SCS_MT_MSG |
				((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	if (vms_sca_hdr_build(&sca, frame, cap, &written) != VMS_CODEC_OK)
		return 0u;

	memset(&sh, 0, sizeof(sh));
	sh.inner_len = (uint16_t)(content_len - VMS_SCS_INNER_LEN_BIAS);
	sh.mtype = (uint16_t)VMS_SCS_CTRL_APPLICATION;
	sh.credit = credit;
	sh.conid_remote = conid_remote;
	sh.conid_local = conid_local;
	if (vms_scs_hdr_build(&sh, frame + VMS_OFF_SCSCTRL_INNERLEN,
			      cap - VMS_OFF_SCSCTRL_INNERLEN) != VMS_CODEC_OK)
		return 0u;

	return (uint32_t)VMS_ETH_HDR_LEN + content_len;
}

static struct vms_frame_info classify(const uint8_t *frame, uint32_t len)
{
	struct vms_frame_info fi;

	memset(&fi, 0, sizeof(fi));
	(void)vms_frame_classify(frame, len, &fi);
	return fi;
}

/* The "provoking command" identity: a real client->server MSCP command
 * carries (remote=SERVER's Con.ID, local=CLIENT's own) from the sender's
 * point of view (struct vms_scs_hdr's own doc: conid_remote is "the
 * DESTINATION endpoint's ... the peer's", conid_local is "OUR OWN"). The
 * server's END reply is sent the OTHER direction, so its own (remote,
 * local) pair is that SAME two values with the roles swapped -- not
 * invented here, just addressed from the other end, exactly as
 * design §3.2.7 names ("swapped by direction on established
 * connections", design-mscp-direction.md §1.1). */
#define CLIENT_CONID 0x00020007u
#define SERVER_CONID 0x00010005u

/* ---- 1: each of the five END lengths now classifies SCS_APPLMSG -------- */

struct end_case {
	const char *name;
	uint16_t    content_len;
	enum vms_mscp_class want_mscp_cls;
	uint8_t     want_frame_cls;   /* VMS_FCLS_SCS_APPLMSG or _APPLMSG94 */
};

static uint32_t build_scc_end(uint8_t *frame, uint32_t cap)
{
	struct vms_mscp_scc_end e;
	uint32_t written = 0;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x11u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0u);
	e.ctlr_flags = VMS_MSCP_SCC_CNTF_OBSERVED;
	e.ctlr_timeout = 20u;
	e.rsvd18 = VMS_MSCP_SCC_RSVD18_OBSERVED;
	if (vms_mscp_scc_end_build(&e, frame, cap, &written) != VMS_CODEC_OK)
		return 0u;
	return stamp_applmsg_envelope(frame, cap,
				      (uint16_t)VMS_MSCP_END_SCA_LEN(VMS_MSCP_SCC_END_LEN),
				      CLIENT_CONID, SERVER_CONID,
				      VMS_MSCP_ENV_CREDIT_OBSERVED);
}

static uint32_t build_read_end(uint8_t *frame, uint32_t cap)
{
	struct vms_mscp_xfer_end e;
	uint32_t written = 0;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x12u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0u);
	e.byte_count = 512u;
	if (vms_mscp_read_end_build(&e, frame, cap, &written) != VMS_CODEC_OK)
		return 0u;
	return stamp_applmsg_envelope(frame, cap,
				      (uint16_t)VMS_MSCP_END_SCA_LEN(VMS_MSCP_READ_END_LEN),
				      CLIENT_CONID, SERVER_CONID,
				      VMS_MSCP_ENV_CREDIT_OBSERVED);
}

static uint32_t build_write_end(uint8_t *frame, uint32_t cap)
{
	struct vms_mscp_xfer_end e;
	uint32_t written = 0;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x13u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0u);
	e.byte_count = 1024u;
	if (vms_mscp_write_end_build(&e, frame, cap, &written) != VMS_CODEC_OK)
		return 0u;
	return stamp_applmsg_envelope(frame, cap,
				      (uint16_t)VMS_MSCP_END_SCA_LEN(VMS_MSCP_WRITE_END_LEN),
				      CLIENT_CONID, SERVER_CONID,
				      VMS_MSCP_ENV_CREDIT_OBSERVED);
}

static uint32_t build_online_end(uint8_t *frame, uint32_t cap)
{
	struct vms_mscp_online_end e;
	uint32_t written = 0;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x14u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0u);
	e.unit_size = 100000u;
	if (vms_mscp_online_end_build(&e, frame, cap, &written) != VMS_CODEC_OK)
		return 0u;
	return stamp_applmsg_envelope(frame, cap,
				      (uint16_t)VMS_MSCP_END_SCA_LEN(VMS_MSCP_ONLINE_END_LEN),
				      CLIENT_CONID, SERVER_CONID,
				      VMS_MSCP_ENV_CREDIT_OBSERVED);
}

static uint32_t build_gus_end(uint8_t *frame, uint32_t cap)
{
	struct vms_mscp_gus_end e;
	uint32_t written = 0;

	memset(&e, 0, sizeof(e));
	e.eh.hdr.cmd_ref = 0x15u;
	e.eh.status = VMS_MSCP_STATUS(VMS_MSCP_ST_SUCCESS, 0u);
	e.shadow_unit = 3u;
	if (vms_mscp_gus_end_build(&e, frame, cap, &written) != VMS_CODEC_OK)
		return 0u;
	return stamp_applmsg_envelope(frame, cap,
				      (uint16_t)VMS_MSCP_END_SCA_LEN(VMS_MSCP_GUS_END_LEN),
				      CLIENT_CONID, SERVER_CONID,
				      VMS_MSCP_ENV_CREDIT_OBSERVED);
}

static void check_one_end(const char *name, uint32_t (*build)(uint8_t *, uint32_t),
			  enum vms_mscp_class want_mscp_cls,
			  uint8_t want_frame_cls)
{
	uint8_t frame[256];
	uint32_t len;
	struct vms_frame_info fi;
	uint32_t remote = 0, local = 0;
	enum vms_mscp_class mcls = VMS_MSCP_CLS_UNKNOWN;
	char what[160];

	memset(frame, 0xAA, sizeof(frame));
	len = build(frame, sizeof(frame));
	snprintf(what, sizeof(what), "%s: builds", name);
	ct_check(len != 0u, what);
	if (len == 0u)
		return;

	fi = classify(frame, len);
	snprintf(what, sizeof(what), "%s: classifies %s", name,
		 want_frame_cls == VMS_FCLS_SCS_APPLMSG ? "SCS_APPLMSG"
							: "SCS_APPLMSG94");
	ct_check(fi.cls == want_frame_cls, what);
	snprintf(what, sizeof(what), "%s: grants VMS_FCAP_CONID", name);
	ct_check((fi.caps & VMS_FCAP_CONID) != 0u, what);

	snprintf(what, sizeof(what),
		 "%s: Con.ID pair is the provoking command's pair REVERSED",
		 name);
	ct_check(vms_scs_conid(frame, len, &fi, &remote, &local) == VMS_CODEC_OK &&
			 remote == CLIENT_CONID && local == SERVER_CONID,
		 what);

	/* mscp_seq_ok() (vms_cluster_codec_mscp.c) must accept whichever of
	 * the two classes this frame really landed in, so the REAL wire frame
	 * -- not just the client's own all-zero splice reconstruction --
	 * resolves through vms_mscp_classify() too. */
	(void)vms_mscp_classify(frame, len, &fi, &mcls);
	snprintf(what, sizeof(what), "%s: vms_mscp_classify resolves it on "
		 "the REAL frame (mscp_seq_ok widened)", name);
	ct_check(mcls == want_mscp_cls, what);
}

static void test_five_end_lengths(void)
{
	printf("-- the five MSCP END lengths: 86/90/102/110 now classify "
	      "SCS_APPLMSG, 94 (WRITE END) still SCS_APPLMSG94\n");
	check_one_end("SCC END (86)", build_scc_end, VMS_MSCP_CLS_SCC_END,
		      VMS_FCLS_SCS_APPLMSG);
	check_one_end("READ END (90)", build_read_end, VMS_MSCP_CLS_READ_END,
		      VMS_FCLS_SCS_APPLMSG);
	check_one_end("ONLINE END (102)", build_online_end,
		      VMS_MSCP_CLS_ONLINE_END, VMS_FCLS_SCS_APPLMSG);
	check_one_end("GUS END (110)", build_gus_end, VMS_MSCP_CLS_GUS_END,
		      VMS_FCLS_SCS_APPLMSG);
	/* the frozen-table no-regression alias: 94 stays on the ORIGINAL
	 * FC-P2.1b row, never migrates to the new generic one. */
	check_one_end("WRITE END (94)", build_write_end, VMS_MSCP_CLS_WRITE_END,
		      VMS_FCLS_SCS_APPLMSG94);
}

/* ---- 2: the two no-regression proofs the ordering exists for ----------- */

/*
 * The 190-content VMS$VAXcluster/DLM class (VMS_FCLS_SCS_MSG). OVMX's own
 * shipping vms_scs_fsm.c msg_transmit_long() stamps this SAME fmtword=4/
 * MTYPE=10/self-consistent-inner-length envelope on EVERY 190-content
 * frame it sends -- so this is not a hypothetical: without the ordering
 * design §3.2.7 mandates (checked AFTER VMS_FCLS_SCS_MSG), this exact,
 * real, shipping frame shape would misclassify into the new class and
 * dlm_class_ok()/vms_cluster_codec_cm.c (which key on VMS_FCLS_SCS_MSG
 * alone) would refuse every DLM/CM message this node sends.
 */
static void test_190_content_still_scs_msg(void)
{
	uint8_t frame[256];
	uint32_t len;
	struct vms_frame_info fi;

	printf("-- no-regression: a 190-content frame carrying the SAME "
	      "fmtword/MTYPE/inner-length envelope OVMX's own CM/DLM send "
	      "path stamps still classifies SCS_MSG, not the new class\n");
	memset(frame, 0, sizeof(frame));
	len = stamp_applmsg_envelope(frame, sizeof(frame), 190u,
				     CLIENT_CONID, SERVER_CONID,
				     VMS_MSCP_ENV_CREDIT_OBSERVED);
	ct_check(len != 0u, "190-content envelope builds");
	fi = classify(frame, len);
	ct_check(fi.cls == VMS_FCLS_SCS_MSG,
		 "still VMS_FCLS_SCS_MSG (dlm_class_ok()/cm.c's own gate holds)");
	ct_check((fi.caps & VMS_FCAP_CONID) != 0u,
		 "...and still grants VMS_FCAP_CONID, exactly as before");
}

/*
 * A block-transfer frame (SS4(d)/(e)): sequenced, 0x4b/0x13, but its abs 56
 * onward is a 28-byte block header, NOT this envelope -- it fails the
 * format-word test (design-mscp-direction.md: "it deliberately FAILS the
 * SCS envelope conformance test (content[44:46] == 0x0004)"). Content 302
 * is one of spec Table 2's block-transfer lengths.
 */
static void test_block_transfer_unaffected(void)
{
	struct vms_sca_hdr sca;
	uint8_t frame[400];
	uint32_t written = 0;
	struct vms_frame_info fi;

	printf("-- no-regression: a block-transfer frame (fails the "
	      "format-word test) still classifies SCS_SEQ, no Con.ID\n");
	memset(frame, 0, sizeof(frame));
	memset(&sca, 0, sizeof(sca));
	memcpy(sca.eth_dst, g_eth_dst, 6);
	memcpy(sca.eth_src, g_eth_src, 6);
	memcpy(sca.dst_lavc, g_eth_dst, 6);
	memcpy(sca.src_lavc, g_eth_src, 6);
	sca.connect_flag = 0x0001u;
	sca.sca_len_field = (uint16_t)(302u - 2u);
	sca.word30 = (uint16_t)((uint16_t)VMS_SCS_MT_MSG |
				((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	ct_check(vms_sca_hdr_build(&sca, frame, sizeof(frame), &written) ==
			 VMS_CODEC_OK,
		 "block-transfer envelope builds");
	/* abs[32,...) -- including abs 56/58/60, this item's own new fields
	 * -- is left an honest zero: no format word, no MTYPE 10. */

	fi = classify(frame, (uint32_t)VMS_ETH_HDR_LEN + 302u);
	ct_check(fi.cls == VMS_FCLS_SCS_SEQ,
		 "still VMS_FCLS_SCS_SEQ, unchanged by this item's widen");
	ct_check((fi.caps & VMS_FCAP_CONID) == 0u,
		 "...and still carries NO Con.ID capability");
}

int main(void)
{
	printf("test_codec_applmsg: length-generic SCS_APPLMSG classify "
	      "widen (FC-P2.7, E48)\n");
	test_five_end_lengths();
	test_190_content_still_scs_msg();
	test_block_transfer_unaffected();
	return ct_summary("test_codec_applmsg");
}
