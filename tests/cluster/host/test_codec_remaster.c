// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_remaster.c - lock-tree remastering on the wire, rung R1
 * (rd vms-380, under rd vms-50d; the done-condition "codec + byte-exact tests
 * on captured pairs, negctl per cell").
 *
 * EVERY POSITIVE ASSERTION HERE IS AGAINST A REAL VAX V7.3 FRAME, loaded from
 * a clean-room specimen whose capture is in docs/clean-room/
 * reference-captures.sha256. Nothing is composed: a builder is called with the
 * typed fields an executive read would supply, and the result is compared byte
 * for byte against what the real new master sent, over the whole 132-byte
 * SYSAP body. A mismatch names the offset.
 *
 * GROUPS
 *   1. The 82/15 ANSWER (rule 1): for op 0x0e, 0x10 and 0x14 the answer is the
 *      request with body[8], body[9] and body[34] rewritten and NOTHING else;
 *      for op 0x0f the new master also clears body[28:32]. Each proven against
 *      the real answer that followed that exact request in the capture.
 *   2. The 82/01 ANSWER to an op-0x12 (rule 2): built from the handle the
 *      adopting engine minted, plus -- on the ROOT record -- the value block
 *      and its sequence. Byte-exact except the one cell this codec refuses to
 *      guess, which the test asserts IS the only difference, by name.
 *   3. The op-0x12 RECORD PARSE: the cells SDA named in RA2/RA4/RA5/RA6, read
 *      off the real holder's record, root and sub.
 *   4. The op-0x14 BUILDER (rule 3): the six fields it asserts are the real
 *      frame's, every other byte is the residue fill, and the RA7 second
 *      pattern lands only in the residue spans.
 *   5. NEGATIVE CONTROLS, one per cell the codec decides: a placeholder lock
 *      id is refused, a wrong opcode is refused, an answer is refused as
 *      input, and the value block does not ride without an executive read.
 *   6. HELD-OUT TOPOLOGY: the same rules against RA6, where the old master,
 *      the new master and the holder are three different nodes from RA5's and
 *      the CSIDs are not in node-number order.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "conformance_dump.h"
#include "vms_cluster_codec_cm.h"   /* VMS_CM_BODY_LEN */
#include "vms_cluster_codec_dlm.h"

#include <stdio.h>
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

/* body[] of a specimen: the 132 bytes SCS really delivers to the SYSAP. */
static const uint8_t *fxbody(const struct vms_fixture *f)
{
	return f->bytes + VMS_OFF_SYSAP_BODY;
}

/*
 * Compare a BUILT frame's SYSAP body against the real answer's, skipping the
 * four envelope bytes body[0:4] (the CM's send/ack message numbers -- a
 * per-connection counter no codec owns) and any offsets the caller names as
 * EXPECTED to differ. Returns the number of unexpected differences and prints
 * each one, so a regression names its own offset.
 */
static unsigned body_diff(const uint8_t *built, const uint8_t *real,
			  const unsigned *expect, unsigned n_expect,
			  const char *label)
{
	unsigned i, j, bad = 0;

	for (i = 4u; i < VMS_CM_BODY_LEN; i++) {
		int excused = 0;

		if (built[i] == real[i])
			continue;
		for (j = 0; j < n_expect; j++) {
			if (expect[j] == i)
				excused = 1;
		}
		if (excused)
			continue;
		printf("       %s: body[%u] built %02x, real VAX %02x\n",
		       label, i, built[i], real[i]);
		bad++;
	}
	return bad;
}

/* ---- group 1: the 82/15 answer ---------------------------------------- */

/*
 * One answered step: build the answer from the real request and require it to
 * be the real answer, byte for byte past the envelope.
 */
static void check_ack_pair(const char *req_name, const char *ans_name,
			   int new_master, const char *what)
{
	const struct vms_fixture *req = fixture(req_name);
	const struct vms_fixture *ans = fixture(ans_name);
	uint8_t built[256];
	uint32_t written = 0;

	printf("-- %s (%s -> %s)\n", what, req_name, ans_name);
	ct_check(req != NULL && ans != NULL, "both specimens load");
	if (req == NULL || ans == NULL)
		return;

	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_rm_ack_build(fxbody(req), VMS_CM_BODY_LEN, new_master,
				      built, sizeof(built),
				      &written) == VMS_CODEC_OK,
		 "  the answer builds");
	ct_check_eq_u32(written, VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN,
			"  and is a whole SYSAP body long");
	/* Two emitted SHAPES, not two roles: `new_master` changes the frame
	 * only for an op-0x0f, so only that combination is the cleared shape
	 * the conformance gate knows as dlm_rm_ack_nm. */
	conf_dump((new_master != 0 &&
		   fxbody(req)[VMS_OFB_DLM_OP] == VMS_DLM_WIREOP_RM_0F) ?
		  "dlm_rm_ack_nm" : "dlm_rm_ack",
		  VMS_OFF_SYSAP_BODY, "-", built + VMS_OFF_SYSAP_BODY,
		  VMS_CM_BODY_LEN);
	ct_check(body_diff(built + VMS_OFF_SYSAP_BODY, fxbody(ans), NULL, 0,
			   what) == 0,
		 "*** the answer equals the real VAX's, byte for byte past the "
		 "envelope ***");
}

static void test_ack_is_the_real_answer(void)
{
	check_ack_pair("rm-0e-req", "rm-0e-ans", 1,
		       "op 0x0e answered by the new master");
	check_ack_pair("rm-10-req", "rm-10-ans", 1,
		       "op 0x10 answered by the new master");
	check_ack_pair("rm-14-oldmaster-req", "rm-14-oldmaster-ans", 0,
		       "op 0x14 answered by the old master");
	check_ack_pair("rm-14-holder-req", "rm-14-holder-ans", 0,
		       "op 0x14 answered by a holder (body[34] f9 -> fa)");
	check_ack_pair("rm-0f-newmaster-req", "rm-0f-newmaster-ans", 1,
		       "op 0x0f answered by the NEW MASTER (body[28:32] cleared)");
	check_ack_pair("rm-0f-member-req", "rm-0f-member-ans", 0,
		       "op 0x0f answered by a PLAIN MEMBER (body[28:32] kept)");
	check_ack_pair("rm6-0e-req", "rm6-0e-ans", 1,
		       "HELD OUT: op 0x0e, RA6 topology");
}

/*
 * THE NARROWING THIS BUILDER DID, PINNED. vms_dlm_op0f_ack_build used to write
 * the whole grant record at body[32:36]; it now writes only body[34]. The two
 * are identical on every real op-0x0f -- which is why the change is safe --
 * and they are NOT identical on an op-0x0e, where body[32] is this request's
 * own byte. Both halves are asserted, so neither can regress silently.
 */
static void test_ack_echoes_the_cells_a_grant_would_overwrite(void)
{
	const struct vms_fixture *req = fixture("rm-0e-req");
	uint8_t built[256];

	printf("-- the 82/15 answer ECHOES body[32], [33] and [35]\n");
	ct_check(req != NULL, "specimen loads");
	if (req == NULL)
		return;
	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_rm_ack_build(fxbody(req), VMS_CM_BODY_LEN, 1, built,
				      sizeof(built), NULL) == VMS_CODEC_OK,
		 "  builds");
	ct_check_eq_u32(built[VMS_OFF_DLM_RESULT_STAMP], 0xfau,
			"  body[34] is the outcome byte, 0xfa (246/246 real)");
	ct_check(built[VMS_OFF_DLM_GRANT_REC] ==
		 fxbody(req)[VMS_OFB_FROM_FRAME(VMS_OFF_DLM_GRANT_REC)],
		 "  body[32] is the REQUEST'S byte, not the grant's 0x01");
	ct_check(built[VMS_OFF_DLM_GRANT_REC + 3u] ==
		 fxbody(req)[VMS_OFB_FROM_FRAME(VMS_OFF_DLM_GRANT_REC) + 3u],
		 "  body[35] is the REQUEST'S byte too");
}

/*
 * The op-0x0f ROLE, which is the only thing `new_master` selects, and it
 * selects it for op 0x0f ALONE: a new master's 0x0e answer does not clear
 * body[28:32] (60/60 real 0x0e pairs leave it alone).
 */
static void test_only_op0f_clears_the_master_cell(void)
{
	const struct vms_fixture *req = fixture("rm-0e-req");
	uint8_t as_new[256], as_member[256];

	printf("-- new_master clears body[28:32] on op 0x0f and nothing else\n");
	ct_check(req != NULL, "specimen loads");
	if (req == NULL)
		return;
	memset(as_new, 0xa5, sizeof(as_new));
	memset(as_member, 0x5a, sizeof(as_member));
	ct_check(vms_dlm_rm_ack_build(fxbody(req), VMS_CM_BODY_LEN, 1, as_new,
				      sizeof(as_new), NULL) == VMS_CODEC_OK &&
		 vms_dlm_rm_ack_build(fxbody(req), VMS_CM_BODY_LEN, 0,
				      as_member, sizeof(as_member),
				      NULL) == VMS_CODEC_OK,
		 "  both roles build an op-0x0e answer");
	ct_check(memcmp(as_new + VMS_OFF_SYSAP_BODY,
			as_member + VMS_OFF_SYSAP_BODY,
			VMS_CM_BODY_LEN) == 0,
		 "*** an op-0x0e answer is the SAME in both roles ***");
}

/* ---- group 3: the op-0x12 record parse -------------------------------- */

static void test_op12_root_record_parse(void)
{
	const struct vms_fixture *f = fixture("rm-12-root-req");
	struct vms_dlm_rm_lock_rec r;

	printf("-- rm-12-root-req: the holder's ROOT record, cells SDA named\n");
	ct_check(f != NULL, "specimen loads");
	if (f == NULL)
		return;
	memset(&r, 0, sizeof(r));
	ct_check(vms_dlm_rm_lock_rec_parse_body(fxbody(f), VMS_CM_BODY_LEN,
						&r) == VMS_CODEC_OK,
		 "  parses as an op-0x12 record");
	ct_check_eq_u32(r.holder_lkid, 0x110000ceu,
			"  body[24:28]: VAX3's own root lock id (SDA before "
			"the move: 110000CE CR, PID 00010016)");
	ct_check_eq_u32(r.holder_epid, 0x20600216u,
			"  body[20:24]: the holder process's EPID on VAX3");
	ct_check_eq_u32(r.granted_mode, VMS_LCK_CR,
			"  body[30]: granted CR, as SDA showed");
	ct_check_eq_u32(r.requested_mode, VMS_LCK_CR, "  body[31]: requested CR");
	ct_check_eq_u32(r.flags, 0x11u, "  body[28]: 0x11 on a root record");
	ct_check_eq_u32(r.is_root, 1, "  the parent span is zero: this is the ROOT");
	ct_check_eq_u32(r.parent_holder_lkid, 0, "  no parent holder handle");
	ct_check_eq_u32(r.parent_master_lkid, 0, "  no parent master handle");
	ct_check(r.id.name_len == 4u && memcmp(r.id.name, "RMT5", 4) == 0,
		 "  the record names the tree's root resource, RMT5");
	ct_check_eq_u32(r.id.mode, 3u, "  body[46]: the resource's access mode");
	ct_check_eq_u32(r.dlm_seq, 3u,
			"  body[14:16]: the cluster's DLM rebuild sequence");
}

static void test_op12_sub_record_parse(void)
{
	const struct vms_fixture *f = fixture("rm-12-sub-req");
	struct vms_dlm_rm_lock_rec r;

	printf("-- rm-12-sub-req: a SUB-resource record names its parent twice\n");
	ct_check(f != NULL, "specimen loads");
	if (f == NULL)
		return;
	memset(&r, 0, sizeof(r));
	ct_check(vms_dlm_rm_lock_rec_parse_body(fxbody(f), VMS_CM_BODY_LEN,
						&r) == VMS_CODEC_OK,
		 "  parses as an op-0x12 record");
	ct_check_eq_u32(r.is_root, 0, "  not the root");
	ct_check_eq_u32(r.parent_holder_lkid, 0x110000ceu,
			"  body[36:40]: the parent's HOLDER lock id -- the "
			"same root lock the root record handed over");
	ct_check_eq_u32(r.parent_master_lkid, 0x020004ccu,
			"  body[40:44]: the parent's NEW-MASTER lock id, the "
			"handle the adopting engine minted and answered");
	ct_check_eq_u32(r.granted_mode, VMS_LCK_CW, "  body[30]: granted CW");
	ct_check(r.id.name_len == 2u && memcmp(r.id.name, "S2", 2) == 0,
		 "  and it names the sub-resource, S2");
}

/* ---- group 2: the 82/01 answer to an op-0x12 -------------------------- */

/*
 * THE FOUR CELLS OF AN 0x12 ANSWER THIS CODEC WILL NOT GUESS, named here once
 * so both the derivation and the held-out test exclude exactly the same set
 * and nothing creeps in:
 *
 *   body[30:32]  f0 01 (5 of 8 real ROOT answers) or f1 01 (3 of 8); 0 0 on
 *                408 of 408 SUB answers and on 38 of 38 plain ENQ grants.
 *                `root_ctx` opt-in; this codec writes the grant's 0 0.
 *   body[32:34]  0x0001 in 283 of 416 real answers -- the value the GRANT
 *                record grounds, 38/38 -- and a per-remaster number on the
 *                rest (0x0365 across all of RMT6's, 0x036e across all of
 *                RMT7's, 0x07b8 in RA2). It reappears as the low half of the
 *                op-0x14's body[20:24] on the same node, so it is a handle in
 *                a second, node-local index space this executive does not
 *                have. This codec writes the grounded 0x0001.
 *
 * Both are recorded as `deviation` rows in tests/cluster/conformance/
 * frame-grounding.tsv so the gate reports them on every fire.
 */
static const unsigned g_unnamed_0x12_cells[4] = { 30u, 31u, 32u, 33u };

/*
 * THE ROOT ANSWER. Built from the handle the engine minted plus the resource's
 * value block and sequence; byte-exact against the real grant EXCEPT body[30]
 * and body[31], the pair this codec will not guess (header rule 2). The test
 * asserts those two ARE the only difference -- so the day an arm names the
 * cell, this test tells us the rest was already right.
 */
static void test_op12_root_answer_is_the_real_grant(void)
{
	const struct vms_fixture *req = fixture("rm-12-root-req");
	const struct vms_fixture *ans = fixture("rm-12-root-ans");
	struct vms_dlm_rm_lock_answer a;
	uint8_t built[256];

	printf("-- rm-12-root-ans: the new master's GRANT for the root lock\n");
	ct_check(req != NULL && ans != NULL, "both specimens load");
	if (req == NULL || ans == NULL)
		return;

	memset(&a, 0, sizeof(a));
	/* Every field an executive read supplies: the handle the adoption
	 * minted (SDA on VAX2 after the move), and the RSB's own block. */
	a.master_lkid = 0x020004ccu;
	a.valblk_present = 1u;
	memcpy(a.valblk, "RMLVB-0123456789", VMS_DLM_VALBLK_WIRE_LEN);
	a.valblk_seq = 1u;

	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(req), VMS_CM_BODY_LEN, &a,
					      built, sizeof(built),
					      NULL) == VMS_CODEC_OK,
		 "  the answer builds");
	conf_dump("dlm_rm_lock_ans", VMS_OFF_SYSAP_BODY,
		  "lkid=0x020004cc", built + VMS_OFF_SYSAP_BODY,
		  VMS_CM_BODY_LEN);
	ct_check(body_diff(built + VMS_OFF_SYSAP_BODY, fxbody(ans),
			   g_unnamed_0x12_cells, 4u, "root 82/01") == 0,
		 "*** the root grant equals the real VAX's everywhere but the "
		 "cells rd vms-380 cannot name ***");
	ct_check(fxbody(ans)[30] == 0xf0u && fxbody(ans)[31] == 0x01u,
		 "  the real VAX wrote f0 01 at body[30:32] on this root");
	ct_check(fxbody(ans)[32] == 0x01u && fxbody(ans)[33] == 0x00u,
		 "  and 0x0001 at body[32:34] -- here it AGREES with the "
		 "grant record this codec writes");
	ct_check(built[VMS_OFF_DLM_MODE] == 0u &&
		 built[VMS_OFF_DLM_MODE + 1u] == 0u,
		 "  this codec writes the grant's 0 0 there, not a guess");
	ct_check_eq_u32(built[VMS_OFF_DLM_GRANT_FLAG], 0x10u,
			"  body[28] = 0x10, the grant flag (416/416 real)");
	ct_check_eq_u32(built[VMS_OFF_DLM_RESULT_STAMP], 0xfau,
			"  body[34] = 0xfa, granted");
}

static void test_op12_sub_answer_is_the_real_grant(void)
{
	const struct vms_fixture *req = fixture("rm-12-sub-req");
	const struct vms_fixture *ans = fixture("rm-12-sub-ans");
	struct vms_dlm_rm_lock_answer a;
	uint8_t built[256];

	printf("-- rm-12-sub-ans: a SUB record's grant carries no value block\n");
	ct_check(req != NULL && ans != NULL, "both specimens load");
	if (req == NULL || ans == NULL)
		return;
	memset(&a, 0, sizeof(a));
	a.master_lkid = 0x020004cbu;   /* SDA on VAX2 after the move */
	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(req), VMS_CM_BODY_LEN, &a,
					      built, sizeof(built),
					      NULL) == VMS_CODEC_OK,
		 "  the answer builds");
	conf_dump("dlm_rm_lock_ans", VMS_OFF_SYSAP_BODY, "lkid=0x020004cb",
		  built + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	ct_check(body_diff(built + VMS_OFF_SYSAP_BODY, fxbody(ans), NULL, 0,
			   "sub 82/01") == 0,
		 "*** the sub grant equals the real VAX's, byte for byte past "
		 "the envelope ***");
}

/* ---- group 4: the op-0x14 builder ------------------------------------ */

/*
 * THE SIX FIELDS AN op-0x14 ASSERTS, each compared against the real frame, and
 * the residue span proven to be the vms-42de ruling's zeros.
 */
static void test_op14_asserts_six_fields_and_zeroes_the_rest(void)
{
	const struct vms_fixture *real = fixture("rm-14-oldmaster-req");
	struct vms_dlm_rm_held h;
	uint8_t built[256];
	const uint8_t *b;
	uint32_t written = 0, i;
	unsigned nonzero_outside = 0;

	printf("-- the op-0x14 builder: six fields, and zeros everywhere else\n");
	ct_check(real != NULL, "the real 0x14 specimen loads");
	if (real == NULL)
		return;

	memset(&h, 0, sizeof(h));
	h.id.group = 0u;
	h.id.mode = 3u;
	h.id.name_len = 4u;
	memcpy(h.id.name, "RMT5", 4);
	h.id.hash = 0xc54eeaeau;   /* the hash the executive holds for RMT5 */
	h.hash_known = 1u;

	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_rm_held_build(&h, built, sizeof(built),
				       &written) == VMS_CODEC_OK,
		 "  it builds");
	ct_check_eq_u32(written, VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN,
			"  and is a whole SYSAP body long");
	b = built + VMS_OFF_SYSAP_BODY;
	conf_dump("dlm_rm_14", VMS_OFF_SYSAP_BODY,
		  "group=0,acmode=3,namelen=4,hash=0xc54eeaea", b,
		  VMS_CM_BODY_LEN);

	/* The six fields, each equal to the real frame's. */
	ct_check(b[8] == fxbody(real)[8] && b[8] == 0x02u,
		 "  body[8]  = 0x02, the real frame's (63/63)");
	ct_check(b[9] == fxbody(real)[9] && b[9] == 0x14u,
		 "  body[9]  = 0x14, the real frame's (63/63)");
	ct_check(memcmp(b + 40, fxbody(real) + 40, 4) == 0 &&
		 b[40] == 0 && b[41] == 0 && b[42] == 0 && b[43] == 0,
		 "  body[40:44] = 0, the real frame's (63/63)");
	ct_check(memcmp(b + 44, fxbody(real) + 44, 4) == 0,
		 "  body[44:48] = the tree's identity, the real frame's");
	ct_check(memcmp(b + 48, fxbody(real) + 48, 4) == 0,
		 "  body[48:48+L] = the root resource name, the real frame's");
	ct_check(memcmp(b + 128, fxbody(real) + 128, 4) == 0,
		 "  body[128:132] = the directory hash, the real frame's");

	/* And nothing else is asserted: the whole residue span is zero. */
	for (i = 0u; i < VMS_CM_BODY_LEN; i++) {
		int named = (i >= 8u && i < 10u) || (i >= 40u && i < 52u) ||
			    (i >= 128u);

		if (!named && b[i] != 0u)
			nonzero_outside++;
	}
	ct_check(nonzero_outside == 0,
		 "*** every byte outside the six fields is ZERO -- the "
		 "rd vms-42de ruling, not OVMX kernel memory ***");
	ct_check(b[12] == 0u && b[13] == 0u && b[14] == 0u && b[15] == 0u,
		 "  body[12:16] is left for the CM's own DLM-sequence stamp");
}

/*
 * RA7's SECOND FIXED PATTERN (rd vms-6ad1) lands in the residue span and
 * NOWHERE ELSE. This is the knob the proof arm turns, so the test that the
 * named fields survive it is the test that makes the arm interpretable.
 */
static void test_op14_ra7_pattern_touches_only_residue(void)
{
	struct vms_dlm_rm_held h;
	uint8_t zeros[256], patt[256];
	const uint8_t *z, *p;
	uint32_t i;
	unsigned differ_named = 0, differ_residue = 0;

	printf("-- RA7: the second fixed pattern lands only in the residue\n");
	memset(&h, 0, sizeof(h));
	h.id.mode = 3u;
	h.id.name_len = 4u;
	memcpy(h.id.name, "RMT5", 4);
	h.id.hash = 0xc54eeaeau;
	h.hash_known = 1u;

	memset(zeros, 0, sizeof(zeros));
	memset(patt, 0, sizeof(patt));
	ct_check(vms_dlm_rm_held_build(&h, zeros, sizeof(zeros),
				       NULL) == VMS_CODEC_OK,
		 "  the zero form builds");
	h.residue_fill = 0x5au;
	ct_check(vms_dlm_rm_held_build(&h, patt, sizeof(patt),
				       NULL) == VMS_CODEC_OK,
		 "  the pattern form builds");
	z = zeros + VMS_OFF_SYSAP_BODY;
	p = patt + VMS_OFF_SYSAP_BODY;
	conf_dump("dlm_rm_14_ra7", VMS_OFF_SYSAP_BODY,
		  "group=0,acmode=3,namelen=4,hash=0xc54eeaea", p,
		  VMS_CM_BODY_LEN);
	for (i = 0u; i < VMS_CM_BODY_LEN; i++) {
		/* body[8:10] cat/op, body[12:16] the CM's DLM-sequence stamp,
		 * body[40:52] the zero word + the identity + the name,
		 * body[128:132] the hash. body[10:12] is RESIDUE. */
		int named = (i >= 8u && i < 10u) || (i >= 12u && i < 16u) ||
			    (i >= 40u && i < 52u) || (i >= 128u);

		if (z[i] == p[i])
			continue;
		if (named)
			differ_named++;
		else
			differ_residue++;
	}
	ct_check(differ_named == 0,
		 "*** not one named field moves when the pattern changes ***");
	ct_check(differ_residue > 0,
		 "  and the residue span really did change (non-vacuous)");
	ct_check(p[40] == 0u && p[41] == 0u && p[42] == 0u && p[43] == 0u,
		 "  body[40:44] stays 0 under the pattern (63/63 real)");
}

/* ---- group 5: negative controls -------------------------------------- */

static void test_negative_controls(void)
{
	const struct vms_fixture *req12 = fixture("rm-12-root-req");
	const struct vms_fixture *ans = fixture("rm-0e-ans");
	const struct vms_fixture *notif = fixture("rm-13-notif");
	struct vms_dlm_rm_lock_answer a;
	struct vms_dlm_rm_lock_rec r;
	struct vms_dlm_rm_step s;
	struct vms_dlm_rm_held h;
	uint8_t built[256];

	printf("-- negative controls\n");
	ct_check(req12 != NULL && ans != NULL && notif != NULL,
		 "specimens load");
	if (req12 == NULL || ans == NULL || notif == NULL)
		return;

	/* A placeholder lock id is the fc8540ae INVLOCKID crash. */
	memset(&a, 0, sizeof(a));
	a.master_lkid = VMS_DLM_LKID_UNSET;
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(req12), VMS_CM_BODY_LEN,
					      &a, built, sizeof(built),
					      NULL) != VMS_CODEC_OK,
		 "a 0x12 answer REFUSES lock id 0 (the fc8540ae INVLOCKID "
		 "crash, encoded)");

	/* An answer is not an input to an answer builder. */
	a.master_lkid = 0x020004ccu;
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(ans), VMS_CM_BODY_LEN, &a,
					      built, sizeof(built),
					      NULL) == VMS_CODEC_E_CLASS,
		 "a cat-0x82 reply is refused as a 0x12 to answer");
	ct_check(vms_dlm_rm_ack_build(fxbody(ans), VMS_CM_BODY_LEN, 0, built,
				      sizeof(built), NULL) == VMS_CODEC_E_CLASS,
		 "a cat-0x82 reply is refused as a step to acknowledge");

	/* The ops each builder will NOT answer. */
	ct_check(vms_dlm_rm_ack_build(fxbody(notif), VMS_CM_BODY_LEN, 0, built,
				      sizeof(built), NULL) == VMS_CODEC_E_CLASS,
		 "an op-0x13 is a NOTIFICATION and gets no 82/15");
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(notif), VMS_CM_BODY_LEN,
					      &a, built, sizeof(built),
					      NULL) == VMS_CODEC_E_CLASS,
		 "an op-0x13 is refused as a lock record");
	ct_check(vms_dlm_rm_lock_rec_parse_body(fxbody(notif), VMS_CM_BODY_LEN,
						&r) == VMS_CODEC_E_CLASS,
		 "... and the record parser refuses it too");
	ct_check(vms_dlm_rm_step_parse_body(fxbody(req12), VMS_CM_BODY_LEN,
					    &s) == VMS_CODEC_E_CLASS,
		 "an op-0x12 is refused by the STEP parser");

	/* A value block does not ride without an executive read for it. */
	memset(&a, 0, sizeof(a));
	a.master_lkid = 0x020004ccu;
	a.valblk_seq = 7u;
	memcpy(a.valblk, "XXXXXXXXXXXXXXXX", VMS_DLM_VALBLK_WIRE_LEN);
	memset(built, 0, sizeof(built));
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(req12), VMS_CM_BODY_LEN,
					      &a, built, sizeof(built),
					      NULL) == VMS_CODEC_OK &&
		 built[VMS_OFF_DLM_VALBLK] == 0u &&
		 built[VMS_OFF_DLM_VALBLK + 15u] == 0u &&
		 built[VMS_OFF_DLM_VALBLK_SERIAL2] == 0u,
		 "valblk_present 0 writes NO value block and NO sequence, "
		 "whatever the struct holds");

	/* The 0x14 builder refuses a nameless tree: "which resource" has no
	 * honest default (the same refusal vms_dlm_adopt_open makes). */
	memset(&h, 0, sizeof(h));
	ct_check(vms_dlm_rm_held_build(&h, built, sizeof(built),
				       NULL) != VMS_CODEC_OK,
		 "an op-0x14 with no resource name is refused");
	h.id.name_len = 4u;
	memcpy(h.id.name, "RMT5", 4);
	memset(built, 0, sizeof(built));
	ct_check(vms_dlm_rm_held_build(&h, built, sizeof(built),
				       NULL) == VMS_CODEC_OK &&
		 built[VMS_OFF_DLM_DIR_HASH] == 0u &&
		 built[VMS_OFF_DLM_DIR_HASH + 3u] == 0u,
		 "hash_known 0 leaves body[128:132] zero rather than mint a "
		 "directory hash");

	/* A short body is short, not guessed at. */
	ct_check(vms_dlm_rm_ack_build(fxbody(ans), 40u, 0, built,
				      sizeof(built), NULL) == VMS_CODEC_E_SHORT,
		 "a body shorter than a SYSAP body is refused");
}

/* ---- the notifications ----------------------------------------------- */

static void test_notifications_are_notifications(void)
{
	const struct vms_fixture *n11 = fixture("rm-11-notif");
	const struct vms_fixture *n13 = fixture("rm-13-notif");
	const struct vms_fixture *req = fixture("rm-0e-req");
	struct vms_dlm_rm_step s;

	printf("-- op 0x11 and op 0x13 open no transaction and are never "
	       "answered (68/68 real)\n");
	ct_check(n11 != NULL && n13 != NULL && req != NULL, "specimens load");
	if (n11 == NULL || n13 == NULL || req == NULL)
		return;

	memset(&s, 0, sizeof(s));
	ct_check(vms_dlm_rm_step_parse_body(fxbody(n11), VMS_CM_BODY_LEN,
					    &s) == VMS_CODEC_OK &&
		 s.op == VMS_DLM_WIREOP_RM_11 && s.notification == 1u,
		 "  the op-0x11 parses and reports itself a notification");
	ct_check(s.id.name_len == 4u && memcmp(s.id.name, "RMT5", 4) == 0,
		 "  and names the tree");
	memset(&s, 0, sizeof(s));
	ct_check(vms_dlm_rm_step_parse_body(fxbody(n13), VMS_CM_BODY_LEN,
					    &s) == VMS_CODEC_OK &&
		 s.op == VMS_DLM_WIREOP_RM_13 && s.notification == 1u,
		 "  the op-0x13 end-of-records likewise");
	memset(&s, 0, sizeof(s));
	ct_check(vms_dlm_rm_step_parse_body(fxbody(req), VMS_CM_BODY_LEN,
					    &s) == VMS_CODEC_OK &&
		 s.notification == 0u,
		 "  while an op-0x0e opens a transaction and WILL be answered");
}

/* ---- group 6: the held-out topology ---------------------------------- */

/*
 * RA6 (pod labrm6b): the old master, the new master and the holder are three
 * different nodes from RA5's, LOCKDIRWT is 1/1/1 instead of 0/0/x, and the
 * CSIDs are not in node-number order. The rules are re-proven there, and the
 * cell this codec refuses to guess shows a DIFFERENT value (f1 01 vs f0 01) --
 * which is exactly why it is not guessed.
 */
static void test_heldout_topology(void)
{
	const struct vms_fixture *req = fixture("rm6-12-root-req");
	const struct vms_fixture *ans = fixture("rm6-12-root-ans");
	struct vms_dlm_rm_lock_answer a;
	struct vms_dlm_rm_lock_rec r;
	uint8_t built[256];
	char state[48];

	printf("-- HELD OUT (RA6, O=VAX2 N=VAX3 H=VAX1, LOCKDIRWT 1/1/1)\n");
	ct_check(req != NULL && ans != NULL, "both specimens load");
	if (req == NULL || ans == NULL)
		return;

	memset(&r, 0, sizeof(r));
	ct_check(vms_dlm_rm_lock_rec_parse_body(fxbody(req), VMS_CM_BODY_LEN,
						&r) == VMS_CODEC_OK,
		 "  the holder's root record parses");
	ct_check_eq_u32(r.is_root, 1, "  and is the root");

	memset(&a, 0, sizeof(a));
	/* The handle the real new master minted, read out of its own answer --
	 * which in the arm comes from the adoption engine. */
	a.master_lkid = (uint32_t)fxbody(ans)[20] |
			((uint32_t)fxbody(ans)[21] << 8) |
			((uint32_t)fxbody(ans)[22] << 16) |
			((uint32_t)fxbody(ans)[23] << 24);
	a.valblk_present = 1u;
	memcpy(a.valblk, fxbody(ans) + 36, VMS_DLM_VALBLK_WIRE_LEN);
	a.valblk_seq = (uint32_t)fxbody(ans)[52] |
		       ((uint32_t)fxbody(ans)[53] << 8) |
		       ((uint32_t)fxbody(ans)[54] << 16) |
		       ((uint32_t)fxbody(ans)[55] << 24);
	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_rm_lock_answer_build(fxbody(req), VMS_CM_BODY_LEN, &a,
					      built, sizeof(built),
					      NULL) == VMS_CODEC_OK,
		 "  the grant builds");
	snprintf(state, sizeof(state), "lkid=0x%08x", a.master_lkid);
	conf_dump("dlm_rm_lock_ans", VMS_OFF_SYSAP_BODY, state,
		  built + VMS_OFF_SYSAP_BODY, VMS_CM_BODY_LEN);
	ct_check(body_diff(built + VMS_OFF_SYSAP_BODY, fxbody(ans),
			   g_unnamed_0x12_cells, 4u, "RA6 root 82/01") == 0,
		 "*** HELD OUT: the root grant equals the real VAX's "
		 "everywhere but the cells rd vms-380 cannot name ***");
	ct_check(fxbody(ans)[30] == 0xf1u,
		 "  body[30] reads f1 here where RA5 read f0 -- one cell, "
		 "two values, two clusters: not a constant to copy");
	ct_check(fxbody(ans)[32] == 0x6eu && fxbody(ans)[33] == 0x03u,
		 "  and body[32:34] reads 0x036e here where RA5 read 0x0001: "
		 "the held-out capture is what caught this cell. This codec "
		 "writes the grounded 0x0001 and the gate reports the "
		 "deviation");
	ct_check_eq_u32(a.valblk_seq, 1u,
			"  the RA6 root's value-block sequence is 1 too");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_codec_remaster: lock-tree remastering on the wire "
	       "(rd vms-380)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES,
				   err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}

	test_ack_is_the_real_answer();
	test_ack_echoes_the_cells_a_grant_would_overwrite();
	test_only_op0f_clears_the_master_cell();
	test_op12_root_record_parse();
	test_op12_sub_record_parse();
	test_op12_root_answer_is_the_real_grant();
	test_op12_sub_answer_is_the_real_grant();
	test_op14_asserts_six_fields_and_zeroes_the_rest();
	test_op14_ra7_pattern_touches_only_residue();
	test_notifications_are_notifications();
	test_heldout_topology();
	test_negative_controls();
	return ct_summary("test_codec_remaster");
}
