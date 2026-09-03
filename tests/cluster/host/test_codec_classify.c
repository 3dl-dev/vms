// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_classify.c - the frame-class registry (FC-P0.6, rung R1).
 *
 * Two jobs:
 *   1. every specimen classifies as the class its provenance header declares;
 *   2. the CAPABILITY GATE holds -- a field accessor refuses on a class the
 *      spec does not ground it for. That second half is the INV-6 mechanism:
 *      the spec's own §4(d) says the non-190 length classes "do not reliably
 *      match this layout", so asking a HELLO or a bulk-transfer frame for a
 *      Con.ID must return E_CLASS, never four bytes.
 */

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec.h"
#include "vms_cluster_codec_scs.h"

#include <string.h>

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (!strcmp(g_fx[i].name, name))
			return &g_fx[i];
	}
	return NULL;
}

static int classify(const struct vms_fixture *f, struct vms_frame_info *fi)
{
	return (int)vms_frame_classify(f->bytes, f->wire_len, fi);
}

/* ---- 1. every specimen lands in its declared class ---------------- */

static void test_specimen_classes(void)
{
	int i;

	printf("-- each specimen classifies as its declared class\n");
	for (i = 0; i < g_n; i++) {
		const struct vms_frame_class_info *ci;
		struct vms_frame_info fi;
		char what[192];

		(void)classify(&g_fx[i], &fi);
		ci = vms_frame_class_lookup(fi.cls);
		snprintf(what, sizeof(what), "%s -> class '%s' (declared '%s')",
			 g_fx[i].name, ci ? ci->name : "?", g_fx[i].class_name);
		ct_check(ci && !strcmp(ci->name, g_fx[i].class_name), what);
	}
}

/*
 * VMS_FCLS_SCS_APPLMSG (FC-P2.7, design §3.2.7, E48) has no `.spec` fixture:
 * every real specimen of it (the MSCP END lengths 86/90/102/110) traces to
 * the vms291 lab-2 mount capture, which is a HOST-ONLY pcap artifact never
 * committed and not in the clean-room manifest (docs/design-mscp-direction.md
 * "Host-only artifacts, never in git") -- citing it in a fixture's `capture:`
 * field would be false provenance (Rule 8). This builds the SAME shape
 * through the shipping, GROUNDED vms_sca_hdr_build()/vms_scs_hdr_build()
 * entries instead (see tests/cluster/host/test_codec_applmsg.c, this item's
 * own dedicated R1 test, for the full five-length proof against the real
 * MSCP end-message builders); it exists here only so this coverage loop
 * still exercises every registered class.
 */
static int code_composed_applmsg_specimen(uint8_t *frame, uint32_t cap,
					  uint32_t *len)
{
	static const uint8_t dst[6] = { 0x08, 0x00, 0x2b, 0x77, 0x88, 0x99 };
	static const uint8_t src[6] = { 0x08, 0x00, 0x2b, 0xaa, 0xbb, 0xcc };
	struct vms_sca_hdr sca;
	struct vms_scs_hdr sh;
	uint16_t content = 86u;   /* the SCC-END length -- any non-94/190 works */
	uint32_t written = 0;

	memset(frame, 0, cap);
	memset(&sca, 0, sizeof(sca));
	memcpy(sca.eth_dst, dst, 6);
	memcpy(sca.eth_src, src, 6);
	memcpy(sca.dst_lavc, dst, 6);
	memcpy(sca.src_lavc, src, 6);
	sca.connect_flag = 0x0001u;
	sca.sca_len_field = (uint16_t)(content - 2u);
	sca.word30 = (uint16_t)((uint16_t)VMS_SCS_MT_MSG |
				((uint16_t)VMS_SCS_FORMAT_V13 << 8));
	if (vms_sca_hdr_build(&sca, frame, cap, &written) != VMS_CODEC_OK)
		return 0;

	memset(&sh, 0, sizeof(sh));
	sh.inner_len = (uint16_t)(content - VMS_SCS_INNER_LEN_BIAS);
	sh.mtype = (uint16_t)VMS_SCS_CTRL_APPLICATION;
	sh.conid_remote = 0x00020007u;
	sh.conid_local = 0x00010005u;
	if (vms_scs_hdr_build(&sh, frame + VMS_OFF_SCSCTRL_INNERLEN,
			      cap - VMS_OFF_SCSCTRL_INNERLEN) != VMS_CODEC_OK)
		return 0;

	*len = (uint32_t)VMS_ETH_HDR_LEN + content;
	return 1;
}

/* ---- 2. every registered class has at least one specimen ---------- */

static void test_class_coverage(void)
{
	uint8_t cls;

	printf("-- every registered class has a specimen\n");
	for (cls = VMS_FCLS_UNKNOWN + 1; cls < VMS_FCLS__COUNT; cls++) {
		const struct vms_frame_class_info *ci =
			vms_frame_class_lookup(cls);
		char what[128];
		int found = 0, i;

		if (!ci)
			continue;
		for (i = 0; i < g_n && !found; i++) {
			struct vms_frame_info fi;

			(void)classify(&g_fx[i], &fi);
			found = (fi.cls == cls);
		}
		if (!found && cls == VMS_FCLS_SCS_APPLMSG) {
			uint8_t frame[128];
			uint32_t len = 0;
			struct vms_frame_info fi;

			if (code_composed_applmsg_specimen(frame, sizeof(frame),
							   &len)) {
				(void)vms_frame_classify(frame, len, &fi);
				found = (fi.cls == cls);
			}
		}
		snprintf(what, sizeof(what),
			 "class '%s' (%s) has >= 1 specimen", ci->name, ci->spec);
		ct_check(found, what);
	}
}

/* ---- 3. the sec 2 length identity, both arms ---------------------- */

static void test_length_identity(void)
{
	const struct vms_fixture *hello = fixture("hello-multicast-vax1");
	const struct vms_fixture *credit = fixture("scs-credit-return-short");
	const struct vms_fixture *ack = fixture("scs-start-ack-round2");
	struct vms_frame_info fi;

	printf("-- SCA length identity (spec sec 2) and the runt-pad rule\n");
	(void)classify(hello, &fi);
	ct_check_eq_u32(fi.sca_content, 120, "HELLO SCA content == 120");
	ct_check(fi.len_check == VMS_SCA_LEN_EXACT, "HELLO length is EXACT");

	(void)classify(ack, &fi);
	ct_check_eq_u32(fi.sca_content, 46, "START ACK content == 46");
	ct_check(fi.len_check == VMS_SCA_LEN_EXACT,
		 "46 + 14 == 60 exactly, so EXACT and NOT runt-padded");

	(void)classify(credit, &fi);
	ct_check_eq_u32(fi.sca_content, 41, "credit short content == 41");
	ct_check(fi.len_check == VMS_SCA_LEN_RUNT_PAD,
		 "41 + 14 == 55 padded to 60: RUNT_PAD, spec sec 2's 928 residuals");

	ct_check(vms_sca_len_check(190, 100) == VMS_SCA_LEN_MISMATCH,
		 "a frame shorter than its own length claim is a MISMATCH");
	ct_check(vms_sca_len_check(190, 300) == VMS_SCA_LEN_MISMATCH,
		 "an over-long non-60-byte frame is a MISMATCH, not a pad");
}

/* ---- 4. negative controls ---------------------------------------- */

static void test_negative_controls(void)
{
	const struct vms_fixture *notsca = fixture("neg-not-sca-ethertype");
	const struct vms_fixture *trunc = fixture("neg-truncated-sca");
	struct vms_frame_info fi;

	printf("-- negative controls\n");
	ct_check(classify(notsca, &fi) == VMS_CODEC_E_NOTSCA,
		 "non-0x6007 ethertype -> E_NOTSCA");
	ct_check(fi.cls == VMS_FCLS_UNKNOWN, "  and class UNKNOWN");
	ct_check(fi.caps == 0, "  and no capabilities");

	ct_check(classify(trunc, &fi) == VMS_CODEC_E_SHORT,
		 "truncated SCA frame -> E_SHORT");
	ct_check(fi.cls == VMS_FCLS_UNKNOWN, "  and class UNKNOWN");

	ct_check(vms_frame_classify(NULL, 100, &fi) == VMS_CODEC_E_INVAL,
		 "NULL frame -> E_INVAL");
}

/* ---- 5. the capability gate -------------------------------------- */

static void check_conid(const char *fixture_name, int expect_ok,
			uint32_t want_remote, uint32_t want_local)
{
	const struct vms_fixture *f = fixture(fixture_name);
	struct vms_frame_info fi;
	uint32_t remote = 0xdeadbeefu, local = 0xdeadbeefu;
	vms_codec_status_t st;
	char what[192];

	(void)classify(f, &fi);
	st = vms_scs_conid(f->bytes, f->wire_len, &fi, &remote, &local);
	if (expect_ok) {
		snprintf(what, sizeof(what), "%s: Con.ID pair readable", fixture_name);
		ct_check(st == VMS_CODEC_OK, what);
		ct_check_eq_u32(remote, want_remote, "  remote Con.ID");
		ct_check_eq_u32(local, want_local, "  local Con.ID");
	} else {
		snprintf(what, sizeof(what),
			 "%s: Con.ID REFUSED (E_CLASS), not read from the offset",
			 fixture_name);
		ct_check(st == VMS_CODEC_E_CLASS, what);
		ct_check(remote == 0xdeadbeefu && local == 0xdeadbeefu,
			 "  and the caller's variables were left untouched");
	}
}

static void test_capability_gate(void)
{
	const struct vms_fixture *hello = fixture("hello-directed-vax2-to-vax1");
	const struct vms_fixture *msg = fixture("scs-msg190-vaxcluster-cat01-op14");
	struct vms_frame_info fi;
	uint8_t mt = 0xff, fmt = 0xff, word = 0xff, dcls = 0xff;
	uint16_t ack = 0xffff, seq = 0xffff, ctrl = 0xffff;

	printf("-- capability gate: grounded fields only\n");

	/* Con.ID: GROUNDED for the 190 class and connection-control only. */
	check_conid("scs-msg190-vaxcluster-cat01-op14", 1,
		    0x62c50009u, 0x33580008u);
	check_conid("scs-connect-request-vaxcluster", 1, 0u, 0x62c50009u);
	check_conid("hello-directed-vax2-to-vax1", 0, 0, 0);
	check_conid("scs-seq-bulk-block-transfer", 0, 0, 0);
	check_conid("solicit-vax3-satellite-boot", 0, 0, 0);

	/* abs 30 means two different things; each family gets its own reader. */
	(void)classify(hello, &fi);
	ct_check(vms_sca_chan_word(hello->bytes, hello->wire_len, &fi, &word)
		 == VMS_CODEC_OK, "HELLO: channel-verify word readable");
	ct_check_eq_u32(word, 0xb2, "  abs 30 low byte == b2 request (sec 4a.1)");
	ct_check(vms_scs_msgtype(hello->bytes, hello->wire_len, &fi, &mt, &fmt)
		 == VMS_CODEC_E_CLASS,
		 "HELLO: SCS msgtype REFUSED (abs 30 is not a msgtype here)");
	ct_check(vms_sca_disc_class(hello->bytes, hello->wire_len, &fi, &dcls)
		 == VMS_CODEC_OK, "HELLO: discovery class byte readable");
	ct_check_eq_u32(dcls, VMS_DISC_CLASS_HELLO, "  abs 36 == 0x05");

	(void)classify(msg, &fi);
	ct_check(vms_scs_msgtype(msg->bytes, msg->wire_len, &fi, &mt, &fmt)
		 == VMS_CODEC_OK, "190-class: msgtype readable");
	ct_check_eq_u32(mt, VMS_SCS_MT_MSG, "  msgtype 0x4b");
	ct_check_eq_u32(fmt, VMS_SCS_FORMAT_V13, "  format 0x13");
	ct_check(vms_sca_chan_word(msg->bytes, msg->wire_len, &fi, &word)
		 == VMS_CODEC_E_CLASS,
		 "190-class: channel-verify word REFUSED");
	ct_check(vms_sca_disc_class(msg->bytes, msg->wire_len, &fi, &dcls)
		 == VMS_CODEC_E_CLASS,
		 "190-class: discovery class byte REFUSED");
	ct_check(vms_scs_seq(msg->bytes, msg->wire_len, &fi, &ack, &seq)
		 == VMS_CODEC_OK, "190-class: recv_ack/send_seq readable");
	ct_check(vms_scs_ctrl_type(msg->bytes, msg->wire_len, &fi, &ctrl)
		 == VMS_CODEC_OK,
		 "190-class: connection-control word readable (CONID class)");
}

/* ---- 6. grounded field values ------------------------------------ */

static void test_grounded_values(void)
{
	const struct vms_fixture *start = fixture("scs-start-vax2-config-round0");
	const struct vms_fixture *credit = fixture("scs-credit-return-short");
	const struct vms_fixture *cc = fixture("scs-connect-request-vaxcluster");
	struct vms_frame_info fi;
	uint16_t ack = 0, seq = 0, ctrl = 0;

	printf("-- grounded field values through the typed accessors\n");

	(void)classify(start, &fi);
	ct_check(vms_scs_seq(start->bytes, start->wire_len, &fi, &ack, &seq)
		 == VMS_CODEC_OK, "START: seq pair readable");
	ct_check_eq_u32(seq, 1, "  send_seq == 1 on a fresh join (sec 4g/4h(4))");

	(void)classify(credit, &fi);
	ct_check(vms_scs_seq(credit->bytes, credit->wire_len, &fi, &ack, &seq)
		 == VMS_CODEC_OK, "credit short: seq pair readable");
	ct_check_eq_u32(seq, 0,
			"  send_seq == 0: a credit return emits no new sequence "
			"(GROUNDED 622/622)");
	ct_check_eq_u32(ack, 5, "  recv_ack carries the acknowledged sequence");

	(void)classify(cc, &fi);
	ct_check(vms_scs_ctrl_type(cc->bytes, cc->wire_len, &fi, &ctrl)
		 == VMS_CODEC_OK, "connect frame: control type readable");
	ct_check_eq_u32(ctrl, VMS_SCS_CTRL_CONNECT_REQ,
			"  == CONNECT_REQ (sec 4h(1a))");
}

/* ---- 7. registry table integrity --------------------------------- */

static void test_registry_integrity(void)
{
	uint8_t cls;

	printf("-- registry table integrity\n");
	for (cls = VMS_FCLS_UNKNOWN + 1; cls < VMS_FCLS__COUNT; cls++) {
		const struct vms_frame_class_info *ci =
			vms_frame_class_lookup(cls);
		char what[128];

		snprintf(what, sizeof(what), "class %u is registered", cls);
		ct_check(ci != NULL, what);
		if (!ci)
			continue;
		snprintf(what, sizeof(what), "class '%s' cites the spec", ci->name);
		ct_check(ci->spec && ci->spec[0] && strcmp(ci->spec, "-"), what);
		snprintf(what, sizeof(what),
			 "class '%s' harvest_len %u <= min_len %u",
			 ci->name, ci->harvest_len, ci->min_len);
		ct_check(ci->harvest_len <= ci->min_len, what);
		snprintf(what, sizeof(what),
			 "class '%s' round-trips by name", ci->name);
		ct_check(vms_frame_class_by_name(ci->name) == ci, what);
	}
	ct_check(vms_frame_class_by_name("no-such-class") == NULL,
		 "an unknown class name resolves to NULL");
	ct_check(vms_frame_class_lookup(VMS_FCLS_UNKNOWN) != NULL,
		 "UNKNOWN is itself a lookupable row");
	ct_check(vms_frame_class_lookup(VMS_FCLS__COUNT) == NULL,
		 "an out-of-range class is NULL");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_classify: frame-class registry (FC-P0.6)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}
	printf("  (%d specimens loaded)\n", g_n);
	test_specimen_classes();
	test_class_coverage();
	test_length_identity();
	test_negative_controls();
	test_capability_gate();
	test_grounded_values();
	test_registry_integrity();
	return ct_summary("test_codec_classify");
}
