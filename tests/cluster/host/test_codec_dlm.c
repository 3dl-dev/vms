// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_dlm.c - cat-0x02 (DLM) codec entries, rung R1 (FC-P4.5).
 *
 * Four groups:
 *   1. Fixture round trip: ENQ request/grant/deny/CONVERT, and the three
 *      vms-c03 ops ($DEQ 0x03, BLKAST 0x05, value-block CONVERT 0x06) -- parse each
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
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN */
#include "vms_cluster_codec_dlm.h"

#include <stdlib.h>
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
	/*
	 * THE TWO HANDLE SLOTS, AS rd vms-b5b0 MEASURED THEM. body[20:24] is
	 * THE MASTER's handle slot and body[24:28] THE REQUESTER's -- the other
	 * way round from this test's first reading. The byte values are this
	 * capture's own, unchanged; only which side each belongs to moved, and
	 * it moved on evidence (36 of 36 releases carry the master-returned
	 * handle at body[20:24]; the requester's own appears at body[24:28] in
	 * its very first frame, before any master could have told it).
	 */
	ct_check_eq_u32(req.master_lkid, 0x2020021cu,
			"  body[20:24]: the master-handle slot, carrying the "
			"GROUNDED PID-form placeholder (no master handle yet)");
	ct_check_eq_u32(req.req_lkid, 0,
			"  body[24:28]: this composed specimen leaves the "
			"requester's own slot 0 (a real VAX puts its handle "
			"there -- see dlm-real-enq-request)");
	ct_check_eq_u32(req.name_len, 8, "  name_len == 8");
	ct_check(memcmp(req.name, "OVMXAAAA", 8) == 0, "  name == \"OVMXAAAA\"");
	/*
	 * THE IDENTITY THAT QUALIFIES THE NAME, body[44:46] + body[46] (rd
	 * vms-b5b0). An op-0x01 ROOT request is a trusted carrier of it, so the
	 * parse VOUCHES for it -- and the values are the capture's own, read
	 * here so the assertion is about the frame and not about our struct.
	 */
	ct_check_eq_u32((unsigned long)req.res_ident_valid, 1u,
			"  the parse vouches for an op-0x01 ROOT request's "
			"identity span");
	ct_check_eq_u32((unsigned long)req.res_acmode,
			(unsigned long)f->bytes[VMS_OFF_DLM_RES_MODE],
			"  body[46] parses as the resource's ACCESS MODE -- the "
			"byte this codec used to write as a constant 0x03");
	ct_check_eq_u32((unsigned long)req.res_group,
			(unsigned long)(f->bytes[VMS_OFF_DLM_RES_GROUP] |
			((uint16_t)f->bytes[VMS_OFF_DLM_RES_GROUP + 1] << 8)),
			"  body[44:46] parses as the UIC group");

	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_enq_request_build(&req, opcode, built, sizeof(built),
					   &written) == VMS_CODEC_OK,
		 "builds back from the typed struct");
	assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len, "dlm-enq-request-pw");

	/* AND THE BUILDER REFUSES TO INVENT ONE. A caller with no resource
	 * identity has no resource; a zero group on a frame for a
	 * group-qualified name makes the directory scan for a resource nobody
	 * has -- the same class of error as a zero hash (INV-6). */
	{
		struct vms_dlm_enq_request noid = req;
		uint8_t poison[256];

		noid.res_ident_valid = 0u;
		memset(poison, 0xAA, sizeof(poison));
		ct_check(vms_dlm_enq_request_build(&noid, opcode, poison,
						   sizeof(poison), &written) ==
			 VMS_CODEC_E_INVAL,
			 "*** the builder REFUSES a request that does not state "
			 "the resource's identity ***");
		ct_check_eq_u32(poison[VMS_OFF_DLM_RES_MODE], 0xAAu,
				"  and wrote nothing at all");
	}
}

/*
 * THE COMPOSED GRANT (ac4-LKID2's reading), KEPT AS A PARSE CASE ONLY
 * (rd vms-b5b0).
 *
 * This specimen was composed from docs/cluster-protocol-spec.md SS4(f).1 before
 * a real master's grant had been byte-diffed against the request it answers.
 * Two of its labels were the other way round and one of its fields does not
 * exist on a real grant:
 *
 *   - body[20:24] is THE MASTER's handle and body[24:28] THE REQUESTER's, not
 *     the reverse (see the codec header's measurement);
 *   - a real grant CLEARS body[30]: there is no granted mode on the wire;
 *   - a real grant carries the grant record at body[28]/[32:36], which this
 *     specimen has nothing at.
 *
 * So the bytes stay (they are what the ac4 reading produced, and the parser
 * still has an arm for a grant with no grant record) and the BUILD half moves
 * to test_real_grant_is_reproduced() below, against a real captured pair.
 */
static void test_enq_grant(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-grant");
	struct vms_frame_info fi;
	struct vms_dlm_enq_response resp;

	printf("-- dlm-enq-grant: the COMPOSED grant, as a parse case (rd vms-b5b0)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_enq_response_parse(f->bytes, f->wire_len, &fi, &resp)
		 == VMS_CODEC_OK, "parses as an ENQ/CONVERT response");
	ct_check(resp.outcome == VMS_DLM_ENQ_GRANTED,
		 "  discriminated as GRANTED (no name echoed)");
	ct_check_eq_u32(resp.master_lkid, 0x310000ABu,
			"  body[20:24] is THE MASTER's handle (rd vms-b5b0 "
			"relabelled it; the byte value is the ac4 one)");
	ct_check_eq_u32(resp.req_lkid, 0x520006AFu,
			"  body[24:28] is THE REQUESTER's own handle");
	ct_check_eq_u32((unsigned long)resp.granted_mode_present, 1u,
			"  this shape DOES carry a mode -- it has no grant "
			"record, which no real grant is without");
	ct_check_eq_u32(resp.granted_mode, VMS_LCK_PW, "  and it reads PW");
	ct_check_eq_u32((unsigned long)resp.valblk_present, 0u,
			"  and it claims NO value block");
}

/*
 * ===========================================================================
 * THE ACCEPTANCE PROOF FOR A MASTER'S GRANT (rd vms-b5b0, the ev5 request
 * storm): OVMX's builder reproduces a REAL OpenVMS VAX master's grant
 * BYTE-FOR-BYTE on every byte this codec owns.
 *
 * WHAT WENT WRONG WITHOUT IT. OVMX's grant carried the two lock handles in
 * each other's slots, no grant record (so body[34] -- the outcome byte -- read
 * 0x00, a value no real answer carries) and the granted mode in a byte every
 * real grant clears. VAX1 could not correlate the completion to its own lock:
 * it re-sent the same op-01 65,356 times in 63.7 s (1026/s) and OVMX answered
 * 65,340 of them. A human stopped it.
 *
 * WHAT THIS TEST IS. The real request (dlm-real-enq-request) and the real grant
 * that answered it 155 us later (dlm-real-enq-grant), both captured off the
 * same wire. Build a grant from the request with the master handle the real
 * master assigned and the block the real master returned, and demand EQUALITY
 * on all 132 body bytes except an explicitly named six:
 *
 *   body[0:4]    the CM's transaction envelope -- the wrapper's, not this
 *                codec's (every builder here leaves it alone)
 *   body[52:54]  the SCS-layer word this codec's own header already documents
 *                as "not the DLM codec's to reproduce"
 *
 * A regression that touches ANY other byte -- or that stops writing the grant
 * record, or swaps the handles back -- reddens here.
 * ===========================================================================
 */
static void test_real_grant_is_reproduced(void)
{
	const struct vms_fixture *rq = fixture("dlm-real-enq-request");
	const struct vms_fixture *gr = fixture("dlm-real-enq-grant");
	struct vms_frame_info fi;
	struct vms_dlm_enq_request parsed;
	struct vms_dlm_enq_response resp;
	uint8_t opcode = 0;
	uint8_t built[256];
	uint32_t written = 0, i, diffs = 0;
	/* body[0:4] (the CM envelope) and body[52:54] (the SCS word). */
	static const int owned_elsewhere[] = { 0, 1, 2, 3, 52, 53 };

	printf("-- rd vms-b5b0: a REAL master's grant, reproduced byte-for-byte\n");
	ct_check(rq != NULL && gr != NULL, "both real-capture fixtures load");
	if (rq == NULL || gr == NULL)
		return;

	/* The real REQUEST, as this codec reads it. */
	ct_check(vms_frame_classify(rq->bytes, rq->wire_len, &fi) ==
		 VMS_CODEC_OK &&
		 vms_dlm_enq_request_parse(rq->bytes, rq->wire_len, &fi,
					   &opcode, &parsed) == VMS_CODEC_OK,
		 "the real request parses as a cat-02 op-01");
	ct_check_eq_u32(parsed.req_lkid, 0x090003cdu,
			"*** body[24:28] is THE REQUESTER's own handle "
			"(0x090003cd -- a value no master ever sent it) ***");
	ct_check_eq_u32(parsed.master_lkid, 0x20200213u,
			"*** body[20:24] is the MASTER's handle slot, carrying "
			"the PID-form placeholder on a fresh ENQ ***");

	/* The real GRANT, as this codec reads it. */
	ct_check(vms_frame_classify(gr->bytes, gr->wire_len, &fi) ==
		 VMS_CODEC_OK &&
		 vms_dlm_enq_response_parse(gr->bytes, gr->wire_len, &fi,
					    &resp) == VMS_CODEC_OK,
		 "the real grant parses as a cat-82 op-01");
	ct_check(resp.outcome == VMS_DLM_ENQ_GRANTED,
		 "  and is read as GRANTED (the 0xfa outcome at body[34])");
	ct_check_eq_u32(resp.req_lkid, 0x090003cdu,
			"*** the grant ECHOES the requester's handle -- which is "
			"the correlation OVMX was breaking ***");
	ct_check_eq_u32(resp.master_lkid, 0x650006b0u,
			"*** and carries the handle the MASTER assigned ***");
	ct_check_eq_u32((unsigned long)resp.granted_mode_present, 0u,
			"  a real grant carries NO granted mode (body[30] is "
			"cleared in 38 of 38)");
	ct_check_eq_u32((unsigned long)resp.valblk_present, 1u,
			"  and it carries the master resource's value block");

	/* *** THE REPRODUCTION *** */
	memset(built, 0xAA, sizeof(built));
	ct_check(vms_dlm_enq_response_build_grant(rq->bytes + VMS_OFF_SYSAP_BODY,
						  VMS_CM_BODY_LEN,
						  resp.master_lkid, resp.valblk,
						  built, sizeof(built),
						  &written) == VMS_CODEC_OK,
		 "OVMX builds a grant from the real request");
	for (i = 0u; i < VMS_CM_BODY_LEN; i++) {
		int skip = 0, k;

		for (k = 0; k < (int)(sizeof(owned_elsewhere) /
				      sizeof(owned_elsewhere[0])); k++) {
			if ((int)i == owned_elsewhere[k])
				skip = 1;
		}
		if (skip)
			continue;
		if (built[VMS_OFF_SYSAP_BODY + i] !=
		    gr->bytes[VMS_OFF_SYSAP_BODY + i]) {
			printf("   body[%3u] ours=%02x real=%02x\n", i,
			       built[VMS_OFF_SYSAP_BODY + i],
			       gr->bytes[VMS_OFF_SYSAP_BODY + i]);
			diffs++;
		}
	}
	ct_check_eq_u32(diffs, 0u,
			"*** every byte of the real grant that this codec owns "
			"is reproduced exactly (126 of 132) ***");

	/* And the two spans that are not this codec's are UNTOUCHED by it --
	 * asserted positively, so a builder that started writing them would
	 * redden rather than pass by being ignored. */
	ct_check(built[VMS_OFF_SYSAP_BODY + 52] == 0u &&
		 built[VMS_OFF_SYSAP_BODY + 53] == 0u,
		 "  the SCS-layer word at body[52:54] is left zero, not minted");

	/* A grant that hands out lock-id 0 is not a grant (the fc8540ae rule). */
	ct_check(vms_dlm_enq_response_build_grant(rq->bytes + VMS_OFF_SYSAP_BODY,
						  VMS_CM_BODY_LEN,
						  VMS_DLM_LKID_UNSET,
						  resp.valblk, built,
						  sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL,
		 "  and a master handle of 0 is refused");
	/* ...and a grant can only answer a request. */
	ct_check(vms_dlm_enq_response_build_grant(gr->bytes + VMS_OFF_SYSAP_BODY,
						  VMS_CM_BODY_LEN,
						  resp.master_lkid, resp.valblk,
						  built, sizeof(built),
						  &written) == VMS_CODEC_E_CLASS,
		 "  a grant cannot be built from another grant (E_CLASS)");
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
	ct_check_eq_u32(resp.master_lkid, 0x2020021cu,
			"  body[20:24] echoes the request's own master-slot "
			"placeholder UNCHANGED: a deny assigns no handle "
			"(slots per rd vms-b5b0)");
	ct_check_eq_u32(resp.granted_mode, 0, "  mode CLEARED to 0");
	ct_check_eq_u32(resp.name_len, 8, "  name_len == 8, echoed");
	ct_check(memcmp(resp.name, "OVMXAAAA", 8) == 0,
		 "  name == \"OVMXAAAA\", echoed verbatim");

	memset(built, 0xAA, sizeof(built));
	/* body[46] is the resource's ACCESS MODE and the reply ECHOES it (rd
	 * vms-b5b0 -- it is not the constant 0x03 this builder used to write).
	 * The value comes from the captured frame itself, which is what makes
	 * the byte-for-byte rebuild below evidence rather than agreement with
	 * our own assumption. */
	ct_check(vms_dlm_enq_response_build_deny(resp.req_lkid, resp.master_lkid,
						 f->bytes[VMS_OFF_DLM_RES_MODE],
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
	ct_check_eq_u32(req.master_lkid, 0x5000038Au,
			"  body[20:24] == the MASTER's handle for the lock "
			"being converted (a convert names the lock on the "
			"master; slots per rd vms-b5b0)");
	ct_check_eq_u32(req.req_lkid, 0x120004B9u,
			"  body[24:28] == the requester's own handle");

	/*
	 * THE PARSE DOES NOT VOUCH FOR AN op-0x07's IDENTITY SPAN (rd vms-b5b0):
	 * a real VAX leaves body[44:48] -- and the name beside it -- STALE on a
	 * convert (docs/design-dlm-name-hash.md §4a measured two such frames
	 * whose readable name belonged to a different resource), so the parser
	 * clears `res_ident_valid` and the engine takes a convert's resource
	 * from the LOCK the convert names instead. To rebuild THIS capture
	 * byte-for-byte the test therefore states the bytes the capture itself
	 * carries; the builder refuses to invent them.
	 */
	ct_check_eq_u32((unsigned long)req.res_ident_valid, 0u,
			"  the parse does NOT vouch for an op-0x07's identity "
			"span (it is stale buffer on a real frame)");
	req.res_group = (uint16_t)(f->bytes[VMS_OFF_DLM_RES_GROUP] |
				   ((uint16_t)f->bytes[VMS_OFF_DLM_RES_GROUP + 1] << 8));
	req.res_acmode = f->bytes[VMS_OFF_DLM_RES_MODE];
	req.res_ident_valid = 1u;

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
	ct_check_eq_u32(d.req_lkid, 0x3a0004ebu,
			"  body[24:28] == 0x3a0004eb, the REQUESTER's own "
			"handle (rd vms-b5b0 relabelled the slot), the "
			"driving ENQ for 'OVMXDEQ1' carried");
	ct_check_eq_u32(d.master_lkid, 0x080001cdu,
			"  body[20:24] == 0x080001cd, the MASTER's handle -- "
			"which is why a release names its lock here: the GRANT "
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
	ct_check_eq_u32(built[VMS_OFF_DLM_RES_MODE], 0xAAu,
			"*** the builder writes NO resource identity: a $DEQ "
			"names its lock by lock-id, so body[46] is not its "
			"field to write (rd vms-b5b0) ***");
}

/*
 * op 0x05 = BLKAST (rd vms-ea1), and THE GUARD: a real BLKAST names its lock
 * by lock-id only, and the specimen's old 'F11B$aSYSDSK1' came from a
 * DIFFERENT frame -- an op-0x04 directory removal -- with a readable
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

	printf("-- dlm-blkast: op 0x05 is BLKAST (vms-c03 record 48 of "
	       "dlm-blk2-20260911.pcap, rd vms-ea1)\n");
	ct_check(f != NULL, "fixture loads");
	if (f == NULL)
		return;

	ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi) == VMS_CODEC_OK,
		 "classifies without error");
	ct_check(vms_dlm_blkast_parse(f->bytes, f->wire_len, &fi, &b) ==
		 VMS_CODEC_OK, "parses as a BLKAST");
	ct_check_eq_u32(b.req_lkid, 0x590004e3u,
			"  body[24:28] == 0x590004e3, the holder's own handle "
			"(rd vms-b5b0 relabelled the slot) -- the EX "
			"holder's ENQ for 'OVMXBLK2' carried");
	ct_check_eq_u32(b.master_lkid, 0x0a0003afu,
			"  body[20:24] == 0x0a0003af, the MASTER's handle for "
			"the resource");

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
	ct_check_eq_u32(built[VMS_OFF_DLM_RES_MODE], 0xAAu,
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
	ct_check_eq_u32(c.req_lkid, 0x2b000489u,
			"  body[24:28] == 0x2b000489, the requester's own "
			"handle (rd vms-b5b0 relabelled the slot) -- the "
			"driving ENQ for 'OVMXLVB3' carried");
	ct_check_eq_u32(c.master_lkid, 0x270001cdu,
			"  body[20:24] == the MASTER's handle, which the GRANT "
			"assigned (rd vms-b5b0 relabelled the slot)");
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
	/*
	 * THE SLOTS, RELABELLED ON EVIDENCE (rd vms-b5b0). SDA showed lock id
	 * 0A0003A4 and this test took body[20:24] to be the requester's because
	 * of it; the storm capture settled it the other way (36 of 36 releases
	 * name their lock at body[20:24] with the value the master returned,
	 * and a requester's first frame already carries its own handle at
	 * body[24:28]). An SDA lock id is a lock id on SOME node, and which
	 * node it is cannot be read off SDA alone.
	 */
	ct_check_eq_u32(resp.master_lkid, 0x0a0003a4u,
			"  body[20:24] == 0x0a0003a4, the MASTER's handle");
	ct_check_eq_u32(resp.req_lkid, 0x570001b7u,
			"  body[24:28] == 0x570001b7, the requester's own "
			"handle for 'OVMXLV01', echoed by the grant");

	/*
	 * THE BYTE-IDENTICAL BUILD PROOF moved (rd vms-b5b0). There is ONE grant
	 * builder now and it builds by ECHOING THE REQUEST, so it cannot be
	 * driven from a grant alone -- the request this grant answered is not in
	 * this fixture pair. The byte-for-byte reproduction is
	 * test_real_grant_is_reproduced() above, against a real captured
	 * request/grant PAIR, which is a stronger proof than rebuilding a grant
	 * from its own parsed fields: it shows the echoed bytes are the
	 * requester's own and not ours.
	 */

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
 * rd vms-ea1: the frame the first vms-c03 reading took for a BLKAST was a NAMED
 * op 0x04 -- a master removing its directory entry -- whose lock-id span is a
 * stale copy of the real BLKAST's. A real named op 0x04 (the vms-8219 fixture,
 * VAX3 removing its entry for a resource it mastered) must therefore be
 * refused by the BLKAST parser, and still be read as a removal.
 */
static void test_named_op04_is_not_a_blkast(void)
{
	const struct vms_fixture *rm = fixture("dlm-dir-remove-vax3");
	struct vms_frame_info fi;
	struct vms_dlm_blkast b;
	struct vms_dlm_res_ident id;

	ct_check(rm != NULL, "the real named op-0x04 fixture loads");
	if (rm == NULL)
		return;
	(void)vms_frame_classify(rm->bytes, rm->wire_len, &fi);
	ct_check(vms_dlm_blkast_parse(rm->bytes, rm->wire_len, &fi, &b) ==
		 VMS_CODEC_E_CLASS,
		 "*** a real named op 0x04 is REFUSED by the BLKAST parser ***");
	ct_check(vms_dlm_res_ident_parse_body(rm->bytes + VMS_OFF_SYSAP_BODY,
					      rm->wire_len - VMS_OFF_SYSAP_BODY,
					      &id) == VMS_CODEC_OK &&
		 id.name_len > 0u,
		 "  and it still reads as a named directory-entry removal");
}

/*
 * THE SUPERSESSION, asserted rather than merely documented. The phantom ops
 * are gone as VALUES: 0x03 means $DEQ and 0x05 means BLKAST (rd vms-ea1), and a
 * parser for one refuses the other's frame. A tree that quietly kept a
 * "completion 0x04" -- or the "BLKAST 0x04" the first vms-c03 reading claimed
 * -- alive somewhere would show up here as a parse that succeeds when it must
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

	printf("-- one opcode, one meaning: 0x03 is $DEQ and 0x05 is BLKAST, "
	       "and neither is anything else\n");
	if (deq == NULL || blk == NULL) {
		ct_check(0, "both fixtures load");
		return;
	}

	ct_check(VMS_DLM_WIREOP_DEQ == 0x03u && VMS_DLM_WIREOP_BLKAST == 0x05u &&
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

	test_named_op04_is_not_a_blkast();
}

static void test_fixture_roundtrips(void)
{
	test_enq_request_pw();
	test_enq_grant();
	test_real_grant_is_reproduced();
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

/*
 * REAL rejoin op-0x0d frames: the byte-identical round-trip crash-guard
 * (vms-20c, FC-P5.5). Unlike the spec-composed dlm-rebuild-request (mostly
 * zero body), these are FOUR REAL captured records from the 2-VAX rejoin
 * (VAX1 survivor -> VAX2). They (a) exercise the rebuild-TYPE fix -- every
 * one carries body[14:16]=0x0004 (REJOIN), which the old 0x0003-only
 * "invariant" wrongly rejected; and (b) prove the codec round-trips a REAL
 * record byte-identical -- including the frame-specific stale-buffer tail --
 * which is the never-crash foundation the survivor-side SENDER links against.
 */
static void test_rebuild_rejoin_roundtrip(void)
{
	static const struct { const char *fx; const char *res; uint8_t reslen; }
	cases[] = {
		{ "dlm-rebuild-rejoin-sys",  "SYS$SYS_ID",    16 },
		{ "dlm-rebuild-rejoin-vcc",  "VCC$vSYSDSK1",  17 },
		{ "dlm-rebuild-rejoin-f11b", "F11B$aSYSDSK1", 22 },
		{ "dlm-rebuild-rejoin-mscp", "MSCP$LOADBAL",  12 },
	};
	unsigned i;

	printf("-- dlm-rebuild-rejoin-*: REAL op-0x0d rejoin frames, byte-identical "
	       "round-trip (rebuild-type 0x0004)\n");
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct vms_fixture *f = fixture(cases[i].fx);
		struct vms_frame_info fi;
		struct vms_dlm_rebuild_record rec;
		uint8_t built[256];
		uint32_t written = 0;
		char msg[96];

		snprintf(msg, sizeof(msg), "  %s: fixture loads", cases[i].fx);
		ct_check(f != NULL, msg);
		if (f == NULL)
			continue;

		ct_check(vms_frame_classify(f->bytes, f->wire_len, &fi)
			 == VMS_CODEC_OK, "    classifies as SCS_MSG");
		/* The rebuild-type fix: a REJOIN frame (body[14:16]=0x0004) MUST
		 * parse -- the prior 0x0003-only gate rejected every one. */
		ct_check(vms_dlm_rebuild_parse(f->bytes, f->wire_len, &fi, &rec)
			 == VMS_CODEC_OK,
			 "    parses (rebuild-type 0x0004 REJOIN accepted)");
		ct_check_eq_u32(rec.rebuild_type, VMS_DLM_REBUILD_TYPE_REJOIN,
				"    rebuild_type == REJOIN (0x0004)");
		ct_check_eq_u32(rec.name_len, cases[i].reslen,
				"    reslen (body[47]) matches the capture");
		ct_check(memcmp(rec.name, cases[i].res, strlen(cases[i].res)) == 0,
			 "    resource name (body[48]) matches the capture");

		/* Byte-identical round-trip: rebuild the request, compare the whole
		 * body against the real captured frame (origin:capture -> every byte
		 * cited). The crash guard: the codec preserves the real record
		 * verbatim, stale-buffer tail included, never a mis-shift. */
		memset(built, 0xAA, sizeof(built));
		ct_check(vms_dlm_rebuild_request_build(&rec, built, sizeof(built),
						       &written) == VMS_CODEC_OK,
			 "    request_build OK");
		ct_check_eq_u32(written, VMS_DLM_REBUILD_ECHO_LEN,
				"    wrote the 132-byte body span");
		assert_cited_bytes_match(f, built, VMS_OFF_SYSAP_BODY, f->wire_len,
					 cases[i].fx);
	}
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
		 "op 0x05 (BLKAST) resolves to CONSUME with no response recipe");

	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST,
				VMS_DLM_WIREOP_CONVERT_VALBLK);
	ct_check(e != NULL && e->action == VMS_WIRE_ACT_CONSUME &&
		 e->recipe == 0u,
		 "op 0x06 (value-block CONVERT) resolves to CONSUME");

	/* Still absent: an allowlist row asserts "grounded in the reference" for
	 * a LOCK-ID op. op 0x05 left this list when rd vms-ea1 grounded it as the
	 * BLKAST; op 0x04 is not a lock-id op at all (named, it is the directory
	 * removal the vms-8219 directory role consumes), and op 0x09 / 0x0a
	 * still appear in the vms-c03 captures without any correlation. */
	e = vms_wire_allow_find(&vms_dlm_allow_table, VMS_SYSAP_VMS_VAXCLUSTER,
				VMS_DLM_CAT_REQUEST, 0x04u);
	ct_check(e == NULL, "op 0x04 (the directory removal) has no lock-id row");
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
	 * lesson they taught does: the $DEQ and the BLKAST (0x03, 0x05) are
	 * lock-id-ONLY messages, so a placeholder there is
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
		 VMS_CODEC_E_INVAL, "  master_lkid==0 REFUSED on BLKAST (0x05)");
	b.master_lkid = 0x00020017u;
	b.req_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_blkast_build(&b, built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "  req_lkid==0 REFUSED on BLKAST (0x05)");

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
	 * a lock". The vms-c03 blk capture really does contain cat-0x02 frames
	 * whose lock-id span is zero or stale.
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

	/* The GRANT builder carries the same guard on the handle it is about to
	 * hand a peer as "the lock-id I assigned you"; a zero there is the same
	 * class of lie. (Driven on the real pair in
	 * test_real_grant_is_reproduced; restated here with the other lock-id
	 * refusals so the family is in one place.) */
	{
		const struct vms_fixture *rq = fixture("dlm-real-enq-request");

		ct_check(rq != NULL &&
			 vms_dlm_enq_response_build_grant(
				 rq->bytes + VMS_OFF_SYSAP_BODY,
				 VMS_CM_BODY_LEN, VMS_DLM_LKID_UNSET, NULL,
				 built, sizeof(built), &written) ==
			 VMS_CODEC_E_INVAL,
			 "  vms_dlm_enq_response_build_grant refuses a master "
			 "handle of 0");
	}
}

/*
 * THE DIRECTORY HASH at body[128:132] (rd vms-4fb), against REAL frames from a
 * private three-node V7.3 cluster: one ROOT name (DLMTA) asked for by two
 * different senders carries the SAME four bytes; a SUB-resource request and
 * the directory node's own 0x82 answer are not learning sources and are
 * refused; a refusal writes nothing.
 */
static void test_dir_hash_accessor(void)
{
	const struct vms_fixture *r2 = fixture("dlm-dirhash-root-vax2");
	const struct vms_fixture *r3 = fixture("dlm-dirhash-root-vax3");
	const struct vms_fixture *sub = fixture("dlm-dirhash-sub-vax2");
	const struct vms_fixture *ans = fixture("dlm-dirhash-answer-vax1");
	struct vms_frame_info fi;
	uint32_t h2 = 0u, h3 = 0u, hash;
	uint8_t frame[256];

	printf("-- the directory hash at body[128:132] (rd vms-4fb, p. 6-50)\n");
	ct_check(r2 != NULL && r3 != NULL && sub != NULL && ans != NULL,
		 "the four L1 specimens load");
	if (r2 == NULL || r3 == NULL || sub == NULL || ans == NULL)
		return;

	ct_check(vms_frame_classify(r2->bytes, r2->wire_len, &fi) == VMS_CODEC_OK &&
		 vms_dlm_dir_hash_parse(r2->bytes, r2->wire_len, &fi, &h2) ==
		 VMS_CODEC_OK, "VAX2's root request yields a hash");
	ct_check(vms_frame_classify(r3->bytes, r3->wire_len, &fi) == VMS_CODEC_OK &&
		 vms_dlm_dir_hash_parse(r3->bytes, r3->wire_len, &fi, &h3) ==
		 VMS_CODEC_OK, "VAX3's root request for the same name yields one");
	ct_check_eq_u32(h2, 0x00336fe3u,
			"  it is body[128:132] little-endian (e3 6f 33 00)");
	ct_check_eq_u32(h3, h2,
			"  *** the SAME value from a different sender: a property "
			"of the NAME ***");
	ct_check(r2->bytes[82] != r3->bytes[82] || r2->bytes[83] != r3->bytes[83],
		 "  ...while body[10:12], the old INFERRED offset, differs "
		 "between the two senders (it is not the hash)");

	hash = 0xA5A5A5A5u;
	ct_check(vms_frame_classify(sub->bytes, sub->wire_len, &fi) == VMS_CODEC_OK &&
		 vms_dlm_dir_hash_parse(sub->bytes, sub->wire_len, &fi, &hash) ==
		 VMS_CODEC_E_CLASS,
		 "a SUB-resource request (parent span nonzero) is refused");
	ct_check_eq_u32(hash, 0xA5A5A5A5u, "  writing nothing");
	ct_check(vms_frame_classify(ans->bytes, ans->wire_len, &fi) == VMS_CODEC_OK &&
		 vms_dlm_dir_hash_parse(ans->bytes, ans->wire_len, &fi, &hash) ==
		 VMS_CODEC_E_CLASS,
		 "the directory's 0x82 ANSWER is refused (body[28:40] rewritten)");
	ct_check_eq_u32(hash, 0xA5A5A5A5u, "  writing nothing");

	/* A value no name-derived function would produce, read back verbatim:
	 * the accessor transports, it does not derive. */
	ct_check(vms_frame_classify(r2->bytes, r2->wire_len, &fi) == VMS_CODEC_OK,
		 "re-classify the root request");
	memcpy(frame, r2->bytes, r2->wire_len);
	frame[VMS_OFF_DLM_DIR_HASH] = 0x78u;
	frame[VMS_OFF_DLM_DIR_HASH + 1u] = 0x56u;
	frame[VMS_OFF_DLM_DIR_HASH + 2u] = 0x34u;
	frame[VMS_OFF_DLM_DIR_HASH + 3u] = 0x12u;
	hash = 0u;
	ct_check(vms_dlm_dir_hash_parse(frame, r2->wire_len, &fi, &hash) ==
		 VMS_CODEC_OK && hash == 0x12345678u,
		 "reads an arbitrary wire value byte for byte");

	hash = 0xA5A5A5A5u;
	ct_check(vms_dlm_dir_hash_parse(frame, r2->wire_len, NULL, &hash) ==
		 VMS_CODEC_E_CLASS, "a frame with no class info is refused");
	ct_check_eq_u32(hash, 0xA5A5A5A5u, "  and the caller's variable is untouched");
	ct_check(vms_dlm_dir_hash_parse(frame, r2->wire_len, &fi, NULL) ==
		 VMS_CODEC_E_CLASS, "a null output is refused");
	{
		struct vms_frame_info wrong = fi;

		wrong.cls = VMS_FCLS_HELLO;
		ct_check(vms_dlm_dir_hash_parse(frame, r2->wire_len, &wrong,
						&hash) == VMS_CODEC_E_CLASS,
			 "a non-SCS_MSG frame is refused");
	}
	{
		uint8_t other[256];

		memcpy(other, frame, r2->wire_len);
		other[VMS_OFF_DLM_OP] = VMS_DLM_WIREOP_CONVERT;
		ct_check(vms_dlm_dir_hash_parse(other, r2->wire_len, &fi,
						&hash) == VMS_CODEC_E_CLASS,
			 "an opcode the value is not grounded on is refused");
		other[VMS_OFF_DLM_OP] = VMS_DLM_WIREOP_ENQ;
		other[VMS_OFF_DLM_CAT] = 0x01u;
		ct_check(vms_dlm_dir_hash_parse(other, r2->wire_len, &fi,
						&hash) == VMS_CODEC_E_CLASS,
			 "a cat-0x01 body is refused");
	}
	ct_check_eq_u32(hash, 0xA5A5A5A5u, "  every refusal wrote nothing");

	/* A truncated frame reports the view's error, not a zero. */
	ct_check(vms_dlm_dir_hash_parse(frame, VMS_OFF_DLM_DIR_HASH + 1u, &fi,
					&hash) != VMS_CODEC_OK,
		 "a frame too short to hold the field is refused");
	ct_check_eq_u32(hash, 0xA5A5A5A5u, "  writing nothing");
}

/*
 * ===========================================================================
 * THE E73 REGRESSION GUARD (rd vms-1ee): a SYSAP BODY parses, and the
 * frame-absolute entry refuses the very same bytes.
 *
 * cnxman_vc_message() hands a SYSAP its own 132 bytes and nothing below them
 * (design sec 3.2.4). Handing those bytes to a frame-absolute parser is exactly
 * integration note E73 -- it does not fail loudly, it silently refuses every
 * real inbound message, which is how a live run lost a whole CM dialogue. This
 * pins BOTH halves: the body entry reads the same fields the frame entry does,
 * and the frame entry on a bare body is a refusal, not a misparse.
 * ===========================================================================
 */
static void test_body_entries_are_what_scs_delivers(void)
{
	const struct vms_fixture *f = fixture("dlm-enq-request-pw");
	struct vms_frame_info fi;
	struct vms_dlm_enq_request from_frame, from_body;
	uint8_t op_frame = 0, op_body = 0;
	const uint8_t *body;
	uint32_t blen;
	uint32_t h_frame = 0, h_body = 0;

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
	ct_check_eq_u32(from_body.req_lkid, from_frame.req_lkid,
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

/* ---- group 5: op-0e DLKSRCH twin (H11, vms-d55) ----------------------- *
 * No captured fixture exists (OVMX-derived, Rule 8: the do-it-like-VMS
 * successor to the retired-scsd SEARCH orchestration). The proof is a
 * build->parse twin over a fully-populated record, into a HEAP buffer
 * sized to EXACTLY `written` so ASan red-zones a one-byte over-write, plus
 * a one-byte-short refusal to prove the builder is bounded, not scribbling.
 * Non-vacuous: all eight fields carry distinct non-zero values and are
 * each compared after the round trip.
 */
static void test_dlksrch_twin(void)
{
	struct vms_dlm_dlksrch_record in, out;
	uint8_t probe[256];
	uint32_t written = 0, body_off = VMS_OFF_SYSAP_BODY;
	uint8_t *exact;
	vms_codec_status_t st;

	memset(&in, 0, sizeof(in));
	in.flag           = 2u; /* VMS_DLM_DLK_VICTIM -- every field meaningful */
	in.initiator_csid = 0x11112222u;
	in.initiator_lkid = 0x33334444u;
	in.blocked_csid   = 0x55556666u;
	in.blocked_lkid   = 0x77778888u;
	in.victim_csid    = 0x9999AAAAu;
	in.victim_lkid    = 0xBBBBCCCCu;
	in.ttl            = 16u;

	/* Size discovery into a poisoned oversize buffer. */
	memset(probe, 0xAA, sizeof(probe));
	st = vms_dlm_dlksrch_build(&in, probe, sizeof(probe), &written);
	ct_check(st == VMS_CODEC_OK, "DLKSRCH build OK");
	ct_check_eq_u32(written, VMS_OFF_DLM_DLK_TTL + 1u,
			"  frame high-water == TTL offset + 1 (109)");

	/* Rebuild into a heap buffer of EXACTLY `written` bytes: a one-byte
	 * over-write trips ASan here rather than passing silently. */
	exact = (uint8_t *)malloc(written);
	ct_check(exact != NULL, "  exact-sized buffer allocates");
	st = vms_dlm_dlksrch_build(&in, exact, written, &written);
	ct_check(st == VMS_CODEC_OK, "  build into exact-sized buffer OK");

	/* Parse the body span back and compare every field (non-vacuous). */
	memset(&out, 0, sizeof(out));
	st = vms_dlm_dlksrch_parse_body(exact + body_off, written - body_off,
					&out);
	ct_check(st == VMS_CODEC_OK, "  DLKSRCH parse_body OK");
	ct_check_eq_u32(out.flag, in.flag, "    flag survives");
	ct_check_eq_u32(out.initiator_csid, in.initiator_csid,
			"    initiator_csid survives");
	ct_check_eq_u32(out.initiator_lkid, in.initiator_lkid,
			"    initiator_lkid survives");
	ct_check_eq_u32(out.blocked_csid, in.blocked_csid,
			"    blocked_csid survives");
	ct_check_eq_u32(out.blocked_lkid, in.blocked_lkid,
			"    blocked_lkid survives");
	ct_check_eq_u32(out.victim_csid, in.victim_csid,
			"    victim_csid survives");
	ct_check_eq_u32(out.victim_lkid, in.victim_lkid,
			"    victim_lkid survives");
	ct_check_eq_u32(out.ttl, in.ttl, "    ttl survives");

	/* One byte short must FAIL, not scribble (bounded-write proof). */
	ct_check(vms_dlm_dlksrch_build(&in, exact, written - 1u, NULL)
		 != VMS_CODEC_OK, "  build REFUSES a one-byte-short buffer");

	free(exact);
}

/*
 * rd vms-cab: the immediate answer to a QUEUED CONVERT, against a real pair
 * captured between two OpenVMS VAX V7.3 nodes (lab run ev11, 2026-10-09
 * 11:14:31.737771 VAX1 -> VAX2 op-0x07, and VAX2's answer 78 us later).
 * Everything after the CM envelope (body[0:4]) must be byte-identical.
 */
static void test_queued_convert_answer_is_the_real_one(void)
{
	static const char req_hex[] =
	    "b93ba22b070099bd020703000100070000000000b7040001880200011b0605000f000100"
	    "000000000000000000000000000000000f02202000000000534b3120202020209a090000"
	    "02000100000000000000000000000000000000000000000000000000000000000000000000"
	    "00000000000000ffffffff00000000ffffffff5e640d90";
	static const char rep_hex[] =
	    "a42bb93b070099bd820703000100070000000000b7040001880200011b0605000f00fb00"
	    "000000000000000000000000000000000000202000000000534b3120202020209a090000"
	    "02000100000000000000000000000000000000000000000000000000000000000000000000"
	    "00000000000000ffffffff00000000ffffffff5e640d90";
	uint8_t req[132], rep[132], frame[VMS_OFF_SYSAP_BODY + 132];
	uint32_t i, written = 0, diffs = 0;

	for (i = 0; i < 132u; i++) {
		unsigned int x, y;
		sscanf(&req_hex[2 * i], "%2x", &x);
		sscanf(&rep_hex[2 * i], "%2x", &y);
		req[i] = (uint8_t)x;
		rep[i] = (uint8_t)y;
	}
	memset(frame, 0, sizeof(frame));
	ct_check(vms_dlm_convert_response_build_queued(req, sizeof(req), frame,
						       sizeof(frame), &written) ==
		 VMS_CODEC_OK, "a queued CONVERT's answer builds");
	for (i = 4u; i < 132u; i++)
		if (frame[VMS_OFF_SYSAP_BODY + i] != rep[i])
			diffs++;
	ct_check_eq_u32(diffs, 0u,
			"*** the queued-CONVERT answer equals the real VAX "
			"master's, byte for byte after the envelope ***");
	ct_check_eq_u32(frame[VMS_OFF_SYSAP_BODY + 34u], 0xfbu,
			"  ... outcome byte 0xfb (queued)");
	ct_check(vms_dlm_convert_response_build_queued(rep, sizeof(rep), frame,
						       sizeof(frame), &written) !=
		 VMS_CODEC_OK, "  ... and a RESPONSE is refused as input");

	/* The LATER grant of that same CONVERT keeps op 0x07: a real master
	 * grants a conversion as 82/07, never as an ENQ grant 82/01. */
	memset(frame, 0, sizeof(frame));
	ct_check(vms_dlm_enq_response_build_grant(req, sizeof(req), 0x0001abcdu,
						  NULL, frame, sizeof(frame),
						  &written) == VMS_CODEC_OK,
		 "the CONVERT's grant builds");
	ct_check_eq_u32(frame[VMS_OFF_SYSAP_BODY + 9u], 0x07u,
			"*** a CONVERT is granted as op 0x07, the request's own op ***");
}

/*
 * rd vms-cab: the answer to an op-0x0f from a member holding no lock on the
 * resource, against two REAL pairs from lab run ci6-evac-15 (2026-10-10,
 * 16:45:14.88 VAX2 -> VAX1 F11B$vSYSDSK1 and 16:45:16.17 VAX1 -> VAX2
 * CACHE$cm..., both during VAX1's REMOVE_NODE departure). Held out from the
 * 1392-pair corpus scan the rule was read from. body[0:4] is the envelope.
 */
static void test_op0f_ack_is_the_real_one(void)
{
	static const char *const pair[2][2] = {
		{
	    "c7c8c9c70600c2c9020f000001000e0000000000d9040003cd0400030100010001000e00"
	    "0101011e000000000000001246313142247653595344534b312020202020000000000000"
	    "010001000000000001000100000000000000000000000000000000000000000000000000"
	    "0000000000000000ffffffff03000000ffffffff173cc220",
	    "cac7c7c80600c2c98215000001000e0000000000d9040003cd040003000000000100fa00"
	    "0101011e000000000000001246313142247653595344534b312020202020000000000000"
	    "010001000000000001000100000000000000000000000000000000000000000000000000"
	    "0000000000000000ffffffff03000000ffffffff173cc220" },
		{
	    "d1c7ccc80100e0cb020fc90001000e0020040100200cd52d5f3cbc000200010001000e00"
	    "010000000000000000000018434143484524636d53595344534b31202020202067010000"
	    "252525202020202800000000206e000065205641583120202061742031302d4f43542d32"
	    "3032360000000000ffffffff00000000ffffffff5e5e659a",
	    "cdc8d1c70100e0cb8215c90001000e0020040100200cd52d5f3cbc00000000000100fa00"
	    "010000000000000000000018434143484524636d53595344534b31202020202067010000"
	    "252525202020202800000000206e000065205641583120202061742031302d4f43542d32"
	    "3032360000000000ffffffff00000000ffffffff5e5e659a" },
	};
	uint8_t req[132], rep[132], frame[VMS_OFF_SYSAP_BODY + 132];
	struct vms_dlm_res_ident id;
	uint32_t p, i, written = 0, diffs;

	for (p = 0; p < 2u; p++) {
		for (i = 0; i < 132u; i++) {
			unsigned int x, y;
			sscanf(&pair[p][0][2 * i], "%2x", &x);
			sscanf(&pair[p][1][2 * i], "%2x", &y);
			req[i] = (uint8_t)x;
			rep[i] = (uint8_t)y;
		}
		memset(frame, 0, sizeof(frame));
		memset(&id, 0, sizeof(id));
		ct_check(vms_dlm_op0f_ack_build(req, sizeof(req), &id, frame,
						sizeof(frame), &written) ==
			 VMS_CODEC_OK, "an op-0x0f answer builds");
		diffs = 0;
		for (i = 4u; i < 132u; i++)
			if (frame[VMS_OFF_SYSAP_BODY + i] != rep[i])
				diffs++;
		ct_check_eq_u32(diffs, 0u,
				"*** the op-0x0f answer equals the real VAX "
				"member's, byte for byte after the envelope ***");
		ct_check_eq_u32(id.name_len, req[47],
				"  ... and the identity names the request's resource");
		ct_check(memcmp(id.name, &req[48], id.name_len) == 0,
			 "  ... by the request's own name bytes");
		ct_check(vms_dlm_op0f_ack_build(rep, sizeof(rep), &id, frame,
						sizeof(frame), &written) !=
			 VMS_CODEC_OK, "  ... and a RESPONSE is refused as input");
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
	test_rebuild_rejoin_roundtrip();
	test_allowlist_rows();
	test_no_builder_accepts_a_placeholder_lock_id();
	test_dir_hash_accessor();
	test_body_entries_are_what_scs_delivers();
	test_dlksrch_twin();

	test_queued_convert_answer_is_the_real_one();
	test_op0f_ack_is_the_real_one();
	return ct_summary("test_codec_dlm");
}
