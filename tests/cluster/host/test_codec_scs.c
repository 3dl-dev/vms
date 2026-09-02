// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_scs.c - SCS connection/directory codec entries, rung R1
 * (FC-P2.1). Ported assertions: test_scs_connect / test_scs_dir from
 * tests/vmsscs/ (the strawman's harvested wire knowledge, not its
 * orchestration -- design docs/design-faithful-cluster-executive.md, "the
 * userspace src/vmsscs/ stack is a non-portable STRAWMAN: harvest its wire
 * knowledge as spec, discard its orchestration").
 *
 * Four groups:
 *   1. Fixture round trip: parse each op 0/1/2/3/5/6/7/10 specimen into
 *      `struct vms_scs_ctrl_frame`, build back from ONLY the typed struct,
 *      assert every CITED byte reproduces exactly -- same discipline as
 *      test_codec_hello.c group 1.
 *   2. The directory-lookup semantic layer (sec 4(h)(2)) on top of the
 *      generic op-10 parse: EMPTY (request), NOT_PRESENT (negative
 *      response, ported from src/vmsscs/scs_dir.c's "NOT PRESENT HERE"
 *      literal), and AFFIRMATIVE (hand-built with the real SCA#38
 *      descriptor bytes -- src/vmsscs/scs_dir.c's dir_affirmative_result,
 *      "internal semantics NOT grounded", sec 4h RE gap (c)).
 *   3. Hand-built structural coverage for op 4 (REJECT_REQ) and op 8/9
 *      (CREDIT_REQ/CREDIT_RSP): no single-frame byte-exact specimen exists
 *      in the corpus for these (sec 4(h)(1c)/(1f) are CENSUS findings, not
 *      one captured frame), so these assert the GROUNDED structural
 *      invariants only -- the census values, not a fabricated frame --
 *      exactly as tests/cluster/host/fixtures/scs-credit.spec's own
 *      precedent ("the unit test asserts those invariants, not the
 *      number").
 *   4. Error paths and the missing op-verb constants (5/7/8/9).
 */

#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_scs.h"

#include <string.h>

static struct vms_fixture g_fx[VMS_FIXTURE_MAX_FILES];
static int g_n;

static const struct vms_fixture *fixture(const char *name)
{
	int i;

	for (i = 0; i < g_n; i++) {
		if (strcmp(g_fx[i].name, name) == 0)
			return &g_fx[i];
	}
	return NULL;
}

/* ---- group 1: fixture round trip, cited bytes only ------------------- */

static void assert_cited_bytes_match(const struct vms_fixture *f,
				     const uint8_t *built, uint32_t n,
				     const char *label)
{
	uint32_t i, checked = 0, mismatches = 0;
	char what[224];

	for (i = 0; i < n; i++) {
		if (!vms_fixture_is_cited(f, i, 1))
			continue;
		checked++;
		if (built[i] != f->bytes[i]) {
			if (mismatches == 0)
				printf("       %s: first cited mismatch at abs "
				       "%u: built %02x, specimen %02x\n",
				       label, i, built[i], f->bytes[i]);
			mismatches++;
		}
	}
	snprintf(what, sizeof(what),
		 "%s: every CITED byte in [0,%u) is byte-exact (%u checked)",
		 label, n, checked);
	ct_check(mismatches == 0 && checked > 0, what);
}

static void roundtrip_ctrl(const char *fname, uint16_t want_op)
{
	const struct vms_fixture *f = fixture(fname);
	struct vms_frame_info fi;
	struct vms_scs_ctrl_frame cf;
	uint8_t built[256];
	uint32_t written = 0;
	char what[192];

	ct_check(f != NULL, fname);
	if (f == NULL) {
		printf("       (fixture missing -- corpus regression)\n");
		return;
	}

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");

	snprintf(what, sizeof(what), "%s: parses as a connection-control frame",
		 fname);
	ct_check(vms_scs_ctrl_parse(f->bytes, f->wire_len, &fi, &cf)
		 == VMS_CODEC_OK, what);
	snprintf(what, sizeof(what), "%s: op == %u", fname, want_op);
	ct_check_eq_u32(cf.op, want_op, what);

	memset(built, 0xAA, sizeof(built));
	snprintf(what, sizeof(what), "%s: builds from the typed struct alone",
		 fname);
	ct_check(vms_scs_ctrl_build(&cf, built, sizeof(built), &written)
		 == VMS_CODEC_OK, what);
	ct_check_eq_u32(written, f->wire_len, "  build wrote the full on-wire length");

	assert_cited_bytes_match(f, built, f->wire_len, fname);
}

static void test_fixture_roundtrips(void)
{
	printf("-- fixture round trip: parse -> typed fields -> build\n");
	roundtrip_ctrl("scs-dir-connect-request", VMS_SCS_CTRL_CONNECT_REQ);
	roundtrip_ctrl("scs-dir-connect-echo", VMS_SCS_CTRL_CONNECT_RSP);
	roundtrip_ctrl("scs-dir-connect-response", VMS_SCS_CTRL_ACCEPT_REQ);
	roundtrip_ctrl("scs-dir-connect-confirm", VMS_SCS_CTRL_ACCEPT_RSP);
	roundtrip_ctrl("scs-reject-response", VMS_SCS_CTRL_REJECT_RSP);
	roundtrip_ctrl("scs-disc-request", VMS_SCS_CTRL_DISCONNECT_REQ);
	roundtrip_ctrl("scs-disc-response", VMS_SCS_CTRL_DISCONNECT_RSP);
	roundtrip_ctrl("scs-dir-lookup-request", VMS_SCS_CTRL_APPLICATION);
	roundtrip_ctrl("scs-dir-lookup-response-negative", VMS_SCS_CTRL_APPLICATION);
}

/* ---- group 2: the directory-lookup semantic layer --------------------- */

static void test_dir_lookup_request_is_empty(void)
{
	const struct vms_fixture *f = fixture("scs-dir-lookup-request");
	struct vms_frame_info fi;
	struct vms_scs_ctrl_frame cf;
	struct vms_scs_dir_lookup dl;

	printf("-- directory lookup: REQUEST reads as EMPTY result\n");
	ct_check(f != NULL, "fixture present");
	if (f == NULL)
		return;
	(void)vms_frame_classify(f->bytes, f->wire_len, &fi);
	ct_check(vms_scs_ctrl_parse(f->bytes, f->wire_len, &fi, &cf)
		 == VMS_CODEC_OK, "ctrl frame parses");
	ct_check(vms_scs_dir_lookup_parse(&cf, &dl) == VMS_CODEC_OK,
		 "directory-lookup view parses");
	ct_check(memcmp(dl.queried_name, "MSCP$TAPE       ", 16) == 0,
		 "  queried_name == 'MSCP$TAPE       '");
	ct_check_eq_u32(dl.result_kind, VMS_SCS_DIR_RESULT_EMPTY,
			"  result_kind == EMPTY (a request carries no result)");
}

static void test_dir_lookup_response_not_present(void)
{
	const struct vms_fixture *f = fixture("scs-dir-lookup-response-negative");
	struct vms_frame_info fi;
	struct vms_scs_ctrl_frame cf;
	struct vms_scs_dir_lookup dl;

	printf("-- directory lookup: negative RESPONSE reads as NOT_PRESENT\n");
	ct_check(f != NULL, "fixture present");
	if (f == NULL)
		return;
	(void)vms_frame_classify(f->bytes, f->wire_len, &fi);
	ct_check(vms_scs_ctrl_parse(f->bytes, f->wire_len, &fi, &cf)
		 == VMS_CODEC_OK, "ctrl frame parses");
	ct_check(vms_scs_dir_lookup_parse(&cf, &dl) == VMS_CODEC_OK,
		 "directory-lookup view parses");
	ct_check(memcmp(dl.queried_name, "MSCP$TAPE       ", 16) == 0,
		 "  queried_name echoed == 'MSCP$TAPE       '");
	ct_check_eq_u32(dl.result_kind, VMS_SCS_DIR_RESULT_NOT_PRESENT,
			"  result_kind == NOT_PRESENT (GROUNDED literal)");
	ct_check(memcmp(dl.result, vms_scs_dir_not_present_here, 16) == 0,
		 "  result bytes == 'NOT PRESENT HERE'");
}

/*
 * The AFFIRMATIVE case: real descriptor bytes, byte-exact to
 * src/vmsscs/scs_dir.c's `dir_affirmative_result` (cited SCA#38, "94-byte
 * 0x4b, VMS$VAXcluster resolved"; internal semantics NOT grounded -- sec
 * 4h RE gap (c)). No corpus fixture carries this exact frame end to end
 * (the affirmative case is not independently in the manifest-hashed
 * captures this item can cite a full frame from), so this is hand-built
 * exactly like test_codec_hello.c's group 2 -- every OTHER field (Con.ID
 * pair, sequence counters, addresses) is ordinary caller-supplied data,
 * never asserted as itself grounded.
 */
static void test_dir_lookup_affirmative_roundtrip(void)
{
	static const uint8_t affirmative[16] = {
		0x01, 0x1b, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00
	};
	struct vms_scs_ctrl_frame cf, cf2;
	struct vms_scs_dir_lookup dl, dl2;
	struct vms_frame_info fi;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- directory lookup: AFFIRMATIVE result round-trips (SCA#38 "
	       "descriptor, sec 4h RE gap (c))\n");

	memset(&dl, 0, sizeof(dl));
	memcpy(dl.queried_name, "VMS$VAXcluster  ", 16);
	dl.result_kind = VMS_SCS_DIR_RESULT_AFFIRMATIVE;
	memcpy(dl.result, affirmative, 16);

	memset(&cf, 0, sizeof(cf));
	memcpy(cf.hdr.eth_dst, "\x08\x00\x2b\x4a\xb7\x15", 6);
	memcpy(cf.hdr.eth_src, "\x08\x00\x2b\x78\x56\xb9", 6);
	memcpy(cf.hdr.dst_lavc, "\xaa\x00\x04\x00\x01\x04", 6);
	memcpy(cf.hdr.src_lavc, "\xaa\x00\x04\x00\x02\x04", 6);
	cf.hdr.connect_flag = 0x0001;
	cf.hdr.word30 = 0x134bu; /* abs30 msgtype 0x4b, abs31 format 0x13 */
	cf.hdr.sca_len_field = VMS_SCSCTRL_LEN_LOOKUP - 2u;
	cf.recv_ack = 5;
	cf.send_seq = 5;
	cf.incarn = 1;
	cf.lan_ovrhd = 0x0012;
	cf.tail_const1 = 0x0001;
	cf.tail_const2 = 0x0200;
	cf.inner_len = VMS_SCSCTRL_LEN_LOOKUP - 44u;
	cf.op = VMS_SCS_CTRL_APPLICATION;
	cf.credit = 1;
	cf.conid_remote = 0x62c50009u;
	cf.conid_local = 0x33580008u;
	cf.has_marker = 1;
	memcpy(cf.marker, "\x01\x00\x00\x00", 4);

	ct_check(vms_scs_dir_lookup_build(&dl, &cf) == VMS_CODEC_OK,
		 "vms_scs_dir_lookup_build succeeds");
	ct_check(cf.has_names == 1 && cf.has_blank == 0,
		 "  stamps has_names, clears has_blank");
	ct_check(memcmp(cf.name1, "VMS$VAXcluster  ", 16) == 0,
		 "  name1 == the queried name");
	ct_check(memcmp(cf.name2, affirmative, 16) == 0,
		 "  name2 == the SCA#38 affirmative descriptor bytes");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_scs_ctrl_build(&cf, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "the stamped frame builds");
	ct_check_eq_u32(written, VMS_ETH_HDR_LEN + VMS_SCSCTRL_LEN_LOOKUP,
			"  writes the 94-content frame length");

	ct_check(vms_frame_classify(built, written, &fi) == VMS_CODEC_OK,
		 "the built frame classifies");
	ct_check(vms_scs_ctrl_parse(built, written, &fi, &cf2) == VMS_CODEC_OK,
		 "the built frame re-parses");
	ct_check(vms_scs_dir_lookup_parse(&cf2, &dl2) == VMS_CODEC_OK,
		 "re-parses as a directory lookup");
	ct_check_eq_u32(dl2.result_kind, VMS_SCS_DIR_RESULT_AFFIRMATIVE,
			"  result_kind == AFFIRMATIVE");
	ct_check(memcmp(dl2.result, affirmative, 16) == 0,
		 "  descriptor bytes survive the round trip unchanged "
		 "(never reinterpreted)");
}

/* A 110-content CONNECT frame's name pair is NOT a lookup result. */
static void test_dir_lookup_refuses_connect_shape(void)
{
	const struct vms_fixture *f = fixture("scs-dir-connect-request");
	struct vms_frame_info fi;
	struct vms_scs_ctrl_frame cf;
	struct vms_scs_dir_lookup dl;

	printf("-- directory-lookup view REFUSES a 110-content CONNECT frame\n");
	ct_check(f != NULL, "fixture present");
	if (f == NULL)
		return;
	(void)vms_frame_classify(f->bytes, f->wire_len, &fi);
	ct_check(vms_scs_ctrl_parse(f->bytes, f->wire_len, &fi, &cf)
		 == VMS_CODEC_OK, "ctrl frame parses");
	ct_check(vms_scs_dir_lookup_parse(&cf, &dl) == VMS_CODEC_E_CLASS,
		 "  E_CLASS: name2 here is a SYSAP name, not a lookup result");
}

/* ---- group 3: hand-built structural coverage, op 4 / 8 / 9 ------------ */

/*
 * op 4 REJECT_REQ: GROUNDED structural invariants only (sec 4(h)(1a)
 * CENSUS-A, 453/453 real-VAX frames): 62-byte content, marker [58:60]==0,
 * [60:62]==1. No single frame is byte-exact-cited; this asserts the
 * invariants, not a fabricated specimen -- the same discipline
 * tests/cluster/host/fixtures/scs-credit.spec documents for its own
 * unlocated field.
 */
static void test_reject_req_structural(void)
{
	struct vms_scs_ctrl_frame cf, cf2;
	struct vms_frame_info fi;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- op 4 REJECT_REQ: GROUNDED structural invariants (sec "
	       "4h(1a) CENSUS-A, 453/453)\n");

	memset(&cf, 0, sizeof(cf));
	memcpy(cf.hdr.eth_dst, "\x08\x00\x2b\x78\x56\xb9", 6);
	memcpy(cf.hdr.eth_src, "\x08\x00\x2b\x4a\xb7\x15", 6);
	memcpy(cf.hdr.dst_lavc, "\xaa\x00\x04\x00\x02\x04", 6);
	memcpy(cf.hdr.src_lavc, "\xaa\x00\x04\x00\x01\x04", 6);
	cf.hdr.connect_flag = 0x0001;
	cf.hdr.word30 = 0x134bu; /* abs30 msgtype 0x4b, abs31 format 0x13 */
	cf.hdr.sca_len_field = VMS_SCSCTRL_LEN_MARKER - 2u;
	cf.recv_ack = 9;
	cf.send_seq = 9;
	cf.incarn = 1;
	cf.lan_ovrhd = 0x0012;
	cf.tail_const1 = 0x0001;
	cf.tail_const2 = 0x0200;
	cf.inner_len = VMS_SCSCTRL_LEN_MARKER - 44u;
	cf.op = VMS_SCS_CTRL_REJECT_REQ;
	cf.credit = 0;
	cf.conid_remote = 0x33580008u;
	cf.conid_local = 0x62c50009u;
	cf.has_marker = 1;
	/* GROUNDED (CENSUS-A): [58:60]==0x0000, [60:62]==0x0001, 453/453. */
	memcpy(cf.marker, "\x00\x00\x01\x00", 4);

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_scs_ctrl_build(&cf, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "REJECT_REQ builds");
	ct_check_eq_u32(written, VMS_ETH_HDR_LEN + VMS_SCSCTRL_LEN_MARKER,
			"  writes the 62-content frame length");
	ct_check(built[14 + 46] == VMS_SCS_CTRL_REJECT_REQ && built[14 + 47] == 0,
		 "  op verb == 4 at abs 60 (LE16)");
	ct_check(built[14 + 58] == 0 && built[14 + 59] == 0,
		 "  marker[58:60] == 0x0000 (CENSUS-A)");
	ct_check(built[14 + 60] == 1 && built[14 + 61] == 0,
		 "  marker[60:62] == 0x0001 (CENSUS-A)");

	ct_check(vms_frame_classify(built, written, &fi) == VMS_CODEC_OK,
		 "re-classifies");
	ct_check(vms_scs_ctrl_parse(built, written, &fi, &cf2) == VMS_CODEC_OK,
		 "re-parses");
	ct_check_eq_u32(cf2.op, VMS_SCS_CTRL_REJECT_REQ, "  op round-trips");
	ct_check(memcmp(cf2.marker, cf.marker, 4) == 0, "  marker round-trips");
}

/*
 * op 8/9 CREDIT_REQ/CREDIT_RSP: GROUNDED structural invariants (sec
 * 4(h)(1c)/(1f)): 58-content class (envelope + handle pair only, no
 * marker), credit field constant 1 across 855 real-VAX frames (sec
 * 4h(1c) table). No single frame is byte-exact-cited.
 */
static void credit_op_structural(uint16_t op, const char *label)
{
	struct vms_scs_ctrl_frame cf, cf2;
	struct vms_frame_info fi;
	uint8_t built[256];
	uint32_t written = 0;
	char what[160];

	memset(&cf, 0, sizeof(cf));
	memcpy(cf.hdr.eth_dst, "\x08\x00\x2b\x4a\xb7\x15", 6);
	memcpy(cf.hdr.eth_src, "\x08\x00\x2b\x78\x56\xb9", 6);
	memcpy(cf.hdr.dst_lavc, "\xaa\x00\x04\x00\x01\x04", 6);
	memcpy(cf.hdr.src_lavc, "\xaa\x00\x04\x00\x02\x04", 6);
	cf.hdr.connect_flag = 0x0001;
	cf.hdr.word30 = 0x134bu; /* abs30 msgtype 0x4b, abs31 format 0x13 */
	cf.hdr.sca_len_field = VMS_SCSCTRL_LEN_SHORT - 2u;
	cf.recv_ack = 20;
	cf.send_seq = 20;
	cf.incarn = 1;
	cf.lan_ovrhd = 0x0012;
	cf.tail_const1 = 0x0001;
	cf.tail_const2 = 0x0200;
	cf.inner_len = VMS_SCSCTRL_LEN_SHORT - 44u;
	cf.op = op;
	/* GROUNDED (sec 4h(1c)/(1f)): credit == 1, constant, 855/855. */
	cf.credit = 1;
	cf.conid_remote = 0x08000563u;
	cf.conid_local = 0x07005933u;

	memset(built, 0xAA, sizeof(built));
	snprintf(what, sizeof(what), "%s: builds", label);
	ct_check(vms_scs_ctrl_build(&cf, built, sizeof(built), &written)
		 == VMS_CODEC_OK, what);
	ct_check_eq_u32(written, VMS_ETH_HDR_LEN + VMS_SCSCTRL_LEN_SHORT,
			"  writes the 58-content frame length (no marker span)");
	ct_check(built[14 + 48] == 1 && built[14 + 49] == 0,
		 "  credit == 1 (GROUNDED constant, sec 4h(1c)/(1f))");

	ct_check(vms_frame_classify(built, written, &fi) == VMS_CODEC_OK,
		 "re-classifies");
	ct_check(vms_scs_ctrl_parse(built, written, &fi, &cf2) == VMS_CODEC_OK,
		 "re-parses");
	ct_check_eq_u32(cf2.op, op, "  op round-trips");
	ct_check(cf2.has_marker == 0 && cf2.has_names == 0,
		 "  no marker/name span on the 58-content class");
}

static void test_credit_ops_structural(void)
{
	printf("-- op 8/9 CREDIT_REQ/CREDIT_RSP: GROUNDED structural "
	       "invariants (sec 4h(1c)/(1f), 855/855 credit==1)\n");
	credit_op_structural(VMS_SCS_CTRL_CREDIT_REQ, "op8 CREDIT_REQ");
	credit_op_structural(VMS_SCS_CTRL_CREDIT_RSP, "op9 CREDIT_RSP");
}

/* ---- group 4: op-verb constants + error paths -------------------------- */

static void test_op_verb_constants(void)
{
	printf("-- the op-verb constants FC-P0.6 did not carry ($SCSDEF, sec "
	       "4(h)(1h)/4(m))\n");
	ct_check_eq_u32(VMS_SCS_CTRL_REJECT_RSP, 5, "REJECT_RSP == 5");
	ct_check_eq_u32(VMS_SCS_CTRL_DISCONNECT_RSP, 7, "DISCONNECT_RSP == 7");
	ct_check_eq_u32(VMS_SCS_CTRL_CREDIT_REQ, 8, "CREDIT_REQ == 8");
	ct_check_eq_u32(VMS_SCS_CTRL_CREDIT_RSP, 9, "CREDIT_RSP == 9");
}

static void test_error_paths(void)
{
	const struct vms_fixture *hello_free = fixture("scs-dir-connect-request");
	struct vms_scs_ctrl_frame cf;
	struct vms_frame_info fi;
	uint8_t out[256];
	uint32_t written = 0;

	printf("-- error paths\n");
	ct_check(vms_scs_ctrl_parse(NULL, 0, NULL, &cf) == VMS_CODEC_E_INVAL,
		 "NULL fi -> E_INVAL");
	ct_check(vms_scs_ctrl_build(NULL, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL, "NULL frame struct -> E_INVAL");

	/* A HELLO is not VMS_FFAM_SCS at all. */
	{
		const struct vms_fixture *h = fixture("hello-multicast-vax1");

		if (h != NULL) {
			(void)vms_frame_classify(h->bytes, h->wire_len, &fi);
			ct_check(vms_scs_ctrl_parse(h->bytes, h->wire_len, &fi,
						    &cf) == VMS_CODEC_E_CLASS,
				 "a HELLO frame is refused: not VMS_FFAM_SCS");
		}
	}

	/* An invalid has_* combination (tail4 AND names together) is rejected. */
	memset(&cf, 0, sizeof(cf));
	cf.has_marker = 1;
	cf.has_tail4 = 1;
	cf.has_names = 1;
	ct_check(vms_scs_ctrl_build(&cf, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL,
		 "tail4 + names together is not a real shape -> E_INVAL");

	/* has_names without has_marker is not a real shape either. */
	memset(&cf, 0, sizeof(cf));
	cf.has_names = 1;
	ct_check(vms_scs_ctrl_build(&cf, out, sizeof(out), &written)
		 == VMS_CODEC_E_INVAL,
		 "names without marker is not a real shape -> E_INVAL");

	/* dir_lookup_parse refuses a NULL. */
	ct_check(vms_scs_dir_lookup_parse(NULL, NULL) == VMS_CODEC_E_INVAL,
		 "vms_scs_dir_lookup_parse(NULL) -> E_INVAL");

	(void)hello_free;
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_scs: SCS connection/directory codec entries "
	       "(FC-P2.1)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_fixture_roundtrips();
	test_dir_lookup_request_is_empty();
	test_dir_lookup_response_not_present();
	test_dir_lookup_affirmative_roundtrip();
	test_dir_lookup_refuses_connect_shape();
	test_reject_req_structural();
	test_credit_ops_structural();
	test_op_verb_constants();
	test_error_paths();

	return ct_summary("test_codec_scs");
}
