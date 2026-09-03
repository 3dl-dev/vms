// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_cm.c - connection-manager (CM / VMS$VAXcluster) codec entries,
 * rung R1 (FC-P3.1).
 *
 * Four groups:
 *   1. Response-recipe byte-exactness: for each GROUNDED (category, opcode)
 *      pair, parse a REQUEST specimen, run this file's builder, and assert
 *      the built BODY span (abs 72+, VMS_CM_BODY_LEN) matches the cited
 *      bytes of a matched RESPONSE specimen exactly. Six recipes: the
 *      cat-0x01 echo family (op 0x09 ADD-open, op 0x03 commit, op 0x0f
 *      echo-not-force, op 0x12 relay), the cat-0x06 close, and the cat-0x02
 *      op-0x0d DLM rebuild echo.
 *   2. The allowlist: this item's GROUNDED table validates, every recipe
 *      row round-trips through vms_wire_allow_find(), and an UNLISTED
 *      (SYSAP, category, opcode) pair -- cat 0x02 op 0x01, the steady-state
 *      DLM traffic this project measured as ungrounded -- yields NULL: "no
 *      response" per the done-condition.
 *   3. Field parsers: open/barrier/params/model/dlm_rebuild decode their
 *      GROUNDED fields off spec-composed specimens (model reuses the
 *      existing FC-P0.7-adjacent scs-msg.spec fixture).
 *   4. Error paths: NULL args, an ungrounded pair refused even with a
 *      well-formed frame, class mismatch.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_cm.h"
#include "vms_frame_compose.h"   /* test-only: struct vms_cm_link, the
				  * full-frame composer used to assemble a
				  * comparable specimen and to round-trip an
				  * ORIGINATED body back through the parser
				  * (design sec 3.2.4, FC-P3.15) */

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

/* A default link: real Con.IDs/recv_ack/send_seq/eth addrs, all fixed so the
 * test is deterministic. Used ONLY by the test-only frame composer to
 * assemble a comparable specimen or round-trip a built body through the
 * parser -- the envelope span (abs 0-71) it produces is NEVER compared
 * against the fixtures: this item's grounded scope is the BODY (abs 72+,
 * VMS_CM_BODY_LEN), and since FC-P3.15 no production builder here even
 * sees abs [0,72) (design sec 3.2.4). */
static void fill_link(struct vms_cm_link *l)
{
	static const uint8_t dst[6] = { 0x08, 0x00, 0x2b, 0x4a, 0xb7, 0x15 };
	static const uint8_t src[6] = { 0x08, 0x00, 0x2b, 0x78, 0x56, 0xb9 };

	memset(l, 0, sizeof(*l));
	memcpy(l->hdr.eth_dst, dst, 6);
	memcpy(l->hdr.eth_src, src, 6);
	memcpy(l->hdr.dst_lavc, dst, 6);
	memcpy(l->hdr.src_lavc, src, 6);
	l->hdr.connect_flag = 0x0001;
	l->recv_ack = 0x0007;
	l->send_seq = 0x0009;
	l->remote_conid = 0x62c50009u;
	l->local_conid = 0x33580008u;
}

/* ---- group 1: response-recipe byte-exactness -------------------------- */

/*
 * Compare `built[0..VMS_CM_BODY_LEN)` (a builder's BODY-relative output)
 * against the response fixture's CITED bytes at the matching FRAME-absolute
 * offset (body[k] == abs 72+k) -- this item's grounded scope.
 *
 * abs [72,76) (body[0:4], send_msg/ack_msg) is EXCLUDED: since FC-P3.15 that
 * span is cnxman_envelope_stamp()'s job (vms_cnxman_csb.h), never a codec_cm
 * builder's -- it is proven separately, on real CSB state, not here.
 */
static void assert_body_matches(const struct vms_fixture *resp,
				const uint8_t *built, const char *label)
{
	uint32_t i, checked = 0, mismatches = 0;
	char what[224];

	for (i = VMS_OFF_SYSAP_BODY; i < VMS_CM_FRAME_LEN; i++) {
		if (i < VMS_OFF_SYSAP_BODY + 4u)
			continue;   /* body[0:4]: the stamper's, not this codec's */
		if (!vms_fixture_is_cited(resp, i, 1))
			continue;
		checked++;
		if (built[i - VMS_OFF_SYSAP_BODY] != resp->bytes[i]) {
			if (mismatches == 0)
				printf("       %s: first body mismatch at abs "
				       "%u: built %02x, specimen %02x\n",
				       label, i, built[i - VMS_OFF_SYSAP_BODY],
				       resp->bytes[i]);
			mismatches++;
		}
	}
	snprintf(what, sizeof(what),
		 "%s: every CITED body byte matches the response specimen "
		 "(%u checked)",
		 label, checked);
	ct_check(mismatches == 0 && checked > 0, what);
}

static void test_echo_recipe(const char *req_name, const char *resp_name,
			     uint8_t own_class, const char *label)
{
	const struct vms_fixture *req = fixture(req_name);
	const struct vms_fixture *resp = fixture(resp_name);
	uint8_t built[VMS_CM_BODY_LEN];
	uint32_t written = 0;
	char what[192];

	ct_check(req != NULL, req_name);
	ct_check(resp != NULL, resp_name);
	if (req == NULL || resp == NULL)
		return;

	memset(built, 0xAA, sizeof(built));
	snprintf(what, sizeof(what), "%s: vms_cm_echo_response_build succeeds",
		 label);
	ct_check(vms_cm_echo_response_build(req->bytes, req->wire_len,
					    own_class, built, sizeof(built),
					    &written)
		 == VMS_CODEC_OK, what);
	ct_check_eq_u32(written, VMS_CM_BODY_LEN, "  wrote VMS_CM_BODY_LEN");
	assert_body_matches(resp, built, label);
}

static void test_close_recipe(void)
{
	const struct vms_fixture *req = fixture("cm-close-req");
	const struct vms_fixture *resp = fixture("cm-close-resp");
	struct vms_cm_node_params np;
	uint8_t built[VMS_CM_BODY_LEN];
	uint32_t written = 0;

	printf("-- cat 0x06 close recipe (own node-parameter block)\n");
	ct_check(req != NULL, "cm-close-req fixture present");
	ct_check(resp != NULL, "cm-close-resp fixture present");
	if (req == NULL || resp == NULL)
		return;

	np.param_f1 = 0x00000010u;
	np.param_f2 = 0x00000001u;
	memcpy(np.version, "V7.3    ", VMS_CM_VERSION_LEN);

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_cm_close_build(req->bytes, req->wire_len, &np, built,
				    sizeof(built), &written)
		 == VMS_CODEC_OK, "vms_cm_close_build succeeds");
	ct_check_eq_u32(written, VMS_CM_BODY_LEN, "  wrote VMS_CM_BODY_LEN");
	assert_body_matches(resp, built, "cm-close");
}

static void test_dlm_op0d_recipe(void)
{
	const struct vms_fixture *req = fixture("cm-dlm-op0d-req");
	const struct vms_fixture *resp = fixture("cm-dlm-op0d-resp");
	uint8_t built[VMS_CM_BODY_LEN];
	uint32_t written = 0;

	printf("-- cat 0x02 op 0x0d DLM rebuild echo recipe\n");
	ct_check(req != NULL, "cm-dlm-op0d-req fixture present");
	ct_check(resp != NULL, "cm-dlm-op0d-resp fixture present");
	if (req == NULL || resp == NULL)
		return;

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_cm_dlm_op0d_response_build(req->bytes, req->wire_len,
						built, sizeof(built), &written)
		 == VMS_CODEC_OK, "vms_cm_dlm_op0d_response_build succeeds");
	ct_check_eq_u32(written, VMS_CM_BODY_LEN, "  wrote VMS_CM_BODY_LEN");
	assert_body_matches(resp, built, "cm-dlm-op0d");

	/* Negative control mirroring spec sec 4(p)'s LOCKMGRERR warning:
	 * body[16] (the L1 length) must NOT be disturbed the way the cat-0x01
	 * body[18] mutation would if it were wrongly applied here. */
	ct_check(built[VMS_OFB_CM_DLM_L1_LEN] ==
			 req->bytes[VMS_OFF_CM_DLM_L1_LEN],
		 "  body[16] (L1 length) echoed untouched -- the cat-0x01 "
		 "mutation was NOT applied here");
}

static void test_ack_build(void)
{
	uint8_t built[VMS_CM_BODY_LEN];
	uint32_t written = 0;

	printf("-- cat 0x04 credit/commit ack builder (spec sec 4(u))\n");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_cm_ack_build(built, sizeof(built), &written)
		 == VMS_CODEC_OK, "vms_cm_ack_build succeeds");
	ct_check_eq_u32(written, VMS_CM_BODY_LEN, "  wrote VMS_CM_BODY_LEN");
	/* body[0:4] (send/ack) is NOT this builder's business since FC-P3.15
	 * -- cnxman_envelope_stamp() fills it (vms_cnxman_csb.h), proven on
	 * real CSB state, not here. */
	ct_check(built[VMS_OFB_CM_CATEGORY] == VMS_CM_CAT_ACK,
		 "  body[8] == category 0x04");
	ct_check(built[VMS_OFB_CM_TXN] == 0 && built[VMS_OFB_CM_TXN + 1] == 0,
		 "  body[4:6] == 0 (no payload)");
}

/* ---- group 2: the allowlist -------------------------------------------- */

static void test_allowlist(void)
{
	const struct vms_wire_allow_table *t = vms_cm_allow_table();
	const struct vms_wire_allow_entry *e;

	printf("-- the GROUNDED (SYSAP, category, opcode) allowlist\n");
	ct_check(t != NULL, "vms_cm_allow_table() returns a table");
	ct_check(vms_wire_allow_table_validate(t) == VMS_CODEC_OK,
		 "the table validates: no dupes, no response-bit categories, "
		 "every RESPOND row has a recipe, every row cites the spec");

	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG,
				VMS_CM_OP_COMMIT);
	ct_check(e != NULL && e->recipe == VMS_CM_RECIPE_ECHO,
		 "cat 0x01 op 0x03 -> the echo recipe");
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_DLM,
				VMS_CM_OP_DLM_REBUILD);
	ct_check(e != NULL && e->recipe == VMS_CM_RECIPE_DLM_OP0D,
		 "cat 0x02 op 0x0d -> the DLM rebuild recipe");
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_MEMBERSHIP,
				VMS_CM_OP_CLOSE);
	ct_check(e != NULL && e->recipe == VMS_CM_RECIPE_CLOSE,
		 "cat 0x06 op 0x00 -> the close recipe");

	/*
	 * The done-condition's decisive assertion: an UNLISTED (SYSAP,
	 * category, opcode) yields "no response". cat 0x02 op 0x01 (the
	 * steady-state DLM ENQ/lookup traffic) is the GROUNDED-ABSENT case
	 * this file's own allowlist comment documents -- src/vmsscs/scsd.c's
	 * cm_response_shape measured that no recipe short of a real lock
	 * database reconstructs more than 37% of real responses, so this
	 * codec answers NOTHING rather than guess.
	 */
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_DLM,
				VMS_CM_OP_PARAMS /* op 0x01 under cat 0x02 */);
	ct_check(e == NULL,
		 "cat 0x02 op 0x01 (ungrounded DLM lookup/enqueue) is "
		 "UNLISTED -- send nothing, log it");
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_DLM,
				VMS_CM_OP_RELAY /* op 0x12 under cat 0x02 */);
	ct_check(e == NULL,
		 "cat 0x02 op 0x12 (ungrounded, 3x higher volume than op "
		 "0x01) is UNLISTED -- send nothing, log it");
	e = vms_wire_allow_find(t, VMS_SYSAP_MSCP_DISK, VMS_CM_CAT_CONFIG,
				VMS_CM_OP_COMMIT);
	ct_check(e == NULL,
		 "the SAME (category, opcode) under a DIFFERENT SYSAP is "
		 "UNLISTED -- the allowlist is per-SYSAP, not global");

	/* The grounded CONSUME rows: accepted, never answered. */
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG,
				VMS_CM_OP_XITION_GO);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME,
		 "cat 0x01 op 0x0a (barrier GO) is a grounded CONSUME row");
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG,
				VMS_CM_OP_BARRIER_REL);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME,
		 "cat 0x01 op 0x0c (barrier release) is a grounded CONSUME row");
	e = vms_wire_allow_find(t, VMS_SYSAP_VMS_VAXCLUSTER, VMS_CM_CAT_CONFIG,
				VMS_CM_OP_MEMBERSHIP);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME,
		 "cat 0x01 op 0x06 (MEMBERSHIP burst) is a grounded CONSUME row");
}

/* ---- group 3: field parsers -------------------------------------------- */

static void test_open_parse(void)
{
	const struct vms_fixture *f = fixture("cm-open-add-req");
	struct vms_frame_info fi;
	struct vms_cm_open o;

	printf("-- vms_cm_open_parse (op 0x09 ADD transition-open)\n");
	ct_check(f != NULL, "cm-open-add-req fixture present");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies");
	ct_check(vms_cm_open_parse(f->bytes, f->wire_len, &fi, &o) == VMS_CODEC_OK,
		 "parses");
	ct_check_eq_u32(o.env.category, 0x01, "  category == 0x01");
	ct_check_eq_u32(o.env.opcode, 0x09, "  opcode == 0x09");
	ct_check_eq_u32(o.epoch, 6, "  epoch == 6");
	ct_check_eq_u32(o.role, VMS_CM_ROLE_XITION, "  role == ROLE_XITION");
	ct_check_eq_u32(o.cls, VMS_CM_CLASS_ADD, "  class == CLASS_ADD");
	ct_check(o.has_bitmap, "  has_bitmap == 1 (op 0x09)");
	ct_check_eq_u32(o.bitmap, 0x0e, "  bitmap == 0x0e (M=3)");
}

static void test_barrier_parse(void)
{
	const struct vms_fixture *f = fixture("cm-barrier-step");
	struct vms_frame_info fi;
	struct vms_cm_barrier b;

	printf("-- vms_cm_barrier_parse (op 0x0b step index)\n");
	ct_check(f != NULL, "cm-barrier-step fixture present");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies");
	ct_check(vms_cm_barrier_parse(f->bytes, f->wire_len, &fi, &b)
		 == VMS_CODEC_OK, "parses");
	ct_check_eq_u32(b.env.opcode, 0x0b, "  opcode == 0x0b");
	ct_check_eq_u32(b.epoch, 3, "  epoch == 3");
	ct_check_eq_u32(b.step, 5, "  step == 5 (NOT the role/class byte pair "
			"other opcodes carry at the same offset)");
}

static void test_params_parse(void)
{
	const struct vms_fixture *f = fixture("cm-params");
	struct vms_frame_info fi;
	struct vms_cm_params p;

	printf("-- vms_cm_params_parse (op 0x01 VOTES + node-parameter block)\n");
	ct_check(f != NULL, "cm-params fixture present");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies");
	ct_check(vms_cm_params_parse(f->bytes, f->wire_len, &fi, &p)
		 == VMS_CODEC_OK, "parses");
	ct_check_eq_u32(p.votes, 0, "  VOTES == 0 (non-voting, sec 4(j))");
	ct_check_eq_u32(p.param_f1, 0x10, "  param_f1 == 0x10 (observed const)");
	ct_check_eq_u32(p.param_f2, 0x01, "  param_f2 == 0x01 (observed const)");
	ct_check(memcmp(p.version, "V7.3    ", VMS_CM_VERSION_LEN) == 0,
		 "  version == \"V7.3    \"");
}

static void test_model_parse(void)
{
	/* Reuses the existing FC-P0.6-era fixture for the SAME 190-byte class
	 * (docs/cluster-protocol-spec.md sec 4(j)): op 0x14 model advert. No
	 * new fixture needed -- this proves this item's typed accessor reads
	 * the SAME grounded bytes an earlier item already cited. */
	const struct vms_fixture *f = fixture("scs-msg190-vaxcluster-cat01-op14");
	struct vms_frame_info fi;
	struct vms_cm_model m;

	printf("-- vms_cm_model_parse (op 0x14 model advertisement)\n");
	ct_check(f != NULL, "scs-msg190-vaxcluster-cat01-op14 fixture present");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies");
	ct_check(vms_cm_model_parse(f->bytes, f->wire_len, &fi, &m)
		 == VMS_CODEC_OK, "parses");
	ct_check_eq_u32(m.env.opcode, 0x14, "  opcode == 0x14");
	ct_check_eq_u32(m.namelen, 21, "  namelen == 21 (GROUNDED sec 4(j))");
	ct_check(memcmp(m.name, "VAXserver 3900 Series", 21) == 0,
		 "  name == \"VAXserver 3900 Series\"");
}

static void test_dlm_rebuild_parse(void)
{
	const struct vms_fixture *f = fixture("cm-dlm-op0d-req");
	struct vms_frame_info fi;
	struct vms_cm_dlm_rebuild d;

	printf("-- vms_cm_dlm_rebuild_parse (cat 0x02 op 0x0d request layout)\n");
	ct_check(f != NULL, "cm-dlm-op0d-req fixture present");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies");
	ct_check(vms_cm_dlm_rebuild_parse(f->bytes, f->wire_len, &fi, &d)
		 == VMS_CODEC_OK, "parses");
	ct_check_eq_u32(d.env.category, 0x02, "  category == 0x02");
	ct_check_eq_u32(d.env.opcode, 0x0d, "  opcode == 0x0d");
	ct_check_eq_u32(d.l1_len, 0x10, "  L1 length == 0x10");
	ct_check_eq_u32(d.resnamelen, 13, "  resource-name length == 13");
	ct_check(memcmp(d.resname, "F11B$aSYSDSK1", 13) == 0,
		 "  resource name == \"F11B$aSYSDSK1\" (Files-11 namespace, "
		 "sec 4(f))");
}

/* ---- group 4: error paths ----------------------------------------------- */

static void test_error_paths(void)
{
	const struct vms_fixture *req = fixture("cm-op0f-req");
	struct vms_cm_link l;
	struct vms_cm_node_params np;
	uint8_t built[VMS_CM_BODY_LEN];
	uint32_t written = 0;
	uint32_t frame_written = 0;
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint8_t junk[VMS_CM_FRAME_LEN];

	printf("-- error paths\n");
	ct_check(req != NULL, "cm-op0f-req fixture present");
	fill_link(&l);
	memset(&np, 0, sizeof(np));

	ct_check(vms_frame_compose_link(NULL, frame, sizeof(frame),
					&frame_written) == VMS_CODEC_E_INVAL,
		 "vms_frame_compose_link: NULL link rejected (test-only "
		 "composer)");
	ct_check(vms_cm_envelope_parse(frame, sizeof(frame), NULL, NULL)
		 == VMS_CODEC_E_INVAL, "vms_cm_envelope_parse: NULL out rejected");

	if (req != NULL) {
		/* op 0x0f IS grounded for the ECHO recipe but NOT for the
		 * CLOSE recipe -- a recipe builder must refuse a
		 * well-formed, classifiable frame whose (cat,op) it does not
		 * own, not merely a malformed one. */
		ct_check(vms_cm_close_build(req->bytes, req->wire_len, &np,
					    built, sizeof(built), &written)
			 == VMS_CODEC_E_CLASS,
			 "vms_cm_close_build refuses a well-formed op 0x0f "
			 "request (wrong recipe for this (cat,op))");
		ct_check(vms_cm_dlm_op0d_response_build(req->bytes,
							req->wire_len, built,
							sizeof(built), &written)
			 == VMS_CODEC_E_CLASS,
			 "vms_cm_dlm_op0d_response_build refuses op 0x0f too");
	}

	memset(junk, 0, sizeof(junk));
	/* Not SCA at all (bad ethertype) -> a class-gated builder must not
	 * accept it. */
	ct_check(vms_cm_echo_response_build(junk, sizeof(junk), 0, built,
					    sizeof(built), &written)
		 != VMS_CODEC_OK,
		 "vms_cm_echo_response_build refuses a non-SCA frame");
}

/* ---- FC-P3.5's three additions: the op-0x0b ORIGINATION, the DLM-body
 * wrapper, and the bitmap-width span accessor ---------------------------- */

static void test_barrier_build(void)
{
	struct vms_cm_link l;
	struct vms_frame_info fi;
	struct vms_cm_barrier parsed;
	uint8_t built[VMS_CM_BODY_LEN];
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0, frame_written = 0, i, nonzero_tail = 0;

	printf("-- vms_cm_barrier_build (op 0x0b, the one CM message this "
	       "codec ORIGINATES)\n");
	fill_link(&l);

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_cm_barrier_build(0x0e, 5, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "builds");
	ct_check_eq_u32(written, VMS_CM_BODY_LEN, "  wrote a whole body");

	/* Round-trip through the test-only composer (design sec 3.2.4): the
	 * parser is frame-based (receive stays frame-level), so a body-level
	 * builder's output has to be assembled into a specimen to exercise
	 * it, exactly as the port will once FC-P1.1/P2.1 land. */
	ct_check(vms_frame_compose(&l, built, frame, sizeof(frame),
				   &frame_written) == VMS_CODEC_OK,
		 "  composes into a full specimen");
	ct_check(vms_frame_classify(frame, frame_written, &fi) == VMS_CODEC_OK,
		 "  the result classifies as an SCA message");
	ct_check(vms_cm_barrier_parse(frame, frame_written, &fi, &parsed)
		 == VMS_CODEC_OK, "  and round-trips through the parser");
	ct_check_eq_u32(parsed.env.category, VMS_CM_CAT_CONFIG, "  cat 0x01");
	ct_check_eq_u32(parsed.env.opcode, VMS_CM_OP_BARRIER, "  op 0x0b");
	ct_check_eq_u32(parsed.epoch, 0x0e, "  epoch echoed from the caller");
	ct_check_eq_u32(parsed.step, 5, "  step 5 at body[16:20]");
	ct_check_eq_u32(parsed.env.token, 0,
			"  body[6:8] (token) is NOT written here -- it is "
			"cnxman_envelope_stamp()'s job (vms_cnxman_csb.h), "
			"proven separately on real CSB state -- and it is "
			"honest zero, never the step index either");

	/* Spec sec 4(p) + the af2 zero-body joiner: the tail is an explicit
	 * zero, not another implementation's uninitialised memory. */
	for (i = 20; i < VMS_CM_BODY_LEN; i++) {
		if (built[i] != 0u)
			nonzero_tail++;
	}
	ct_check_eq_u32(nonzero_tail, 0,
			"  body[20:] is an EXPLICIT ZERO tail");

	ct_check(vms_cm_barrier_build(0x0e, 0, built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  step 0 is rejected (spec sec 4(p): indices run 1...12)");
	ct_check(vms_cm_barrier_build(0x0e, 1, NULL, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  a NULL out_body is rejected -- no zero-filled body");
}

/* ==========================================================================
 * FC-P3.3: the JOINER's three originations (sec 5c) + the op-0x06
 * instrumentation helper.
 *
 * Each builder is round-tripped through the test-only composer into the
 * matching PARSER, so the assertion is "the grounded field landed where the
 * grounded field is read from" rather than an offset typed twice.
 * ========================================================================== */

static void test_joiner_originations(void)
{
	struct vms_cm_link l;
	struct vms_frame_info fi;
	struct vms_cm_model model;
	struct vms_cm_params params;
	struct vms_cm_node_params own;
	uint8_t built[VMS_CM_BODY_LEN];
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t written = 0, frame_written = 0, i, nonzero = 0;
	static const uint8_t name[] = "OVMX x86_64";

	printf("-- FC-P3.3 sec 5c: the joiner's op 0x14 / 0x01 / 0x02\n");
	fill_link(&l);

	/* ---- op 0x14, the model advertisement (sec 4(j) row 1) ---- */
	ct_check(vms_cm_model_build(name, 11u, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "model builds");
	ct_check_eq_u32(written, VMS_CM_BODY_LEN, "  wrote a whole body");
	ct_check(vms_frame_compose(&l, built, frame, sizeof(frame),
				   &frame_written) == VMS_CODEC_OK,
		 "  composes into a specimen");
	ct_check(vms_frame_classify(frame, frame_written, &fi) == VMS_CODEC_OK,
		 "  classifies");
	ct_check(vms_cm_model_parse(frame, frame_written, &fi, &model)
		 == VMS_CODEC_OK, "  round-trips through vms_cm_model_parse");
	ct_check_eq_u32(model.env.category, VMS_CM_CAT_CONFIG, "  cat 0x01");
	ct_check_eq_u32(model.env.opcode, VMS_CM_OP_MODEL, "  op 0x14");
	ct_check_eq_u32(model.namelen, 11u, "  the length prefix at body[16]");
	ct_check(memcmp(model.name, name, 11) == 0,
		 "  and the ASCII string at body[17]");
	ct_check(vms_cm_model_build(name, (uint8_t)(VMS_CM_MODEL_MAX + 1u),
				    built, sizeof(built), &written)
		 == VMS_CODEC_E_RANGE,
		 "  an over-long model name is REFUSED, never truncated");
	ct_check(vms_cm_model_build(NULL, 0u, built, sizeof(built), &written)
		 == VMS_CODEC_OK,
		 "  and namelen 0 is legal: a node that has no model string "
		 "advertises none");

	/* ---- op 0x01, the cluster parameters (sec 4(j) VOTES) ---- */
	memset(&own, 0, sizeof(own));
	own.param_f1 = 0x11223344u;
	own.param_f2 = 0x55667788u;
	memcpy(own.version, "VMX V0.6", VMS_CM_VERSION_LEN);
	ct_check(vms_cm_params_build(2u, &own, built, sizeof(built), &written)
		 == VMS_CODEC_OK, "params builds");
	ct_check(vms_frame_compose(&l, built, frame, sizeof(frame),
				   &frame_written) == VMS_CODEC_OK,
		 "  composes");
	ct_check(vms_frame_classify(frame, frame_written, &fi) == VMS_CODEC_OK,
		 "  classifies");
	ct_check(vms_cm_params_parse(frame, frame_written, &fi, &params)
		 == VMS_CODEC_OK, "  round-trips through vms_cm_params_parse");
	ct_check_eq_u32(params.env.opcode, VMS_CM_OP_PARAMS, "  op 0x01");
	ct_check_eq_u32(params.votes, 2u, "  VOTES at body[22:24]");
	ct_check_eq_u32(params.param_f1, 0x11223344u,
			"  the caller's own param_f1 -- never a captured 0x10");
	ct_check_eq_u32(params.param_f2, 0x55667788u, "  ... and param_f2");
	ct_check(memcmp(params.version, "VMX V0.6", VMS_CM_VERSION_LEN) == 0,
		 "  the caller's OWN version string, never a baked \"V7.3\"");
	ct_check(vms_cm_params_build(0u, NULL, built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  a NULL parameter block is refused, not zero-filled");

	/* ---- op 0x02, the admission trigger (sec 4(o) row 4) ---- */
	memset(built, 0xAA, sizeof(built));
	ct_check(vms_cm_config_build(built, sizeof(built), &written)
		 == VMS_CODEC_OK, "config builds");
	ct_check_eq_u32(built[VMS_OFB_CM_CATEGORY], VMS_CM_CAT_CONFIG,
			"  cat 0x01");
	ct_check_eq_u32(built[VMS_OFB_CM_OPCODE], VMS_CM_OP_CONFIG, "  op 0x02");
	for (i = 0; i < VMS_CM_BODY_LEN; i++) {
		if (i == VMS_OFB_CM_CATEGORY || i == VMS_OFB_CM_OPCODE)
			continue;
		if (built[i] != 0u)
			nonzero++;
	}
	ct_check_eq_u32(nonzero, 0u,
			"  EVERY other byte is an explicit zero -- sec 4(o)'s "
			"REPLAYED-not-decoded spans are NOT reproduced");
}

static void test_membership_find_sysid(void)
{
	const struct vms_fixture *req = fixture("cm-commit-req");
	struct vms_cm_link l;
	struct vms_frame_info fi;
	uint8_t body[VMS_CM_BODY_LEN];
	uint8_t frame[VMS_CM_FRAME_LEN];
	uint32_t frame_written = 0, off = 0, width = 0;
	const uint64_t sysid = 0x000004000103ull;
	unsigned i;

	printf("-- FC-P3.3: vms_cm_membership_find_sysid (INSTRUMENTATION)\n");
	fill_link(&l);

	/* A cat-0x01 op-0x06 body with the caller's own SCSSYSTEMID at a byte
	 * offset THIS TEST chose -- because no capture grounds one, which is
	 * exactly why the function reports the offset instead of reading a
	 * record at it (integration note E8). */
	memset(body, 0, sizeof(body));
	body[VMS_OFB_CM_CATEGORY] = VMS_CM_CAT_CONFIG;
	body[VMS_OFB_CM_OPCODE] = VMS_CM_OP_MEMBERSHIP;
	for (i = 0; i < 8u; i++)
		body[40u + i] = (uint8_t)((sysid >> (8u * i)) & 0xffu);

	ct_check(vms_frame_compose(&l, body, frame, sizeof(frame),
				   &frame_written) == VMS_CODEC_OK,
		 "composes an op-0x06 specimen");
	ct_check(vms_frame_classify(frame, frame_written, &fi) == VMS_CODEC_OK,
		 "  classifies");
	ct_check(vms_cm_membership_find_sysid(frame, frame_written, &fi, sysid,
					      &off, &width) == VMS_CODEC_OK,
		 "  finds the caller's own SCSSYSTEMID");
	ct_check_eq_u32(off, 40u, "  at the offset it really sits at");
	ct_check_eq_u32(width, 8u, "  reported as the quadword form");

	/* A different sysid is NOT found -- the function reports absence, it
	 * does not fall back to a plausible offset. */
	ct_check(vms_cm_membership_find_sysid(frame, frame_written, &fi,
					      0x0000040001ffull, &off, &width)
		 == VMS_CODEC_E_RANGE,
		 "  a sysid that is not there is reported ABSENT");
	ct_check_eq_u32(off, 0u, "  with no offset asserted");
	ct_check_eq_u32(width, 0u, "  and no width asserted");

	/* And it refuses any other (category, opcode): reading this span from
	 * a commit request would report residue as membership. */
	if (req != NULL) {
		ct_check(vms_frame_classify(req->bytes, req->wire_len, &fi)
			 == VMS_CODEC_OK, "  cm-commit-req classifies");
		ct_check(vms_cm_membership_find_sysid(req->bytes, req->wire_len,
						      &fi, sysid, &off, &width)
			 == VMS_CODEC_E_CLASS,
			 "  and a NON-op-0x06 frame is refused outright");
	}
}

static void test_body_build(void)
{
	const struct vms_fixture *req = fixture("cm-dlm-op0d-req");
	struct vms_frame_info fi;
	struct vms_cm_envelope req_env;
	uint8_t body[VMS_CM_BODY_LEN];
	uint8_t built[VMS_CM_BODY_LEN];
	uint32_t written = 0, i, diffs = 0;

	printf("-- vms_cm_body_build (wrapping a body the LOCK MANAGER made)\n");
	ct_check(req != NULL, "cm-dlm-op0d-req fixture present");
	if (req == NULL)
		return;
	ct_check(vms_frame_classify(req->bytes, req->wire_len, &fi) ==
			 VMS_CODEC_OK,
		 "the request specimen classifies");
	ct_check(vms_cm_envelope_parse(req->bytes, req->wire_len, &fi,
				       &req_env) == VMS_CODEC_OK,
		 "and its envelope parses");

	for (i = 0; i < VMS_CM_BODY_LEN; i++)
		body[i] = (uint8_t)(i * 7u + 3u);

	ct_check(vms_cm_body_build(req->bytes, req->wire_len, body,
				   sizeof(body), built, sizeof(built),
				   &written) == VMS_CODEC_OK, "builds");
	for (i = 8; i < VMS_CM_BODY_LEN; i++) {
		if (built[i] != body[i])
			diffs++;
	}
	ct_check_eq_u32(diffs, 0,
			"  body[8:132] (the DLM's payload) is copied VERBATIM "
			"-- a wrapper, never a recipe");
	ct_check(built[0] == body[0] && built[1] == body[1] &&
		 built[2] == body[2] && built[3] == body[3],
		 "  body[0:4] (send/ack) is untouched here too -- "
		 "cnxman_envelope_stamp()'s job, not this wrapper's");
	ct_check((uint16_t)(built[4] | (built[5] << 8)) == req_env.txn,
		 "  body[4:6] (txn) is the ANSWERED REQUEST's, echoed by this "
		 "wrapper (the DLM's reply never writes body[0:8])");
	ct_check((uint16_t)(built[6] | (built[7] << 8)) == req_env.token,
		 "  and so is body[6:8] (token)");
	ct_check(vms_cm_body_build(req->bytes, req->wire_len, body,
				   VMS_CM_BODY_LEN - 1, built, sizeof(built),
				   &written) == VMS_CODEC_E_INVAL,
		 "  a short body is rejected rather than tail-padded with "
		 "whatever the caller's buffer held");
}

static void test_open_bitmap_span(void)
{
	const struct vms_fixture *add = fixture("cm-open-add-req");
	const struct vms_fixture *other = fixture("cm-commit-req");
	struct vms_frame_info fi;
	uint8_t span[VMS_CM_BITMAP_SPAN_LEN];
	uint32_t i, residual = 0;

	printf("-- vms_cm_open_bitmap_span (the UNDETERMINED-width evidence)\n");
	ct_check(add != NULL, "cm-open-add-req fixture present");
	if (add == NULL)
		return;

	ct_check(vms_frame_classify(add->bytes, add->wire_len, &fi)
		 == VMS_CODEC_OK, "classifies");
	ct_check(vms_cm_open_bitmap_span(add->bytes, add->wire_len, &fi, span)
		 == VMS_CODEC_OK, "reads the span from an op-0x09 open");
	ct_check_eq_u32(span[VMS_CM_BITMAP_SPAN_IDX], 0x0e,
			"  body[55] sits at VMS_CM_BITMAP_SPAN_IDX");
	for (i = 0; i < VMS_CM_BITMAP_SPAN_LEN; i++) {
		if (i != VMS_CM_BITMAP_SPAN_IDX && span[i] != 0u)
			residual++;
	}
	ct_check_eq_u32(residual, 0,
			"  and every other byte of body[52:60] is zero, as in "
			"all 54 library opens (spec sec 4(p))");

	if (other != NULL) {
		ct_check(vms_cm_open_bitmap_span(other->bytes, other->wire_len,
						 &fi, span)
			 == VMS_CODEC_E_CLASS,
			 "  refused on an opcode that carries no bitmap -- its "
			 "residue is not membership");
	}
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_cm: connection-manager codec entries (FC-P3.1)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	printf("-- cat 0x01 echo-recipe family (spec sec 4(p)/(r))\n");
	test_echo_recipe("cm-open-add-req", "cm-open-add-resp", 0,
			 "cm-open-add (op 0x09)");
	test_echo_recipe("cm-commit-req", "cm-commit-resp", 0,
			 "cm-commit (op 0x03)");
	test_echo_recipe("cm-op0f-req", "cm-op0f-resp", 0,
			 "cm-op0f (op 0x0f, echo not force)");
	test_echo_recipe("cm-relay-req", "cm-relay-resp", 0x04,
			 "cm-relay (op 0x12)");
	test_close_recipe();
	test_dlm_op0d_recipe();
	test_ack_build();

	test_allowlist();

	test_open_parse();
	test_barrier_parse();
	test_params_parse();
	test_model_parse();
	test_dlm_rebuild_parse();

	test_barrier_build();      /* FC-P3.5 */
	test_body_build();         /* FC-P3.5 */
	test_open_bitmap_span();   /* FC-P3.5 */

	test_joiner_originations();     /* FC-P3.3 */
	test_membership_find_sysid();   /* FC-P3.3 */

	test_error_paths();

	return ct_summary("test_codec_cm");
}
