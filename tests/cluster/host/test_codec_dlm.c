// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_dlm.c - cat-0x02 (DLM) codec entries, rung R1 (FC-P4.5).
 *
 * Four groups:
 *   1. Fixture round trip: ENQ request/grant/deny/CONVERT -- parse each
 *      into the typed struct, build back from ONLY the typed fields (never
 *      the fixture buffer), and assert every CITED byte of the DLM body
 *      span (abs 72-204) is reproduced exactly. The shared header/envelope
 *      span (abs 0-71) is deliberately left uncited in every fixture --
 *      this item's builders never touch it (see the header doc comment's
 *      division of labour).
 *   2. The op-0d rebuild-record echo recipe: parse the request fixture,
 *      build the response from it plus two envelope counters, and assert
 *      the built body matches the response fixture's CITED bytes exactly
 *      -- proving the "memcpy 132 + four mutations, nothing else" recipe
 *      byte-for-byte, spec §4(p).
 *   3. The allowlist rows this item contributes validate structurally
 *      (vms_wire_allow_table_validate) and resolve the grounded ops.
 *   4. THE HARD-LESSON TEST: no completion/commit builder accepts a
 *      literal/placeholder (zero) lock id -- the fc8540ae INVLOCKID
 *      crash, encoded as a permanent regression test.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_dlm.h"

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

/*
 * `start` scopes the comparison to the span a builder actually writes.
 * Every fixture also carries a plausible header/envelope prefix (abs
 * 0-71) purely so vms_frame_classify() succeeds -- those bytes ARE cited
 * (the specimen format has no "write but don't cite" mode), but no
 * builder in this file touches them (the header doc comment's division
 * of labour: this item owns only the DLM SYSAP body, abs 72-204), so
 * comparing them against the poison-filled `built` buffer would fail for
 * a reason that has nothing to do with this item's correctness. Starting
 * at VMS_OFF_SYSAP_BODY keeps the proof honest without pretending those
 * header bytes are cited-and-unchecked.
 */
static void assert_cited_bytes_match(const struct vms_fixture *f,
				     const uint8_t *built, uint32_t start,
				     uint32_t n, const char *label)
{
	uint32_t i, checked = 0, mismatches = 0;
	char what[224];

	for (i = start; i < n; i++) {
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

/* ---- group 1: ENQ/CONVERT fixture round trip -------------------------- */

static void test_enq_request_pw(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-request-pw");
	struct vms_frame_info fi;
	struct vms_dlm_enq_request req;
	uint8_t opcode = 0;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-enq-request-pw: ENQ op 0x01, mode PW (spec 4(f).1)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(fi.cls == VMS_FCLS_SCS_MSG, "  classified as VMS_FCLS_SCS_MSG");

	ct_check(vms_dlm_enq_request_parse(f->bytes, f->wire_len, &fi,
					   &opcode, &req) == VMS_CODEC_OK,
		 "parses as an ENQ/CONVERT request");
	ct_check_eq_u32(opcode, VMS_DLM_OP_ENQ, "  opcode == ENQ (0x01)");
	ct_check_eq_u32(req.mode, VMS_LCK_PW, "  mode == PW (4)");
	ct_check_eq_u32(req.req_pid_or_lkid, 0x2020021cu,
			"  req_pid == the GROUNDED interactive-process constant");
	ct_check_eq_u32(req.master_lkid, 0, "  master_lkid == 0 (fresh ENQ)");
	ct_check_eq_u32(req.name_len, 8, "  name_len == 8");
	ct_check(memcmp(req.name, "OVMXAAAA", 8) == 0, "  name == \"OVMXAAAA\"");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_enq_request_build(&req, opcode, built, sizeof(built),
					   &written) == VMS_CODEC_OK,
		 "builds back from the typed struct");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len, "dlm-enq-request-pw");
}

static void test_enq_grant(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-grant");
	struct vms_frame_info fi;
	struct vms_dlm_enq_response resp;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-enq-grant: GRANTED shape (spec 4(f).1 \"Completion status\")\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_enq_response_parse(f->bytes, f->wire_len, &fi, &resp)
		 == VMS_CODEC_OK, "parses as an ENQ/CONVERT response");
	ct_check(resp.outcome == VMS_DLM_ENQ_GRANTED,
		 "  discriminated as GRANTED (mode!=0, no name echoed)");
	ct_check_eq_u32(resp.req_lkid, 0x310000ABu,
			"  req_lkid == the GROUNDED SDA-confirmed handle");
	ct_check_eq_u32(resp.master_lkid, 0x520006AFu,
			"  master_lkid == the GROUNDED SDA-confirmed handle");
	ct_check_eq_u32(resp.granted_mode, VMS_LCK_PW, "  granted_mode == PW");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_enq_response_build_grant(resp.req_lkid, resp.master_lkid,
						  resp.granted_mode, built,
						  sizeof(built), &written)
		 == VMS_CODEC_OK, "builds back from the typed fields");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len, "dlm-enq-grant");
}

static void test_enq_deny(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-deny");
	struct vms_frame_info fi;
	struct vms_dlm_enq_response resp;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-enq-deny: DENIED shape (SS$_NOTQUEUED, spec 4(f).1)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_enq_response_parse(f->bytes, f->wire_len, &fi, &resp)
		 == VMS_CODEC_OK, "parses as an ENQ/CONVERT response");
	ct_check(resp.outcome == VMS_DLM_ENQ_DENIED,
		 "  discriminated as DENIED (mode==0, name echoed)");
	ct_check_eq_u32(resp.req_lkid, 0x2020021cu,
			"  req_lkid == the request's PID placeholder, UNCHANGED");
	ct_check_eq_u32(resp.granted_mode, 0, "  mode CLEARED to 0");
	ct_check_eq_u32(resp.name_len, 8, "  name_len == 8, echoed");
	ct_check(memcmp(resp.name, "OVMXAAAA", 8) == 0,
		 "  name == \"OVMXAAAA\", echoed verbatim");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_enq_response_build_deny(resp.req_lkid, resp.master_lkid,
						 resp.name_len, resp.name, built,
						 sizeof(built), &written)
		 == VMS_CODEC_OK, "builds back from the typed fields");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len, "dlm-enq-deny");
}

static void test_convert_request(void)
{
	const struct vms_fixture *f = fixture("dlm-convert-request");
	struct vms_frame_info fi;
	struct vms_dlm_enq_request req;
	uint8_t opcode = 0;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-convert-request: CONVERT op 0x07, NL->EX (spec 4(f).1, ac4-CVT)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_enq_request_parse(f->bytes, f->wire_len, &fi,
					   &opcode, &req) == VMS_CODEC_OK,
		 "parses as an ENQ/CONVERT request");
	ct_check_eq_u32(opcode, VMS_DLM_OP_CONVERT, "  opcode == CONVERT (0x07)");
	ct_check_eq_u32(req.mode, VMS_LCK_EX, "  new mode == EX (5)");
	ct_check_eq_u32(req.req_pid_or_lkid, 0x5000038Au,
			"  body[20] == the EXISTING local lock-id (not a PID)");
	ct_check_eq_u32(req.master_lkid, 0x120004B9u,
			"  master_lkid == the established RSB handle");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_enq_request_build(&req, opcode, built, sizeof(built),
					   &written) == VMS_CODEC_OK,
		 "builds back from the typed struct");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len, "dlm-convert-request");
}

static void test_fixture_roundtrips(void)
{
	test_enq_request_pw();
	test_enq_grant();
	test_enq_deny();
	test_convert_request();
}

/* ---- group 2: op-0d rebuild-record echo recipe ------------------------ */

static void test_rebuild_echo_recipe(void)
{
	const struct vms_fixture *req_f = fixture("dlm-rebuild-request");
	const struct vms_fixture *resp_f = fixture("dlm-rebuild-response");
	struct vms_frame_info fi;
	struct vms_dlm_rebuild_record rec;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-rebuild-request/response: op 0x0d echo recipe (spec 4(p))\n");
	ct_check(req_f != NULL && resp_f != NULL, "both fixtures load");
	if (req_f == NULL || resp_f == NULL)
		return;

	ct_check(vms_frame_classify(req_f->bytes, req_f->wire_len, &fi)
		 == VMS_CODEC_OK, "request classifies without error");
	ct_check(vms_dlm_rebuild_parse(req_f->bytes, req_f->wire_len, &fi, &rec)
		 == VMS_CODEC_OK, "parses as a rebuild record (invariants hold)");
	ct_check_eq_u32(rec.name_len, 10, "  name_len == 10");
	ct_check(memcmp(rec.name, "SYS$SYS_ID", 10) == 0,
		 "  name == \"SYS$SYS_ID\" (spec-4(p)-GROUNDED observed string)");

	memset(built, 0xAA, sizeof(built));
	/* own_send_msg=5, ack_of_peer_send=7 (the request's own send-msg#,
	 * per dlm-rebuild-response.spec's header comment). */
	ct_check(vms_dlm_rebuild_response_build(&rec, 5, 7, built, sizeof(built),
						&written) == VMS_CODEC_OK,
		 "builds the response by the spec's own recipe");
	ct_check_eq_u32(written, VMS_DLM_REBUILD_ECHO_LEN,
			"  reports the 132-byte body span written");

	/* Compare against the RESPONSE fixture, not the request -- proves the
	 * recipe's four mutations landed and nothing else changed, byte for
	 * byte against an independently-authored specimen. */
	assert_cited_bytes_match(resp_f, built, VMS_OFF_SYSAP_BODY, resp_f->wire_len,
				 "dlm-rebuild-response");
}

/* ---- group 3: allowlist rows ------------------------------------------ */

static void test_allowlist_rows(void)
{
	const struct vms_wire_allow_entry *e;

	printf("-- DLM allowlist rows: structural validation + lookup\n");
	ct_check(vms_wire_allow_table_validate(&vms_dlm_allow_table) == VMS_CODEC_OK,
		 "vms_dlm_allow_table validates (no dup keys, no response-bit "
		 "categories, every row cites the spec)");

	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, VMS_DLM_OP_ENQ);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_RESPOND,
		 "op 0x01 (ENQ) resolves to RESPOND");

	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, VMS_DLM_OP_REBUILD);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_RESPOND,
		 "op 0x0d (rebuild) resolves to RESPOND");

	/* The PROVISIONAL completion/commit ops are DELIBERATELY absent --
	 * the allowlist asserts "grounded in the reference" (spec §4(p)),
	 * which the completion body is not. */
	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST,
				VMS_DLM_OP_COMPLETE_PROVISIONAL);
	ct_check(e == NULL,
		 "op 0x04 (PROVISIONAL completion) is NOT in the allowlist");
}

/* ---- group 4: THE HARD-LESSON TEST ------------------------------------ */

/*
 * fc8540ae (operator memory cluster-promotion-gap.md pm(15)): a DLM
 * completion frame carrying a PLACEHOLDER lock id at this field bugchecked
 * a real VAX with `Fatal BUG CHECK INVLOCKID, Invalid lock id` and took
 * the whole cluster down. This test is the permanent regression guard: no
 * builder in this file may accept VMS_DLM_LKID_UNSET (0) in a lock-id
 * field, in either position, for either op.
 */
static void test_no_builder_accepts_a_placeholder_lock_id(void)
{
	struct vms_dlm_completion c;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- THE HARD LESSON: no completion/commit builder accepts a "
	       "placeholder lock id (fc8540ae INVLOCKID crash, regression-locked)\n");

	memset(&c, 0, sizeof(c));
	c.master_lkid = 0x00020017u; /* a plausible real LKB handle */
	c.req_lkid = 0x00010042u;
	c.name_len = 8;
	memcpy(c.name, "OVMXAAAA", 8);

	/* Sanity: a completion with two REAL nonzero ids builds fine, on
	 * both ops -- proves the refusal below is about the zero, not a
	 * blanket rejection. */
	ct_check(vms_dlm_completion_build(&c, VMS_DLM_OP_COMPLETE_PROVISIONAL,
					  built, sizeof(built), &written)
		 == VMS_CODEC_OK,
		 "  a completion with two real nonzero lock ids builds");
	ct_check(vms_dlm_completion_build(&c, VMS_DLM_OP_COMMIT_PROVISIONAL,
					  built, sizeof(built), &written)
		 == VMS_CODEC_OK,
		 "  a commit with two real nonzero lock ids builds");

	/* The fc8540ae shape: master_lkid == the placeholder 0. */
	c.master_lkid = VMS_DLM_LKID_UNSET;
	c.req_lkid = 0x00010042u;
	ct_check(vms_dlm_completion_build(&c, VMS_DLM_OP_COMPLETE_PROVISIONAL,
					  built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  master_lkid==0 REFUSED on completion (op 0x04)");
	ct_check(vms_dlm_completion_build(&c, VMS_DLM_OP_COMMIT_PROVISIONAL,
					  built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  master_lkid==0 REFUSED on commit (op 0x03)");

	/* req_lkid==0 is refused too -- both lock-id fields carry the same
	 * "not a real lock" sentinel. */
	c.master_lkid = 0x00020017u;
	c.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_completion_build(&c, VMS_DLM_OP_COMPLETE_PROVISIONAL,
					  built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  req_lkid==0 REFUSED on completion (op 0x04)");

	/* Both zero at once -- must not builds "because they cancel out". */
	c.master_lkid = VMS_DLM_LKID_UNSET;
	c.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_completion_build(&c, VMS_DLM_OP_COMPLETE_PROVISIONAL,
					  built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  both lock ids 0 REFUSED");

	/* The GRANT builder carries the same guard on req_lkid (the value
	 * this codec is about to hand a peer as "the lock-id I assigned
	 * you"; a zero there is the same class of lie). */
	ct_check(vms_dlm_enq_response_build_grant(VMS_DLM_LKID_UNSET,
						  0x00020017u, VMS_LCK_PW,
						  built, sizeof(built), &written)
		 == VMS_CODEC_E_INVAL,
		 "  vms_dlm_enq_response_build_grant refuses req_lkid==0");
}

/*
 * FC-P4.3: the directory hash at body[10:12]. There is a PARSER and there is
 * deliberately NO BUILDER, and this test asserts both halves -- the second one
 * by the only means available for an absent function: the parse of a value the
 * caller could not have produced, and a link-time absence a reviewer can see.
 *
 * The offset is INFERRED (see the header) until FC-P4.2 confirms it offline,
 * so what is asserted here is the ACCESSOR's behaviour, not the field's
 * meaning: it reads the two bytes at abs 82 little-endian out of a cat-0x02
 * frame, refuses a frame of the wrong class, and writes nothing when it
 * refuses.
 */
static void test_dir_hash_accessor(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-request-pw");
	struct vms_frame_info fi;
	uint16_t hash = 0xFFFFu;
	uint8_t frame[256];

	printf("-- the directory hash at body[10:12] (FC-P4.3, p. 6-50)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;
	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies");

	ct_check(vms_dlm_dir_hash_parse(f->bytes, f->wire_len, &fi, &hash) ==
		 VMS_CODEC_OK, "the hash field is readable on a real cat-02 frame");
	ct_check_eq_u32(hash,
			(uint32_t)f->bytes[VMS_OFF_DLM_DIR_HASH] |
			((uint32_t)f->bytes[VMS_OFF_DLM_DIR_HASH + 1u] << 8),
			"  and it is abs 82/83 read little-endian, nothing else");

	/* A value no name-derived function would produce, read back verbatim:
	 * the accessor transports, it does not derive. */
	memcpy(frame, f->bytes, f->wire_len);
	frame[VMS_OFF_DLM_DIR_HASH] = 0x34u;
	frame[VMS_OFF_DLM_DIR_HASH + 1u] = 0x12u;
	hash = 0;
	ct_check(vms_dlm_dir_hash_parse(frame, f->wire_len, &fi, &hash) ==
		 VMS_CODEC_OK, "reads an arbitrary wire value");
	ct_check_eq_u32(hash, 0x1234u, "  byte for byte, whatever the wire said");

	/* Refusals write nothing: "the frame carried no hash" and "the hash is
	 * 0" are different facts, and only one of them may reach the wire. */
	hash = 0xA5A5u;
	ct_check(vms_dlm_dir_hash_parse(frame, f->wire_len, NULL, &hash) ==
		 VMS_CODEC_E_CLASS, "a frame with no class info is refused");
	ct_check_eq_u32(hash, 0xA5A5u, "  and the caller's variable is untouched");
	ct_check(vms_dlm_dir_hash_parse(frame, f->wire_len, &fi, NULL) ==
		 VMS_CODEC_E_CLASS, "a null output is refused");

	{
		struct vms_frame_info wrong = fi;

		wrong.cls = VMS_FCLS_HELLO;
		hash = 0xA5A5u;
		ct_check(vms_dlm_dir_hash_parse(frame, f->wire_len, &wrong,
						&hash) == VMS_CODEC_E_CLASS,
			 "a non-SCS_MSG frame is refused");
		ct_check_eq_u32(hash, 0xA5A5u, "  writing nothing");
	}

	{
		/* A frame that is not cat-0x02 at all. */
		uint8_t other[256];

		memcpy(other, frame, f->wire_len);
		other[VMS_OFF_DLM_CAT] = 0x01u;
		hash = 0xA5A5u;
		ct_check(vms_dlm_dir_hash_parse(other, f->wire_len, &fi,
						&hash) == VMS_CODEC_E_CLASS,
			 "a cat-0x01 body is refused");
		ct_check_eq_u32(hash, 0xA5A5u, "  writing nothing");
	}

	/* A truncated frame reports the view's error, not a zero. */
	hash = 0xA5A5u;
	ct_check(vms_dlm_dir_hash_parse(frame, VMS_OFF_DLM_DIR_HASH + 1u, &fi,
					&hash) != VMS_CODEC_OK,
		 "a frame too short to hold the field is refused");
	ct_check_eq_u32(hash, 0xA5A5u, "  writing nothing");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_dlm: cat-0x02 DLM codec entries (FC-P4.5)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_fixture_roundtrips();
	test_rebuild_echo_recipe();
	test_allowlist_rows();
	test_no_builder_accepts_a_placeholder_lock_id();
	test_dir_hash_accessor();

	return ct_summary("test_codec_dlm");
}
