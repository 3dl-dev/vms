/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_dir.c - rd vms-8219: the lock directory's ENTRIES and the
 * directory-role codec, against REAL frames from a private three-node
 * OpenVMS VAX V7.3 cluster whose only directory node was VAX1.
 *
 *   1. The identity a lookup / removal / registration carries is read from
 *      the same four positions in all three (group, mode, name, hash), and a
 *      sub-resource lookup is not a directory lookup.
 *   2. OVMX's answer is the real directory's answer: the 0xf9 "you master
 *      it" and the 0xf8 "the master is X" built from the REAL request are
 *      byte-identical to VAX1's REAL answer everywhere but body[0:8] (the
 *      envelope, the CM wrapper's) and the bytes VAX1 left as stale buffer
 *      (body[28:39] outside the outcome and the redirect CSID).
 *   3. The table: lookup outcomes (p. 6-31 2/3), removal only by the master,
 *      a departure, a vector change, a full table refused rather than
 *      answered, and a randomized model check of insert/remove under the
 *      back-shift deletion.
 */
#include "cluster_fixture.h"
#include "cluster_test.h"
#include "vms_cluster_codec_dlm.h"
#include "vms_cluster_codec_cm.h"
#include "vms_dlm_dir.h"

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

static const uint8_t *body_of(const struct vms_fixture *f)
{
	return f->bytes + VMS_OFF_SYSAP_BODY;
}

static uint32_t blen_of(const struct vms_fixture *f)
{
	return f->wire_len - VMS_OFF_SYSAP_BODY;
}

static void ident(struct vms_dlm_res_ident *id, const char *name,
		  uint16_t group, uint8_t mode, uint32_t hash)
{
	memset(id, 0, sizeof(*id));
	id->group = group;
	id->mode = mode;
	id->hash = hash;
	id->name_len = (uint8_t)strlen(name);
	memcpy(id->name, name, id->name_len);
}

/* ==========================================================================
 * 1. The identity, from real frames
 * ========================================================================== */
static void test_identity_from_real_frames(void)
{
	const struct vms_fixture *look = fixture("dlm-dirhash-root-vax2");
	const struct vms_fixture *sub = fixture("dlm-dirhash-sub-vax2");
	const struct vms_fixture *rem = fixture("dlm-dir-remove-vax3");
	const struct vms_fixture *reg = fixture("dlm-dir-register-vax1");
	struct vms_dlm_res_ident id;

	printf("-- the resource identity a directory reads (real frames)\n");
	ct_check(look && sub && rem && reg, "the four specimens load");
	if (!look || !sub || !rem || !reg)
		return;

	ct_check(vms_dlm_res_ident_parse_body(body_of(look), blen_of(look), &id)
		 == VMS_CODEC_OK, "a ROOT op-0x01 lookup yields an identity");
	ct_check(id.name_len == 5 && memcmp(id.name, "DLMTA", 5) == 0,
		 "  name DLMTA");
	ct_check_eq_u32(id.mode, 3u, "  access mode 3 (user, body[46])");
	ct_check_eq_u32(id.group, 0u, "  group 0 (system-wide, body[44:46])");
	ct_check_eq_u32(id.hash, 0x00336fe3u, "  hash e3 6f 33 00 (body[128:132])");

	ct_check(vms_dlm_res_ident_parse_body(body_of(sub), blen_of(sub), &id)
		 == VMS_CODEC_E_CLASS,
		 "a SUB-resource op-0x01 is NOT a directory lookup");

	ct_check(vms_dlm_res_ident_parse_body(body_of(rem), blen_of(rem), &id)
		 == VMS_CODEC_OK && id.name_len == 5 &&
		 memcmp(id.name, "DLMTC", 5) == 0 && id.mode == 3u,
		 "a named op-0x04 removal yields DLMTC, mode 3");
	ct_check(vms_dlm_res_ident_parse_body(body_of(reg), blen_of(reg), &id)
		 == VMS_CODEC_OK && id.name_len > 0u,
		 "an op-0x0d registration yields an identity");
	{
		uint8_t b[VMS_CM_BODY_LEN];

		memcpy(b, body_of(rem), VMS_CM_BODY_LEN);
		b[47] = 0u;
		ct_check(vms_dlm_res_ident_parse_body(b, VMS_CM_BODY_LEN, &id)
			 == VMS_CODEC_E_CLASS,
			 "an UNNAMED op-0x04 is not a removal");
		memcpy(b, body_of(look), VMS_CM_BODY_LEN);
		b[9] = VMS_DLM_WIREOP_CONVERT;
		ct_check(vms_dlm_res_ident_parse_body(b, VMS_CM_BODY_LEN, &id)
			 == VMS_CODEC_E_CLASS, "a convert is not a lookup");
		memcpy(b, body_of(look), VMS_CM_BODY_LEN);
		b[47] = 32u;
		ct_check(vms_dlm_res_ident_parse_body(b, VMS_CM_BODY_LEN, &id)
			 == VMS_CODEC_E_RANGE, "a 32-byte name is refused");
	}
}

/* ==========================================================================
 * 2. OVMX's answer == the real directory's answer
 * ========================================================================== */

/* Byte-compare `built` (a frame) against the real answer's body, outside
 * body[0:8] and outside body[28:39] except the bytes `keep` names. */
static int answer_matches(const uint8_t *built, const struct vms_fixture *real,
			  int keep_csid)
{
	const uint8_t *rb = body_of(real);
	const uint8_t *bb = built + VMS_OFF_SYSAP_BODY;
	uint32_t i;

	for (i = 8u; i < VMS_CM_BODY_LEN; i++) {
		int stale = (i >= 28u && i < 39u && i != 34u &&
			     !(keep_csid && i < 32u));
		if (stale)
			continue;
		if (bb[i] != rb[i]) {
			printf("   body[%u]: built %02x, real %02x\n", i, bb[i], rb[i]);
			return 0;
		}
	}
	return 1;
}

static void test_answers_match_the_real_directory(void)
{
	const struct vms_fixture *q2 = fixture("dlm-dirhash-root-vax2");
	const struct vms_fixture *a2 = fixture("dlm-dirhash-answer-vax1");
	const struct vms_fixture *q3 = fixture("dlm-dirhash-root-vax3");
	const struct vms_fixture *a3 = fixture("dlm-dir-redirect-vax1");
	uint8_t built[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	uint32_t written = 0, i;
	int zero = 1;

	printf("-- OVMX's directory answers vs VAX1's real ones\n");
	ct_check(q2 && a2 && q3 && a3, "the four specimens load");
	if (!q2 || !a2 || !q3 || !a3)
		return;
	ct_check_eq_u32(body_of(a2)[34], VMS_DLM_DIR_YOU_MASTER,
			"the real answer to VAX2's first DLMTA lookup is 0xf9");
	ct_check_eq_u32(body_of(a3)[34], VMS_DLM_DIR_REDIRECT,
			"the real answer to VAX3's later one is 0xf8");

	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_dir_answer_build(body_of(q2), blen_of(q2),
					  VMS_DLM_DIR_YOU_MASTER, 0u, built,
					  sizeof(built), &written) == VMS_CODEC_OK &&
		 written == sizeof(built), "OVMX builds 'you master it'");
	ct_check(answer_matches(built, a2, 0),
		 "  *** byte-identical to VAX1's real 0xf9 answer ***");
	for (i = 28u; i < 39u; i++) {
		if (i != 34u && built[VMS_OFF_SYSAP_BODY + i] != 0u)
			zero = 0;
	}
	ct_check(zero, "  and the span VAX1 left stale is ZERO, not invented");

	ct_check(vms_dlm_dir_answer_build(body_of(q3), blen_of(q3),
					  VMS_DLM_DIR_REDIRECT, 0x00010002u,
					  built, sizeof(built), &written) ==
		 VMS_CODEC_OK, "OVMX builds 'the master is 00010002'");
	ct_check(answer_matches(built, a3, 1),
		 "  *** byte-identical to VAX1's real 0xf8 answer, CSID included ***");

	ct_check(vms_dlm_dir_answer_build(body_of(q3), blen_of(q3), 0xfa, 0u,
					  built, sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "no status but the two grounded ones");
	ct_check(vms_dlm_dir_answer_build(body_of(q3), blen_of(q3),
					  VMS_DLM_DIR_REDIRECT, 0u, built,
					  sizeof(built), &written) ==
		 VMS_CODEC_E_INVAL, "a redirect naming nobody is refused");
	{
		const struct vms_fixture *sub = fixture("dlm-dirhash-sub-vax2");
		const struct vms_fixture *rem = fixture("dlm-dir-remove-vax3");

		ct_check(sub && vms_dlm_dir_answer_build(body_of(sub),
				blen_of(sub), VMS_DLM_DIR_YOU_MASTER, 0u, built,
				sizeof(built), &written) == VMS_CODEC_E_CLASS,
			 "a sub-resource request is never answered as a lookup");
		ct_check(rem && vms_dlm_dir_answer_build(body_of(rem),
				blen_of(rem), VMS_DLM_DIR_YOU_MASTER, 0u, built,
				sizeof(built), &written) == VMS_CODEC_E_CLASS,
			 "a removal is never answered");
	}
}

/*
 * rd vms-629: the op-0x08 lookup a real VAX sends its directory node from
 * inside a barrier. Two independent real samples; OVMX's answer to each is the
 * real directory's, opcode 0x01 included.
 */
static void op08_pair(const char *req_name, const char *ans_name)
{
	const struct vms_fixture *q = fixture(req_name);
	const struct vms_fixture *a = fixture(ans_name);
	uint8_t built[VMS_OFF_SYSAP_BODY + VMS_CM_BODY_LEN];
	struct vms_dlm_res_ident id;
	uint32_t written = 0;

	ct_check(q && a, "both specimens load");
	if (!q || !a)
		return;
	ct_check(vms_dlm_res_ident_parse_body(body_of(q), blen_of(q), &id) ==
		 VMS_CODEC_OK && id.name_len == 16u &&
		 memcmp(id.name, "SYS$SYS_ID", 10) == 0,
		 "  the op-0x08 names a ROOT: SYS$SYS_ID + the sender's id");
	ct_check_eq_u32(body_of(a)[9], VMS_DLM_WIREOP_ENQ,
			"  the real answer's opcode byte is 0x01, not 0x08");
	ct_check_eq_u32(body_of(a)[34], VMS_DLM_DIR_YOU_MASTER,
			"  and it says 'you master it'");
	memset(built, 0xa5, sizeof(built));
	ct_check(vms_dlm_dir_answer_build(body_of(q), blen_of(q),
					  VMS_DLM_DIR_YOU_MASTER, 0u, built,
					  sizeof(built), &written) == VMS_CODEC_OK,
		 "  OVMX builds the directory's answer to it");
	ct_check(answer_matches(built, a, 0),
		 "  *** byte-identical to the real VAX's answer, opcode 0x01 "
		 "included ***");
}

static void test_op08_barrier_lookup(void)
{
	uint8_t sub[VMS_CM_BODY_LEN];
	const struct vms_fixture *q = fixture("dlm-op08-req-cf1");
	struct vms_dlm_res_ident id;

	printf("-- rd vms-629: a VAX's barrier lookup (op 0x08) gets the real "
	       "directory's answer\n");
	op08_pair("dlm-op08-req-coldform", "dlm-op08-answer-coldform");
	op08_pair("dlm-op08-req-cf1", "dlm-op08-answer-cf1");
	if (q == NULL)
		return;
	memcpy(sub, body_of(q), sizeof(sub));
	sub[36] = 0x01u;   /* a non-zero parent span: not a ROOT */
	ct_check(vms_dlm_res_ident_parse_body(sub, sizeof(sub), &id) ==
		 VMS_CODEC_E_CLASS,
		 "a sub-resource op 0x08 is not a directory lookup");
}

/* ==========================================================================
 * 3. The table
 * ========================================================================== */
static struct vms_dlm_dir_entry g_store[64];

static int keep_even(void *ctx, uint32_t hash, int hash_known)
{
	(void)ctx;
	(void)hash_known;
	return (hash & 1u) == 0u;
}

static void test_table_outcomes(void)
{
	struct vms_dlm_dir d;
	struct vms_dlm_res_ident a, a_mode1, a_grp1, b;
	vms_csid_t m = 0;

	printf("-- the table: p. 6-31 outcomes, removal, departure, vector\n");
	ct_check(vms_dlm_dir_init(&d, g_store, 64u) == 0, "init");
	ct_check(vms_dlm_dir_init(&d, g_store, 48u) != 0,
		 "a non-power-of-two capacity is refused");
	(void)vms_dlm_dir_init(&d, g_store, 64u);
	ident(&a, "DLMTA", 0u, 3u, 0x00336fe3u);
	ident(&a_mode1, "DLMTA", 0u, 1u, 0x00336fe3u);
	ident(&a_grp1, "DLMTA", 1u, 3u, 0x00336fe3u);
	ident(&b, "DLMTB", 0u, 3u, 0x7e66dde3u);

	ct_check(vms_dlm_dir_lookup(&d, &a, 0x00010002u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_YOU && m == 0x00010002u,
		 "no entry: the requester masters it, and it is RECORDED");
	ct_check(vms_dlm_dir_lookup(&d, &a, 0x00010003u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_REDIRECT && m == 0x00010002u,
		 "the next asker is redirected to that master");
	ct_check(vms_dlm_dir_lookup(&d, &a, 0x00010002u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_YOU,
		 "the master itself asking is told it masters it");
	ct_check(vms_dlm_dir_lookup(&d, &a_mode1, 0x00010003u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_YOU,
		 "the same name in another ACCESS MODE is another resource");
	ct_check(vms_dlm_dir_lookup(&d, &a_grp1, 0x00010003u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_YOU,
		 "the same name in another GROUP is another resource");

	ct_check(vms_dlm_dir_remove(&d, &a, 0x00010003u) != 0 &&
		 vms_dlm_dir_find(&d, &a) != NULL,
		 "a removal from a system that is not the master removes nothing");
	ct_check(vms_dlm_dir_remove(&d, &a, 0x00010002u) == 0 &&
		 vms_dlm_dir_find(&d, &a) == NULL, "the master's removal does");
	ct_check(vms_dlm_dir_lookup(&d, &a, 0x00010003u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_YOU && m == 0x00010003u,
		 "after removal the next asker becomes the master");

	ct_check(vms_dlm_dir_register(&d, &b, 0x00010001u) == 0 &&
		 vms_dlm_dir_find(&d, &b)->master == 0x00010001u,
		 "a master's registration records it");
	ct_check(vms_dlm_dir_register(&d, &b, 0x00010002u) == 0 &&
		 vms_dlm_dir_find(&d, &b)->master == 0x00010002u,
		 "a later registration (a remaster) overwrites it");
	ct_check(vms_dlm_dir_register(&d, &b, 0u) != 0,
		 "a registration naming no master is refused");

	ct_check_eq_u32(vms_dlm_dir_drop_master(&d, 0x00010003u), 3u,
			"a departure drops every entry its system mastered");
	ct_check(vms_dlm_dir_find(&d, &b) != NULL, "and no other");
	(void)vms_dlm_dir_register(&d, &a, 0x00010001u);
	ct_check_eq_u32(vms_dlm_dir_drop_unless(&d, keep_even, NULL), 2u,
			"a vector change drops what this node no longer directs");
	ct_check(vms_dlm_dir_find(&d, &a) == NULL &&
		 vms_dlm_dir_find(&d, &b) == NULL,
		 "  (both odd-hashed entries gone)");
}

static void test_table_full_is_refused(void)
{
	struct vms_dlm_dir d;
	struct vms_dlm_res_ident id;
	vms_csid_t m;
	uint32_t i, ok = 0;
	char nm[16];

	printf("-- a full table is refused, never answered\n");
	(void)vms_dlm_dir_init(&d, g_store, 16u);
	for (i = 0; i < 20u; i++) {
		snprintf(nm, sizeof(nm), "R%u", i);
		ident(&id, nm, 0u, 0u, i * 2654435761u);
		if (vms_dlm_dir_lookup(&d, &id, 0x00010002u, 0u, &m) ==
		    VMS_DLM_DIR_ANSWER_YOU)
			ok++;
	}
	ct_check_eq_u32(ok, 14u, "14 of 16 slots fill (one eighth stays free)");
	ct_check(vms_dlm_dir_lookup(&d, &id, 0x00010002u, 0u, &m) ==
		 VMS_DLM_DIR_ANSWER_NONE,
		 "the 15th new name gets NO answer -- not an unrecorded 0xf9");
	ct_check(d.full_refusals >= 1u, "and the refusal is counted");
}

/* ==========================================================================
 * 3b. THE TWO-MASTER HOLE, at the table (rd vms-025 / vms-db2a)
 *
 * The table is now keyed by the NAME, and that is not a refactor: it is what
 * makes the two questions below answerable at all. Each test here FAILS on the
 * hash-probed table this replaced, which is the negative control.
 * ========================================================================== */
#define SELF_CSID  0x00010004u
#define VAX_CSID   0x00010002u
#define VAX2_CSID  0x00010003u

static void test_name_lookup_finds_a_wire_recorded_master(void)
{
	struct vms_dlm_dir d;
	struct vms_dlm_res_ident a;
	vms_csid_t m = 0, master = 0;
	uint32_t hash = 0;
	uint8_t hash_known = 0;

	printf("-- rd vms-025: THIS NODE's own $ENQ can ask its own directory\n");
	(void)vms_dlm_dir_init(&d, g_store, 64u);
	ident(&a, "EVACWL", 0u, 3u, 0x1234abcdu);

	/* A real VAX looked the name up here and this directory answered "you
	 * master it" -- and RECORDED it (rd vms-8219). */
	ct_check(vms_dlm_dir_lookup(&d, &a, VAX_CSID, SELF_CSID, &m) ==
		 VMS_DLM_DIR_ANSWER_YOU && m == VAX_CSID,
		 "a VAX's lookup is answered 'you master it' and recorded");

	/* *** THE QUESTION THE ENGINE COULD NOT ASK *** -- by NAME, with no
	 * hash, because a local $ENQ holds a name and nothing else. */
	ct_check(vms_dlm_dir_lookup_name(&d, "EVACWL", 6u, &master, &hash,
					 &hash_known) ==
		 VMS_DLM_DIR_NAME_MASTER && master == VAX_CSID,
		 "*** the name lookup names the VAX as the master -- so a local "
		 "$ENQ cannot master it a second time ***");
	ct_check(hash_known == 1u && hash == 0x1234abcdu,
		 "  and it carries the WIRE hash off the VAX's own frame -- the "
		 "only value that may address a frame for this name");
	ct_check_eq_u32(d.name_lookups, 1u, "the ask is counted");

	/* A name nothing has been said about is NOT an answer. */
	ct_check(vms_dlm_dir_lookup_name(&d, "NOBODY", 6u, &master, NULL,
					 NULL) == VMS_DLM_DIR_NAME_NONE,
		 "a name no system has named here is reported as NO entry, "
		 "never as an unmastered 0");
}

static void test_self_claim_is_answered_as_master(void)
{
	struct vms_dlm_dir d;
	struct vms_dlm_res_ident a, a_other_domain;
	vms_csid_t m = 0, master = 0;
	uint8_t hash_known = 1u;

	printf("-- rd vms-db2a: a name THIS NODE masters is answered as master\n");
	(void)vms_dlm_dir_init(&d, g_store, 64u);

	/* This node mastered the name on first use and recorded the claim. It
	 * has no wire hash for it and must not invent one. */
	ct_check(vms_dlm_dir_claim_self(&d, "EVACWL", 6u, SELF_CSID) == 0,
		 "this node records its own mastery of a name it touched first");
	ct_check_eq_u32(d.self_claims, 1u, "counted");
	ct_check(vms_dlm_dir_lookup_name(&d, "EVACWL", 6u, &master, NULL,
					 &hash_known) ==
		 VMS_DLM_DIR_NAME_MASTER && master == SELF_CSID &&
		 hash_known == 0u,
		 "  the entry names THIS node and carries NO hash (honest "
		 "absence, never a zero dressed as a hash)");
	ct_check(vms_dlm_dir_claim_self(&d, "EVACWL", 6u, SELF_CSID) == 0 &&
		 d.self_claims == 1u, "  claiming it again changes nothing");

	/* *** THE HOLE, FROM THE OTHER SIDE *** -- a VAX now looks the name up.
	 * Its frame carries ITS group, ITS access mode and ITS hash, none of
	 * which this node holds; on the hash-probed table its lookup missed the
	 * claim entirely and was answered "you master it". */
	ident(&a, "EVACWL", 0u, 3u, 0x1234abcdu);
	ct_check(vms_dlm_dir_lookup(&d, &a, VAX_CSID, SELF_CSID, &m) ==
		 VMS_DLM_DIR_ANSWER_SELF && m == SELF_CSID,
		 "*** the VAX's lookup is answered THIS NODE MASTERS IT "
		 "(p. 6-51), not 'you master it' ***");
	ct_check_eq_u32(d.answered_you, 0u,
			"  nothing was answered 'you master it'");
	ct_check_eq_u32(d.answered_self, 1u, "  and the outcome is counted");

	/* A DIFFERENT resource domain of the same name reaches the same answer:
	 * the conflation is conservative and can only ever conclude that THIS
	 * node is the master -- never route one system at another. */
	ident(&a_other_domain, "EVACWL", 1u, 1u, 0x9999fefeu);
	ct_check(vms_dlm_dir_lookup(&d, &a_other_domain, VAX2_CSID, SELF_CSID,
				    &m) == VMS_DLM_DIR_ANSWER_SELF,
		 "another resource domain of that name is answered the same "
		 "way (over-serialize, never two masters)");

	/* With no identity of our own the outcome is unreachable rather than
	 * guessed: a node that does not know its CSID cannot claim mastery. */
	ct_check(vms_dlm_dir_lookup(&d, &a, VAX_CSID, 0u, &m) !=
		 VMS_DLM_DIR_ANSWER_SELF,
		 "a node that does not know its own CSID never answers SELF");
}

static void test_two_masters_for_one_name_are_refused(void)
{
	struct vms_dlm_dir d;
	struct vms_dlm_res_ident g0, g1;
	vms_csid_t m = 0, master = 0;

	printf("-- two systems mastering one name: refused, never chosen\n");
	(void)vms_dlm_dir_init(&d, g_store, 64u);
	ident(&g0, "QMAN$", 0u, 3u, 0x11110000u);
	ident(&g1, "QMAN$", 1u, 3u, 0x22220000u);
	(void)vms_dlm_dir_lookup(&d, &g0, VAX_CSID, SELF_CSID, &m);
	(void)vms_dlm_dir_lookup(&d, &g1, VAX2_CSID, SELF_CSID, &m);

	ct_check(vms_dlm_dir_lookup_name(&d, "QMAN$", 5u, &master, NULL,
					 NULL) == VMS_DLM_DIR_NAME_AMBIGUOUS,
		 "*** a local $ENQ on that name is REFUSED, not routed at one "
		 "of them on a coin toss ***");
	ct_check_eq_u32(d.name_ambiguous, 1u, "counted");
}

static int keep_nothing(void *ctx, uint32_t hash, int hash_known)
{
	(void)ctx;
	(void)hash;
	(void)hash_known;
	return 0;
}

static int keep_sole_directory(void *ctx, uint32_t hash, int hash_known)
{
	(void)hash;
	/* What dlm_arm_dir_is_ours does: an entry with no wire hash cannot be
	 * judged against a vector index, so it is kept exactly while this node
	 * is the sole directory node. */
	return hash_known ? 1 : (*(const int *)ctx);
}

static void test_a_hashless_claim_survives_only_the_sole_directory(void)
{
	struct vms_dlm_dir d;
	int sole;
	vms_csid_t master = 0;

	printf("-- a transition: a hashless self-claim is judged on the vector\n");
	(void)vms_dlm_dir_init(&d, g_store, 64u);
	(void)vms_dlm_dir_claim_self(&d, "EVACWL", 6u, SELF_CSID);
	sole = 1;
	ct_check_eq_u32(vms_dlm_dir_drop_unless(&d, keep_sole_directory, &sole),
			0u, "sole directory node: the claim stays");
	ct_check(vms_dlm_dir_lookup_name(&d, "EVACWL", 6u, &master, NULL,
					 NULL) == VMS_DLM_DIR_NAME_MASTER,
		 "  and is still answerable");
	sole = 0;
	ct_check_eq_u32(vms_dlm_dir_drop_unless(&d, keep_sole_directory, &sole),
			1u, "no longer the sole directory node: it is dropped "
			    "(p. 6-33), never kept on a stale vector");
	ct_check(vms_dlm_dir_lookup_name(&d, "EVACWL", 6u, &master, NULL,
					 NULL) == VMS_DLM_DIR_NAME_NONE,
		 "  and the directory then says nothing about the name");

	/* And a departure takes this node's own claims with it only when the
	 * CSID that left is this node's -- which it never is. */
	(void)vms_dlm_dir_claim_self(&d, "EVACWL", 6u, SELF_CSID);
	ct_check_eq_u32(vms_dlm_dir_drop_master(&d, VAX_CSID), 0u,
			"another system's departure does not drop our claim");
	ct_check_eq_u32(vms_dlm_dir_drop_unless(&d, keep_nothing, NULL), 1u,
			"a predicate that keeps nothing drops it");
}

/* A randomized model check: the table agrees with a flat reference after
 * every insert/remove, under heavy hash collisions (the back-shift path). */
static void test_table_model(void)
{
	struct vms_dlm_dir d;
	struct ref { int live; vms_csid_t m; } ref[48];
	struct vms_dlm_res_ident id;
	uint32_t step, k, bad = 0;
	char nm[16];

	printf("-- randomized model check (collision-heavy)\n");
	memset(ref, 0, sizeof(ref));
	(void)vms_dlm_dir_init(&d, g_store, 64u);
	srand(8219);
	for (step = 0; step < 20000u; step++) {
		k = (uint32_t)rand() % 48u;
		snprintf(nm, sizeof(nm), "K%u", k);
		ident(&id, nm, 0u, 0u, (k % 5u) * 64u + 7u); /* 5 home slots */
		if (rand() % 3) {
			vms_csid_t m = 0x00010000u | ((uint32_t)rand() % 4u + 1u);

			if (vms_dlm_dir_register(&d, &id, m) == 0) {
				ref[k].live = 1;
				ref[k].m = m;
			}
		} else if (ref[k].live) {
			if (vms_dlm_dir_remove(&d, &id, ref[k].m) == 0)
				ref[k].live = 0;
			else
				bad++;
		}
		for (k = 0; k < 48u; k++) {
			const struct vms_dlm_dir_entry *e;

			snprintf(nm, sizeof(nm), "K%u", k);
			ident(&id, nm, 0u, 0u, (k % 5u) * 64u + 7u);
			e = vms_dlm_dir_find(&d, &id);
			if ((e != NULL) != (ref[k].live != 0) ||
			    (e && e->master != ref[k].m))
				bad++;
		}
	}
	ct_check_eq_u32(bad, 0u, "20000 steps, the table never disagrees");
}

int main(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("test_dlm_dir: the lock directory's entries (rd vms-8219)\n");
	g_n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				   g_fx, VMS_FIXTURE_MAX_FILES, err, sizeof(err));
	if (g_n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		return 1;
	}
	test_identity_from_real_frames();
	test_answers_match_the_real_directory();
	test_op08_barrier_lookup();
	test_table_outcomes();
	test_table_full_is_refused();
	test_name_lookup_finds_a_wire_recorded_master();
	test_self_claim_is_answered_as_master();
	test_two_masters_for_one_name_are_refused();
	test_a_hashless_claim_survives_only_the_sole_directory();
	test_table_model();
	return ct_summary("test_dlm_dir");
}
