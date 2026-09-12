// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_dlm.c - cat-0x02 (DLM) codec entries, rung R1 (FC-P4.5).
 *
 * Four groups:
 *   1. Fixture round trip: ENQ request/grant/deny/CONVERT, and the three
 *      vms-c03 ops ($DEQ 0x03, BLKAST 0x04, value-block CONVERT 0x06) -- parse each
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
 *   4. THE HARD-LESSON TEST: no lock-id-bearing builder accepts a
 *      literal/placeholder (zero) lock id, and the lock-id-ONLY messages
 *      refuse one on the PARSE side too -- the fc8540ae INVLOCKID crash,
 *      encoded as a permanent regression test. The frames it used to cover
 *      (the PROVISIONAL completion/commit pair) turned out not to exist; the
 *      guard moved onto the real ops that live at those opcodes.
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
	ct_check_eq_u32(opcode, VMS_DLM_WIREOP_ENQ, "  opcode == ENQ (0x01)");
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
	ct_check_eq_u32(opcode, VMS_DLM_WIREOP_CONVERT, "  opcode == CONVERT (0x07)");
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

/* ---- group 1b: the vms-c03 ops -- $DEQ, BLKAST, value-block CONVERT ---- */

/*
 * op 0x03 = $DEQ. The fixture's every cited byte IS a byte of
 * dlm-deq-20260911.pcap f14 (its .spec header carries the frame-by-frame
 * correlation), so parsing it and building it back proves the field map
 * against a real OpenVMS VAX 7.3 cluster's own release frame.
 */
static void test_deq_release(void)
{
	const struct vms_fixture *f = fixture("dlm-deq-release");
	struct vms_frame_info fi;
	struct vms_dlm_deq d;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-deq-release: op 0x03 is $DEQ (vms-c03, f14 of "
	       "dlm-deq-20260911.pcap)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_deq_parse(f->bytes, f->wire_len, &fi, &d) ==
		 VMS_CODEC_OK, "parses as a $DEQ");
	ct_check_eq_u32(d.master_lkid, 0x3a0004ebu,
			"  body[24:28] == 0x3a0004eb, the master handle the "
			"driving ENQ for 'OVMXDEQ1' carried");
	ct_check_eq_u32(d.req_lkid, 0x080001cdu,
			"  body[20:24] == 0x080001cd, the handle the GRANT "
			"assigned this requester");
	ct_check_eq_u32(d.mode, VMS_LCK_NL, "  body[30] == NL, the released mode");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_deq_build(&d, built, sizeof(built), &written) ==
		 VMS_CODEC_OK, "builds back from the typed struct");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len,
				 "dlm-deq-release");

	/* A real $DEQ carries no resource name, so the builder must leave the
	 * name span alone. Checked positively, not just by the fixture's
	 * silence: the poison byte survives where an ENQ would have written
	 * the 0x03 marker. */
	ct_check_eq_u32(built[VMS_OFF_DLM_NAME_MARKER], 0xAAu,
			"*** the builder writes NO name marker: a $DEQ names "
			"its lock by lock-id, and body[46] is not a field ***");
}

/*
 * op 0x04 = BLKAST, and THE GUARD: the reference frame has a readable
 * resource name at body[48] that belongs to a DIFFERENT lock. The codec must
 * have no way to surface it. That is asserted here the only way an absent
 * field can be: the struct has no name member (a compile-time fact a reviewer
 * can see) and the builder leaves the span untouched (a runtime fact).
 */
static void test_blkast(void)
{
	const struct vms_fixture *f = fixture("dlm-blkast");
	struct vms_frame_info fi;
	struct vms_dlm_blkast b;
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- dlm-blkast: op 0x04 is BLKAST (vms-c03, f58 of "
	       "dlm-blk2-20260911.pcap)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_blkast_parse(f->bytes, f->wire_len, &fi, &b) ==
		 VMS_CODEC_OK, "parses as a BLKAST");
	ct_check_eq_u32(b.master_lkid, 0x590004e3u,
			"  body[24:28] == 0x590004e3, the master handle the EX "
			"holder's ENQ for 'OVMXBLK2' carried");
	ct_check_eq_u32(b.req_lkid, 0x0a0003afu,
			"  body[20:24] == 0x0a0003af, the holder's own handle");

	/* OBSERVED, not pinned -- asserted as "the two bytes the peer sent",
	 * which is all the codec claims about them. */
	ct_check(b.mode_ctx_valid == 1u && b.mode_ctx[0] == 0x01u &&
		 b.mode_ctx[1] == 0x05u,
		 "  body[30:32] is reported verbatim (OBSERVED 0x01,0x05 -- the "
		 "codec does not claim to know what the pair means)");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_blkast_build(&b, built, sizeof(built), &written) ==
		 VMS_CODEC_OK, "builds back from the typed struct");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len,
				 "dlm-blkast");

	/* *** THE INV-6 GUARD. *** */
	ct_check_eq_u32(built[VMS_OFF_DLM_NAME_MARKER], 0xAAu,
			"*** no name marker is built: the reference BLKAST's "
			"body[48] 'F11B$aSYSDSK1' is STALE BUFFER, not this "
			"frame's resource ***");
	ct_check_eq_u32(built[VMS_OFF_DLM_NAME], 0xAAu,
			"*** and no name byte either ***");

	/* The OBSERVED pair is OPT-IN: a caller with no real executive values
	 * for it writes nothing there, exactly like the directory hash. */
	b.mode_ctx_valid = 0u;
	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_blkast_build(&b, built, sizeof(built), &written) ==
		 VMS_CODEC_OK, "builds with mode_ctx_valid clear");
	ct_check(built[VMS_OFF_DLM_BLKAST_MODE_CTX] == 0xAAu &&
		 built[VMS_OFF_DLM_BLKAST_MODE_CTX + 1u] == 0xAAu,
		 "*** and writes NOTHING at body[30:32]: an OBSERVED field is "
		 "omitted honestly, never defaulted ***");
}

/*
 * op 0x06 = the CONVERT that carries the lock value block. The proof is the
 * driver's own 16-byte pattern appearing verbatim at body[36:52].
 */
static void test_valblk_convert(void)
{
	const struct vms_fixture *f = fixture("dlm-valblk-convert");
	struct vms_frame_info fi;
	struct vms_dlm_valblk_convert c;

	printf("-- dlm-valblk-convert: op 0x06 carries the LVB (vms-c03, f14 of "
	       "dlm-lvb3-20260911.pcap)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_valblk_convert_parse(f->bytes, f->wire_len, &fi, &c) ==
		 VMS_CODEC_OK, "parses as a value-block CONVERT");
	ct_check_eq_u32(c.master_lkid, 0x2b000489u,
			"  body[24:28] == 0x2b000489, the master handle the "
			"driving ENQ for 'OVMXLVB3' carried");
	ct_check_eq_u32(c.req_lkid, 0x270001cdu,
			"  body[20:24] == the handle the GRANT assigned");
	ct_check_eq_u32(c.mode, VMS_LCK_NL,
			"  body[30] == NL: this is the convert DOWN from EX");
	ct_check(memcmp(c.valblk, "WROTEBYVAX1XXXXX",
			VMS_DLM_VALBLK_WIRE_LEN) == 0,
		 "*** body[36:52] IS the 16 bytes the driver wrote at LKSB+8, "
		 "verbatim off a real wire ***");
	ct_check_eq_u32(c.serial, 0x1fu,
			"  body[32] SERIAL == 0x1f (per-lock, == body[52]); "
			"grounded vms-727, no longer un-pinned");

	/*
	 * THE BYTE-IDENTICAL BUILD PROOF (vms-727). Build an op-0x06 frame back
	 * from ONLY the typed struct and assert every CITED byte of this real
	 * capture is reproduced exactly -- the strongest form of grounding:
	 * the builder emits the real wire, byte for byte, not an assertion that
	 * it would. Poison the whole buffer first so anything the builder does
	 * NOT write shows up as a mismatch on a cited byte.
	 */
	{
		uint8_t built[256];
		uint32_t written = 0;
		uint32_t i;
		int tail_all_zero = 1;

		memset(built, 0xAA, sizeof(built));
		ct_check(vms_dlm_valblk_convert_build(&c, built, sizeof(built),
						      &written) == VMS_CODEC_OK,
			 "builds an op-0x06 back from the typed struct");
		assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY,
					 f->wire_len, "dlm-valblk-convert");
		ct_check_eq_u32(built[VMS_OFF_DLM_VALBLK_SERIAL],
				built[VMS_OFF_DLM_VALBLK_SERIAL2],
				"*** the SERIAL is stamped identically at "
				"body[32] and body[52] (front == back) ***");
		/* body[56:88] is stale sender buffer on the wire; the builder
		 * must emit clean ZEROS there, never our own memory -- checked
		 * positively against the 0xAA poison. */
		for (i = VMS_OFF_SYSAP_BODY + 56u;
		     i < VMS_OFF_SYSAP_BODY + VMS_DLM_VALBLK_BODY_LEN; i++)
			if (built[i] != 0x00u)
				tail_all_zero = 0;
		ct_check(tail_all_zero == 1,
			 "*** body[56:88] is zero-filled, NOT the VAX stack "
			 "garbage the real sender pads with (anti-stale-buffer) ***");
		ct_check_eq_u32(written, VMS_OFF_SYSAP_BODY + VMS_DLM_VALBLK_BODY_LEN,
				"  written length is the full op-0x06 body");
	}

	/*
	 * An op-0x07 CONVERT is NOT an op-0x06: the two are the same family and
	 * different semantics, and conflating them would make the codec read a
	 * value block out of a frame that carries none.
	 */
	{
		const struct vms_fixture *cv = fixture("dlm-convert-request");
		struct vms_frame_info cfi;
		struct vms_dlm_valblk_convert junk;

		if (cv != NULL &&
		    vms_frame_classify(cv->bytes, cv->wire_len, &cfi) ==
			    VMS_CODEC_OK) {
			memset(&junk, 0xA5, sizeof(junk));
			ct_check(vms_dlm_valblk_convert_parse(cv->bytes,
							      cv->wire_len,
							      &cfi, &junk) ==
				 VMS_CODEC_E_CLASS,
				 "an op-0x07 CONVERT is REFUSED by the op-0x06 "
				 "accessor (same family, different semantics)");
			ct_check_eq_u32(junk.master_lkid, 0xA5A5A5A5u,
					"  and the caller's struct is untouched");
		}
	}
}

/*
 * op 0x01 GRANT that RETURNS THE MASTER'S VALUE BLOCK -- the LVB READ crossing
 * (vms-727). The proof is the same 16-byte pattern the requester wrote earlier
 * coming BACK in the master's grant at body[36:52], and the builder reproducing
 * the real grant-with-valblk frame byte for byte.
 */
static void test_grant_valblk(void)
{
	const struct vms_fixture *f = fixture("dlm-grant-valblk");
	struct vms_frame_info fi;
	struct vms_dlm_enq_response resp;

	printf("-- dlm-grant-valblk: op 0x01 GRANT carries the LVB back "
	       "(vms-727, c2-seq.pcap grant reply)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_enq_response_parse(f->bytes, f->wire_len, &fi, &resp) ==
		 VMS_CODEC_OK, "parses as an ENQ response");
	ct_check(resp.outcome == VMS_DLM_ENQ_GRANTED,
		 "  the value-block grant is a GRANT, not misread as a DENY "
		 "(mode is NL but the record marker resolves the shape)");
	ct_check(resp.valblk_present == 1,
		 "  valblk_present == 1: the grant-with-valblk record was seen");
	ct_check(memcmp(resp.valblk, "WROTEBYVAX1XXXXX",
			VMS_DLM_VALBLK_WIRE_LEN) == 0,
		 "*** body[36:52] IS the 16 bytes the requester wrote, returned "
		 "by the master in the grant, verbatim off a real wire ***");
	ct_check_eq_u32(resp.req_lkid, 0x0a0003a4u,
			"  body[20:24] == 0x0a0003a4, the requester handle "
			"(SDA Lock id 0A0003A4)");
	ct_check_eq_u32(resp.master_lkid, 0x570001b7u,
			"  body[24:28] == 0x570001b7, the master handle for "
			"'OVMXLV01'");

	/*
	 * THE BYTE-IDENTICAL BUILD PROOF (vms-727). Build the grant back from
	 * ONLY req_lkid/master_lkid/mode + the 16-byte block and assert every
	 * CITED byte of the real capture is reproduced exactly. Poison the whole
	 * buffer first so anything the builder does NOT write shows as a mismatch.
	 */
	{
		uint8_t built[256];
		uint32_t written = 0;

		memset(built, 0xAA, sizeof(built));
		ct_check(vms_dlm_enq_response_build_grant_valblk(resp.req_lkid,
				resp.master_lkid, resp.granted_mode, resp.valblk,
				built, sizeof(built), &written) == VMS_CODEC_OK,
			 "builds the grant-with-valblk back from the typed fields");
		assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY,
					 f->wire_len, "dlm-grant-valblk");
		ct_check_eq_u32(written, VMS_OFF_SYSAP_BODY + VMS_DLM_VALBLK_BODY_LEN,
				"  written length is the full grant-valblk body");

		/* A grant-id of 0 is not a grant (the fc8540ae rule). */
		ct_check(vms_dlm_enq_response_build_grant_valblk(VMS_DLM_LKID_UNSET,
				resp.master_lkid, resp.granted_mode, resp.valblk,
				built, sizeof(built), &written) == VMS_CODEC_E_INVAL,
			 "  refuses req_lkid 0; refuses master_lkid 0 likewise");
		ct_check(vms_dlm_enq_response_build_grant_valblk(resp.req_lkid,
				VMS_DLM_LKID_UNSET, resp.granted_mode, resp.valblk,
				built, sizeof(built), &written) == VMS_CODEC_E_INVAL,
			 "  refuses master_lkid 0");
	}

	/*
	 * A PLAIN grant (dlm-enq-grant) carries NO value block: the record marker
	 * is absent, so the parser leaves valblk_present 0 and the requester's own
	 * block is left alone. This is the stale-buffer guard's positive control.
	 */
	{
		const struct vms_fixture *pg = fixture("dlm-enq-grant");
		struct vms_frame_info pfi;
		struct vms_dlm_enq_response pr;

		if (pg != NULL &&
		    vms_frame_classify(pg->bytes, pg->wire_len, &pfi) == VMS_CODEC_OK) {
			memset(&pr, 0xA5, sizeof(pr));
			ct_check(vms_dlm_enq_response_parse(pg->bytes, pg->wire_len,
							    &pfi, &pr) == VMS_CODEC_OK,
				 "a plain grant still parses");
			ct_check(pr.outcome == VMS_DLM_ENQ_GRANTED &&
				 pr.valblk_present == 0,
				 "*** a plain grant carries valblk_present == 0 "
				 "(no record marker -> the proxy block is left alone) ***");
		}
	}
}

/*
 * THE SUPERSESSION, asserted rather than merely documented. The phantom ops
 * are gone as VALUES: 0x03 and 0x04 now mean $DEQ and BLKAST, and a parser for
 * one refuses the other's frame. A tree that quietly kept a "completion 0x04"
 * alive somewhere would show up here as a parse that succeeds when it must
 * not.
 */
static void test_superseded_opcodes_are_one_meaning_each(void)
{
	const struct vms_fixture *deq = fixture("dlm-deq-release");
	const struct vms_fixture *blk = fixture("dlm-blkast");
	struct vms_frame_info fi;
	struct vms_dlm_deq d;
	struct vms_dlm_blkast b;
	uint8_t op = 0;
	struct vms_dlm_enq_request req;

	printf("-- one opcode, one meaning: 0x03 is $DEQ and 0x04 is BLKAST, "
	       "and neither is anything else\n");
	if (deq == NULL || blk == NULL) {
		ct_check(0, "both fixtures load");
		return;
	}

	ct_check(VMS_DLM_WIREOP_DEQ == 0x03u && VMS_DLM_WIREOP_BLKAST == 0x04u &&
		 VMS_DLM_WIREOP_CONVERT_VALBLK == 0x06u,
		 "the opcode values are the captured ones");

	(void)vms_frame_classify(deq->bytes, deq->wire_len, &fi);
	ct_check(vms_dlm_blkast_parse(deq->bytes, deq->wire_len, &fi, &b) ==
		 VMS_CODEC_E_CLASS,
		 "the BLKAST parser REFUSES a $DEQ frame");
	ct_check(vms_dlm_enq_request_parse(deq->bytes, deq->wire_len, &fi, &op,
					   &req) == VMS_CODEC_E_CLASS,
		 "and so does the ENQ/CONVERT parser");

	(void)vms_frame_classify(blk->bytes, blk->wire_len, &fi);
	ct_check(vms_dlm_deq_parse(blk->bytes, blk->wire_len, &fi, &d) ==
		 VMS_CODEC_E_CLASS,
		 "the $DEQ parser REFUSES a BLKAST frame");
}

static void test_fixture_roundtrips(void)
{
	test_enq_request_pw();
	test_enq_grant();
	test_enq_deny();
	test_convert_request();
	test_deq_release();
	test_blkast();
	test_valblk_convert();
	test_grant_valblk();
	test_superseded_opcodes_are_one_meaning_each();
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
				VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_ENQ);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_RESPOND,
		 "op 0x01 (ENQ) resolves to RESPOND");

	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_REBUILD);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_RESPOND,
		 "op 0x0d (rebuild) resolves to RESPOND");

	/*
	 * The three vms-c03 ops joined the table when a real cluster's own
	 * frames grounded them. They are CONSUME, not RESPOND: the capture set
	 * contains no cat-0x82 reply to any of the three, so a RESPOND row
	 * would be asserting a response recipe nobody has ever seen.
	 */
	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_DEQ);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME &&
		 e->recipe == 0u,
		 "op 0x03 ($DEQ) resolves to CONSUME with no response recipe");

	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, VMS_DLM_WIREOP_BLKAST);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME &&
		 e->recipe == 0u,
		 "op 0x04 (BLKAST) resolves to CONSUME with no response recipe");

	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST,
				VMS_DLM_WIREOP_CONVERT_VALBLK);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME &&
		 e->recipe == 0u,
		 "op 0x06 (value-block CONVERT) resolves to CONSUME");

	/* Still ungrounded, still absent: an allowlist row asserts "grounded in
	 * the reference", and op 0x05 / 0x09 / 0x0a appear in the vms-c03
	 * captures without any correlation that says what they mean. */
	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, 0x05u);
	ct_check(e == NULL, "op 0x05 (seen but ungrounded) is NOT in the table");
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
	struct vms_dlm_deq d;
	struct vms_dlm_blkast b;
	struct vms_dlm_valblk_convert c;
	const struct vms_fixture *f;
	uint8_t poisoned[256];
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- THE HARD LESSON: no lock-id-bearing builder accepts a "
	       "placeholder lock id (fc8540ae INVLOCKID crash, "
	       "regression-locked across the supersession)\n");

	/*
	 * THE PHANTOM IS GONE, THE GUARD IS NOT. The frames this test used to
	 * cover -- the PROVISIONAL "completion 0x04 / commit 0x03" pair -- do
	 * not exist on a real wire and no longer exist in this codec. The
	 * lesson they taught does: the $DEQ and the BLKAST that really do live
	 * at those opcodes are lock-id-ONLY messages, so a placeholder there is
	 * strictly worse than it was on a completion.
	 */
	memset(&d, 0, sizeof(d));
	d.master_lkid = 0x00020017u;   /* plausible real LKB handles */
	d.req_lkid = 0x00010042u;
	d.mode = VMS_LCK_NL;
	ct_check(vms_dlm_deq_build(&d, built, sizeof(built), &written) ==
		 VMS_CODEC_OK,
		 "  a $DEQ with two real nonzero lock ids builds");

	d.master_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_deq_build(&d, built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "  master_lkid==0 REFUSED on $DEQ (0x03)");
	d.master_lkid = 0x00020017u;
	d.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_deq_build(&d, built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "  req_lkid==0 REFUSED on $DEQ (0x03)");
	d.master_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_deq_build(&d, built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL,
		 "  both lock ids 0 REFUSED (they do not cancel out)");

	memset(&b, 0, sizeof(b));
	b.master_lkid = 0x00020017u;
	b.req_lkid = 0x00010042u;
	ct_check(vms_dlm_blkast_build(&b, built, sizeof(built), &written) ==
		 VMS_CODEC_OK,
		 "  a BLKAST with two real nonzero lock ids builds");
	b.master_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_blkast_build(&b, built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "  master_lkid==0 REFUSED on BLKAST (0x04)");
	b.master_lkid = 0x00020017u;
	b.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_blkast_build(&b, built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "  req_lkid==0 REFUSED on BLKAST (0x04)");

	/* A refused build writes NOTHING -- the caller's frame is untouched,
	 * so a caller that ignored the status cannot transmit a half-built
	 * frame carrying a real opcode and a zero lock id. */
	memset(built, 0xAA, sizeof(built));
	(void)vms_dlm_blkast_build(&b, built, sizeof(built), &written);
	ct_check_eq_u32(built[VMS_OFF_DLM_OP], 0xAAu,
			"*** a refused build wrote NO byte at all ***");

	/* The op-0x06 value-block CONVERT builder (vms-727) takes the same
	 * refusal: a value block naming lock 0 has nothing to attach to. */
	{
		struct vms_dlm_valblk_convert vc;

		memset(&vc, 0, sizeof(vc));
		vc.master_lkid = 0x00020017u;
		vc.req_lkid = 0x00010042u;
		vc.mode = VMS_LCK_NL;
		vc.serial = 0x1fu;
		ct_check(vms_dlm_valblk_convert_build(&vc, built, sizeof(built),
						      &written) == VMS_CODEC_OK,
			 "  an op-0x06 with two real lock ids builds");
		vc.master_lkid = VMS_DLM_LKID_UNSET;
		ct_check(vms_dlm_valblk_convert_build(&vc, built, sizeof(built),
						      &written) == VMS_CODEC_E_INVAL,
			 "  master_lkid==0 REFUSED on op-0x06 value-block CONVERT");
		vc.master_lkid = 0x00020017u;
		vc.req_lkid = VMS_DLM_LKID_UNSET;
		ct_check(vms_dlm_valblk_convert_build(&vc, built, sizeof(built),
						      &written) == VMS_CODEC_E_INVAL,
			 "  req_lkid==0 REFUSED on op-0x06 value-block CONVERT");
	}

	/*
	 * AND THE SAME REFUSAL ON THE PARSE SIDE, which the completion codec
	 * never had. These three ops name their lock by lock-id and by nothing
	 * else, so "the peer sent zero" may not be surfaced as "the peer named
	 * a lock". The vms-c03 blk capture really does contain cat-0x02 op-0x04
	 * frames with a zero there.
	 */
	f = fixture("dlm-blkast");
	if (f != NULL) {
		struct vms_frame_info fi;

		memcpy(poisoned, f->bytes, f->wire_len);
		memset(poisoned + VMS_OFF_DLM_MASTER_LKID, 0, 4);
		(void)vms_frame_classify(poisoned, f->wire_len, &fi);
		memset(&b, 0xA5, sizeof(b));
		ct_check(vms_dlm_blkast_parse(poisoned, f->wire_len, &fi, &b) !=
			 VMS_CODEC_OK,
			 "*** a BLKAST frame whose master_lkid is 0 is REFUSED "
			 "by the PARSER, not reported as lock 0 ***");
		ct_check_eq_u32(b.master_lkid, 0xA5A5A5A5u,
				"  and the caller's struct is untouched");
	}

	f = fixture("dlm-valblk-convert");
	if (f != NULL) {
		struct vms_frame_info fi;

		memcpy(poisoned, f->bytes, f->wire_len);
		memset(poisoned + VMS_OFF_DLM_REQ_LKID, 0, 4);
		(void)vms_frame_classify(poisoned, f->wire_len, &fi);
		memset(&c, 0xA5, sizeof(c));
		ct_check(vms_dlm_valblk_convert_parse(poisoned, f->wire_len,
						      &fi, &c) != VMS_CODEC_OK,
			 "a value-block CONVERT with an unset lock id is "
			 "REFUSED: an LVB with no lock to attach it to is not "
			 "a value block");
	}

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

/*
 * ==========================================================================
 * THE E73 REGRESSION GUARD (rd vms-1ee): a SYSAP BODY parses, and the
 * frame-absolute entry refuses the very same bytes.
 *
 * cnxman_vc_message() hands a SYSAP its own 132 bytes and nothing below them
 * (design sec 3.2.4). Handing those bytes to a frame-absolute parser is exactly
 * integration note E73 -- it does not fail loudly, it silently refuses every
 * real inbound message, which is how a live run lost a whole CM dialogue. This
 * pins BOTH halves: the body entry reads the same fields the frame entry does,
 * and the frame entry on a bare body is a refusal, not a misparse.
 * ==========================================================================
 */
static void test_body_entries_are_what_scs_delivers(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-request-pw");
	struct vms_frame_info fi;
	struct vms_dlm_enq_request from_frame, from_body;
	uint8_t op_frame = 0, op_body = 0;
	const uint8_t *body;
	uint32_t blen;
	uint16_t h_frame = 0, h_body = 0;

	printf("-- rd vms-1ee: the BODY entries, over the same implementation\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;
	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "the captured FRAME classifies");

	/* The slice SCS would deliver. */
	body = f->bytes + VMS_OFF_SYSAP_BODY;
	blen = f->wire_len - VMS_OFF_SYSAP_BODY;

	ct_check(vms_dlm_enq_request_parse(f->bytes, f->wire_len, &fi,
					   &op_frame, &from_frame) ==
		 VMS_CODEC_OK, "the FRAME entry parses the capture");
	ct_check(vms_dlm_enq_request_parse_body(body, blen, &op_body,
						&from_body) == VMS_CODEC_OK,
		 "and the BODY entry parses the 132 bytes SCS delivers");

	/* ONE implementation: the two must agree field for field. */
	ct_check_eq_u32(op_body, op_frame, "  same opcode");
	ct_check_eq_u32(from_body.mode, from_frame.mode, "  same mode");
	ct_check_eq_u32(from_body.req_pid_or_lkid, from_frame.req_pid_or_lkid,
			"  same req_pid/lkid");
	ct_check_eq_u32(from_body.master_lkid, from_frame.master_lkid,
			"  same master_lkid");
	ct_check_eq_u32(from_body.name_len, from_frame.name_len,
			"  same name_len");
	ct_check(from_body.name_len == from_frame.name_len &&
		 memcmp(from_body.name, from_frame.name, from_body.name_len) == 0,
		 "  same resource name -- one implementation, two ways in");

	ct_check(vms_dlm_dir_hash_parse(f->bytes, f->wire_len, &fi, &h_frame) ==
		 vms_dlm_dir_hash_parse_body(body, blen, &h_body),
		 "  the hash accessor agrees on both paths");
	ct_check_eq_u32(h_body, h_frame, "  ... and on the value");

	/*
	 * *** THE GUARD. *** The frame entry, handed the bare body, must
	 * REFUSE. If this ever starts succeeding, a frame-absolute parser has
	 * begun reading a body at the wrong offsets -- E73, silently.
	 */
	{
		struct vms_frame_info bogus;
		uint8_t op = 0;
		struct vms_dlm_enq_request r;

		ct_check(vms_frame_classify(body, blen, &bogus) !=
			 VMS_CODEC_OK,
			 "a bare SYSAP body does not classify as a frame -- "
			 "there is no ethertype at abs 12 to read");
		memset(&bogus, 0, sizeof(bogus));
		bogus.cls = VMS_FCLS_SCS_MSG;   /* the most generous case */
		ct_check(vms_dlm_enq_request_parse(body, blen, &bogus, &op,
						   &r) != VMS_CODEC_OK,
			 "and even then the FRAME entry REFUSES the bare body "
			 "rather than misreading it (E73)");
	}
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
	test_body_entries_are_what_scs_delivers();

	return ct_summary("test_codec_dlm");
}
