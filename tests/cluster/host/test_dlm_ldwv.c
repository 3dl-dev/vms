/* SPDX-License-Identifier: GPL-2.0 */
/*
 * test_dlm_ldwv.c - FC-P4.3's R1: the Lock Directory Weight Vector.
 *
 * Every assertion below is against a PUBLISHED statement, cited at the check,
 * from Roy G. Davis, *VAXcluster Principles* (Digital Press, 1993) ch. 6/7 as
 * transcribed in docs/research-dlm-directory-algorithm.md (FC-P4.1). Nothing
 * here asserts a hash VALUE: the hash function is not published and OVMX never
 * computes one, so a hash in this file is an arbitrary number standing in for
 * "whatever the cluster put on the wire" -- the only property under test is
 * `hash mod n` and what the vector holds at that index.
 *
 * THE FIVE THINGS PROVED HERE
 *   1. p. 6-33's worked example, byte for byte: VAX_A weight 1, VAX_B weight 3,
 *      two 0-weight systems -> a FOUR-entry vector [A, B, B, B].
 *   2. p. 6-32's all-zero rule: LOCKDIRWT 0 on every member -> exactly one
 *      entry per system.
 *   3. p. 6-32/Fig. 6-18: a system's OWN entries read 0 in its own copy, and
 *      every other copy is otherwise IDENTICAL -- the "logically equivalent"
 *      property, checked by building N copies and comparing them offset by
 *      offset.
 *   4. p. 6-31's index rule: entry[hash mod n], with 0 meaning "this node".
 *   5. p. 6-33's discard: any change to the vector bumps the generation and
 *      makes it unresolvable until it is refilled.
 * ...plus the two refusals FC-P4.3 owes INV-6 (a member set this node cannot
 * weigh, and one that will not fit) and the CSV-index-order build over real
 * CSBs with its self-check.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "cnxman_fake_ops.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_dlm_ldwv.h"

/* ==========================================================================
 * Fixture: one system's description, as the connection manager would hold it
 * ========================================================================== */
struct sysdesc {
	vms_csid_t csid;
	uint8_t    lockdirwt;
	uint8_t    lockdirwt_valid;
	const char *name;
};

/* p. 6-33's figure names VAX_A before VAX_B; their CSIDs put them in that same
 * CSV-index order (the low 16 bits index the CSV, p. 7-25). */
#define CSID_A 0x00010001u
#define CSID_B 0x00010002u
#define CSID_C 0x00010003u
#define CSID_D 0x00010004u

static void member_from(struct vms_ldwv_member *m, const struct sysdesc *d,
			int is_local)
{
	memset(m, 0, sizeof(*m));
	m->csid = d->csid;
	m->lockdirwt = d->lockdirwt;
	m->lockdirwt_valid = d->lockdirwt_valid;
	m->is_local = (uint8_t)(is_local ? 1 : 0);
}

/* Build ONE system's copy of the vector from the same ordered member set. */
static enum vms_ldwv_status build_copy(struct vms_ldwv *v,
				       const struct sysdesc *d, uint32_t n,
				       uint32_t local_index)
{
	struct vms_ldwv_member m[8];
	uint32_t i;

	for (i = 0; i < n && i < 8u; i++)
		member_from(&m[i], &d[i], i == local_index);
	vms_ldwv_init(v);
	return vms_ldwv_build(v, m, n);
}

/* ==========================================================================
 * 1. The published worked example (p. 6-32/6-33)
 * ========================================================================== */
static void worked_example_p6_33(void)
{
	/* "VAX_A weight 1, VAX_B weight 3, others 0 -> a 4-entry vector
	 * [A, B, B, B]; VAX_B is directory for ~3x as many trees as VAX_A; the
	 * 0-weight systems are directory for none." (p. 6-32, Fig. 6-18) */
	static const struct sysdesc set[4] = {
		{ CSID_A, 1, 1, "VAX_A" },
		{ CSID_B, 3, 1, "VAX_B" },
		{ CSID_C, 0, 1, "VAX_C" },
		{ CSID_D, 0, 1, "VAX_D" },
	};
	struct vms_ldwv v;
	uint32_t i, a_entries = 0, b_entries = 0;
	int all_zero = 1;
	struct vms_ldwv_member m[4];

	printf("--- p. 6-33 worked example: weights 1 and 3 over four systems ---\n");

	for (i = 0; i < 4u; i++)
		member_from(&m[i], &set[i], 0);
	ct_check_eq_u32(vms_ldwv_entry_count(m, 4u, &all_zero), 4u,
			"entry count is the sum of the LOCKDIRWTs");
	ct_check(all_zero == 0, "the all-zero rule does NOT apply here");

	/* A THIRD system's copy: neither A nor B is local, so no entry reads 0
	 * and the whole published figure is visible at once. */
	ct_check_eq_u32((unsigned long)build_copy(&v, set, 4u, 2u),
			(unsigned long)VMS_LDWV_OK, "the vector builds");
	ct_check_eq_u32(v.n, 4u, "four entries (p. 6-33)");
	ct_check(v.entry[0] == CSID_A, "entry 0 is VAX_A");
	ct_check(v.entry[1] == CSID_B && v.entry[2] == CSID_B &&
		 v.entry[3] == CSID_B,
		 "entries 1..3 are VAX_B, CONTIGUOUS (p. 6-32)");

	for (i = 0; i < v.n; i++) {
		if (v.entry[i] == CSID_A)
			a_entries++;
		if (v.entry[i] == CSID_B)
			b_entries++;
		ct_check(v.entry[i] != CSID_C && v.entry[i] != CSID_D,
			 "a 0-weight system is directory for no name");
	}
	ct_check(b_entries == 3u * a_entries,
		 "VAX_B is directory for 3x as many trees as VAX_A");
}

/* ==========================================================================
 * 2. The all-zero rule (p. 6-32) -- and the not-yet-advertised reading
 * ========================================================================== */
static void all_zero_weights(void)
{
	static const struct sysdesc learned0[3] = {
		{ CSID_A, 0, 1, "A" }, { CSID_B, 0, 1, "B" }, { CSID_C, 0, 1, "C" },
	};
	static const struct sysdesc unknown[3] = {
		{ CSID_A, 0, 0, "A" }, { CSID_B, 0, 0, "B" }, { CSID_C, 0, 0, "C" },
	};
	struct vms_ldwv v;

	printf("--- p. 6-32: LOCKDIRWT 0 everywhere -> one entry per system ---\n");

	ct_check_eq_u32((unsigned long)build_copy(&v, learned0, 3u, 3u),
			(unsigned long)VMS_LDWV_OK, "builds with every weight 0");
	ct_check_eq_u32(v.n, 3u, "exactly one entry per system");
	ct_check(v.entry[0] == CSID_A && v.entry[1] == CSID_B &&
		 v.entry[2] == CSID_C, "one each, in order");
	ct_check_eq_u32(v.weights_learned, 1u,
			"and it is recorded that the weights were LEARNED");

	/*
	 * FC-P4.1 SS1/SS4.1: until FC-P3.2 pins the LOCKDIRWT wire field, NO
	 * member has advertised one, and the honest reading of a member set in
	 * which nobody has is the all-zero one. It is built, and the fact that
	 * it rests on that reading is RECORDED rather than invisible.
	 */
	ct_check_eq_u32((unsigned long)build_copy(&v, unknown, 3u, 3u),
			(unsigned long)VMS_LDWV_OK,
			"nobody has advertised a LOCKDIRWT -> the all-zero reading");
	ct_check_eq_u32(v.n, 3u, "one entry per system");
	ct_check_eq_u32(v.weights_learned, 0u,
			"and the vector says it rests on the unadvertised reading");
}

/* ==========================================================================
 * 3. Logically equivalent copies; own entries read 0 (p. 6-32, Fig. 6-18)
 * ========================================================================== */
static void copies_are_logically_equivalent(void)
{
	static const struct sysdesc set[4] = {
		{ CSID_A, 1, 1, "VAX_A" },
		{ CSID_B, 3, 1, "VAX_B" },
		{ CSID_C, 0, 1, "VAX_C" },
		{ CSID_D, 0, 1, "VAX_D" },
	};
	struct vms_ldwv copy[4];
	uint32_t sys, i;
	int widths_agree = 1, offsets_agree = 1, own_are_zero = 1;

	printf("--- p. 6-32: every copy is logically equivalent; OWN entries read 0 ---\n");

	for (sys = 0; sys < 4u; sys++)
		ct_check_eq_u32((unsigned long)build_copy(&copy[sys], set, 4u, sys),
				(unsigned long)VMS_LDWV_OK, "a system's copy builds");

	for (sys = 0; sys < 4u; sys++) {
		if (copy[sys].n != copy[0].n)
			widths_agree = 0;
		for (i = 0; i < copy[sys].n; i++) {
			vms_csid_t mine = copy[sys].entry[i];
			vms_csid_t theirs = copy[(sys + 1u) % 4u].entry[i];

			if (mine == 0u) {
				if (theirs != set[sys].csid)
					offsets_agree = 0;   /* our own slot */
			} else if (theirs != 0u && theirs != mine) {
				offsets_agree = 0;
			}
			if (mine == set[sys].csid)
				own_are_zero = 0;
		}
	}
	ct_check(widths_agree, "every copy has the same number of entries");
	ct_check(offsets_agree,
		 "and the same system at each offset (differing only in the 0s)");
	ct_check(own_are_zero,
		 "a system NEVER sees its own CSID in its own copy -- it sees 0");

	/* And 0 is read as "this node", which is what makes the directory-node
	 * admission test a comparison against 0 and not against a CSID this node
	 * may not have learned yet. */
	ct_check(vms_ldwv_is_ours(&copy[1], 1u),
		 "VAX_B's copy says VAX_B is the directory for index 1");
	ct_check(!vms_ldwv_is_ours(&copy[0], 1u),
		 "VAX_A's copy says it is not");
}

/* ==========================================================================
 * 4. The index rule (p. 6-31): entry[hash mod n]
 * ========================================================================== */
static void index_rule(void)
{
	static const struct sysdesc set[4] = {
		{ CSID_A, 1, 1, "VAX_A" },
		{ CSID_B, 3, 1, "VAX_B" },
		{ CSID_C, 0, 1, "VAX_C" },
		{ CSID_D, 0, 1, "VAX_D" },
	};
	struct vms_ldwv v;
	uint32_t idx = 99u;
	vms_csid_t csid = 0xffffffffu;
	uint32_t h, a_share = 0, b_share = 0;
	int rule_holds = 1;

	printf("--- p. 6-31: the directory node is entry[hash mod n] ---\n");
	(void)build_copy(&v, set, 4u, 2u);   /* VAX_C's copy: A and B both visible */

	ct_check_eq_u32((unsigned long)vms_ldwv_index(&v, 0u, &idx),
			(unsigned long)VMS_LDWV_OK, "hash 0 indexes");
	ct_check_eq_u32(idx, 0u, "0 mod 4 == 0");
	ct_check_eq_u32((unsigned long)vms_ldwv_index(&v, 4097u, &idx),
			(unsigned long)VMS_LDWV_OK, "4097 indexes");
	ct_check_eq_u32(idx, 1u, "4097 mod 4 == 1");
	ct_check_eq_u32((unsigned long)vms_ldwv_index(&v, 65535u, &idx),
			(unsigned long)VMS_LDWV_OK, "65535 indexes");
	ct_check_eq_u32(idx, 3u, "65535 mod 4 == 3");

	ct_check_eq_u32((unsigned long)vms_ldwv_resolve(&v, 0u, &csid),
			(unsigned long)VMS_LDWV_OK, "hash 0 resolves");
	ct_check(csid == CSID_A, "...to VAX_A, the system at entry 0");

	/* Sweep the WHOLE 16-bit hash space: the rule must hold for every value
	 * the wire can carry, and the weighted share must come out at the ratio
	 * the book predicts (3:1). */
	for (h = 0; h <= 0xffffu; h++) {
		vms_csid_t got = 0;

		if (vms_ldwv_resolve(&v, (uint16_t)h, &got) != VMS_LDWV_OK ||
		    got != v.entry[h % v.n])
			rule_holds = 0;
		if (got == CSID_A)
			a_share++;
		if (got == CSID_B)
			b_share++;
	}
	ct_check(rule_holds, "entry[hash mod n] holds for all 65536 hash values");
	ct_check(a_share == 16384u && b_share == 49152u,
		 "and the weighted share is 1:3 over the whole hash space");
}

/* ==========================================================================
 * 5. p. 6-33's discard, and the refusals
 * ========================================================================== */
static void discard_and_refusals(void)
{
	static const struct sysdesc set[2] = {
		{ CSID_A, 1, 1, "A" }, { CSID_B, 1, 1, "B" },
	};
	static const struct sysdesc mixed[2] = {
		{ CSID_A, 2, 1, "A" }, { CSID_B, 0, 0, "B" },
	};
	struct sysdesc big[2];
	struct vms_ldwv v;
	vms_csid_t csid = 0;
	uint32_t gen0, gen1;

	printf("--- p. 6-33: a change discards the vector; and the two refusals ---\n");

	(void)build_copy(&v, set, 2u, 1u);
	gen0 = vms_ldwv_generation(&v);
	ct_check(v.valid != 0u, "a built vector is authoritative");
	ct_check_eq_u32((unsigned long)vms_ldwv_resolve(&v, 7u, &csid),
			(unsigned long)VMS_LDWV_OK, "and resolves");

	vms_ldwv_invalidate(&v);
	gen1 = vms_ldwv_generation(&v);
	ct_check(gen1 != gen0, "Phase 1 bumps the generation (the cache key)");
	ct_check_eq_u32((unsigned long)vms_ldwv_resolve(&v, 7u, &csid),
			(unsigned long)VMS_LDWV_E_NOVEC,
			"and an invalidated vector resolves NOTHING (p. 6-33)");

	/* A rebuild is another change, even to the same contents. */
	(void)build_copy(&v, set, 2u, 1u);
	ct_check(vms_ldwv_generation(&v) != gen1,
		 "and the refill is itself a change");

	/* INV-6 refusal 1: some members have advertised a LOCKDIRWT, others have
	 * not. Laying that out would put every entry after the unweighable
	 * member at an offset no other copy agrees with. */
	ct_check_eq_u32((unsigned long)build_copy(&v, mixed, 2u, 1u),
			(unsigned long)VMS_LDWV_E_WEIGHTS,
			"a mixed learned/unlearned weight set is REFUSED");
	ct_check(v.valid == 0u, "and leaves NO vector, not a partial one");

	/* INV-6 refusal 2: it does not fit. Truncating would silently change the
	 * modulus, i.e. the directory node for most names. */
	big[0].csid = CSID_A; big[0].lockdirwt = 255; big[0].lockdirwt_valid = 1;
	big[1].csid = CSID_B; big[1].lockdirwt = 255; big[1].lockdirwt_valid = 1;
	{
		struct vms_ldwv_member m[2];
		uint32_t i;

		for (i = 0; i < 2u; i++)
			member_from(&m[i], &big[i], 0);
		/* 510 <= 512 fits... */
		vms_ldwv_init(&v);
		ct_check_eq_u32((unsigned long)vms_ldwv_build(&v, m, 2u),
				(unsigned long)VMS_LDWV_OK,
				"510 entries fit the storage bound");
		/* ...and one more system does not. */
		{
			struct vms_ldwv_member m3[3];

			m3[0] = m[0]; m3[1] = m[1];
			memset(&m3[2], 0, sizeof(m3[2]));
			m3[2].csid = CSID_C;
			m3[2].lockdirwt = 10;
			m3[2].lockdirwt_valid = 1;
			vms_ldwv_init(&v);
			ct_check_eq_u32((unsigned long)vms_ldwv_build(&v, m3, 3u),
					(unsigned long)VMS_LDWV_E_TOOBIG,
					"a set that does not fit is REFUSED, not truncated");
			ct_check(v.valid == 0u && v.n == 0u,
				 "and leaves no vector behind");
		}
	}

	vms_ldwv_init(&v);
	ct_check_eq_u32((unsigned long)vms_ldwv_build(&v, NULL, 0u),
			(unsigned long)VMS_LDWV_E_INVAL, "a null member set is refused");
}

/* ==========================================================================
 * 6. The CLUB build: CSV-index order over real CSBs, and the self-check
 * ========================================================================== */

static struct vms_csb *add_member(struct vms_cluster *cl, vms_csid_t csid,
				  uint8_t weight, int weight_valid, int local)
{
	struct vms_csb *csb = cnxman_club_alloc_csb(&cl->club,
						    (vms_scs_sysid_t)csid, 1);

	if (csb == NULL)
		return NULL;
	cnxman_csb_set_csid(csb, csid);
	if (weight_valid)
		cnxman_csb_set_lockdirwt(csb, weight);
	cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED | VMS_CSB_F_MEMBER));
	if (local)
		cnxman_csb_set_flags(csb, VMS_CSB_F_LOCAL);
	return csb;
}

static void club_build_csv_order(void)
{
	struct vms_cluster cl;
	struct cnxman_ops ops;
	struct fake_cnx f;
	struct vms_club *club;

	printf("--- the CLUB's own vector: CSV-index order over real CSBs ---\n");
	memset(&cl, 0, sizeof(cl));
	fake_ops_init(&ops, &f);
	(void)cnxman_club_init(&cl);
	club = &cl.club;

	/*
	 * DELIBERATELY OUT OF SLOT ORDER. CSV-index order is ascending
	 * (CSID & 0xffff) -- the CSV slot the low half of a CSID names
	 * (p. 7-25) -- NOT the order this node happened to allocate CSBs in.
	 * Adding them backwards is what makes the difference visible.
	 */
	ct_check(add_member(&cl, CSID_C, 1, 1, 0) != NULL, "CSB for slot 3");
	ct_check(add_member(&cl, CSID_A, 1, 1, 0) != NULL, "CSB for slot 1");
	ct_check(add_member(&cl, CSID_B, 2, 1, 1) != NULL, "CSB for slot 2 (local)");

	ct_check_eq_u32((unsigned long)cnxman_ldwv_rebuild(club, &ops),
			(unsigned long)VMS_LDWV_OK, "the CLUB's vector builds");
	ct_check_eq_u32(club->ldwv.n, 4u, "1 + 2 + 1 entries");
	ct_check(club->ldwv.entry[0] == CSID_A,
		 "entry 0 is CSV slot 1, not the first CSB allocated");
	ct_check(club->ldwv.entry[1] == 0u && club->ldwv.entry[2] == 0u,
		 "the LOCAL system's two entries read 0 in its own copy");
	ct_check(club->ldwv.entry[3] == CSID_C, "entry 3 is CSV slot 3");

	/*
	 * THE ORDER SELF-CHECK. A lookup that lands on one of OUR entries is
	 * correctly addressed; one that does not falsifies either the order
	 * hypothesis or the freshness of this vector, and is COUNTED.
	 */
	ct_check(cnxman_dir_lookup_received(club, 1u, CSID_A, &ops) == 1,
		 "a lookup whose hash lands on our entry is accepted");
	ct_check_eq_u32(club->dir_lookup_misaddressed, 0u, "and counts nothing");
	ct_check(cnxman_dir_lookup_received(club, 0u, CSID_A, &ops) == 0,
		 "a lookup whose hash lands on ANOTHER system's entry is not ours");
	ct_check_eq_u32(club->dir_lookup_misaddressed, 1u,
			"and is COUNTED, never silently served");
	ct_check_eq_u32(club->dir_lookups_received, 2u, "both were counted as received");
	ct_check(f.logs > 0u, "and the mis-addressed one produced a %CNXMAN line");

	/* A member that is not SELECTED is not in the vector: membership is the
	 * CSB flag the commit set, not "a CSB exists". */
	cnxman_csb_clear_flags(cnxman_club_find_csid(club, CSID_C),
			       VMS_CSB_F_SELECTED);
	ct_check_eq_u32((unsigned long)cnxman_ldwv_rebuild(club, &ops),
			(unsigned long)VMS_LDWV_OK, "rebuild after a removal");
	ct_check_eq_u32(club->ldwv.n, 3u, "the departed system's entry is gone");

	/* A CSB whose CSID this node has not LEARNED has no CSV slot and so no
	 * place in the vector -- inventing one would be an invented identity. */
	{
		struct vms_csb *unknown =
			cnxman_club_alloc_csb(club, (vms_scs_sysid_t)0x999u, 1);

		ct_check(unknown != NULL, "a CSB for a system whose CSID is unknown");
		cnxman_csb_set_flags(unknown, VMS_CSB_F_SELECTED);
		cnxman_csb_set_lockdirwt(unknown, 4);
		ct_check_eq_u32((unsigned long)cnxman_ldwv_rebuild(club, &ops),
				(unsigned long)VMS_LDWV_OK, "rebuild ignores it");
		ct_check_eq_u32(club->ldwv.n, 3u,
				"an unlearned CSID takes no entry (INV-6)");
	}
}

/* A CLUB whose members cannot all be weighed produces NO vector, and says so. */
static void club_build_refusal(void)
{
	struct vms_cluster cl;
	struct cnxman_ops ops;
	struct fake_cnx f;

	printf("--- a CLUB whose weights disagree in kind gets NO vector ---\n");
	memset(&cl, 0, sizeof(cl));
	fake_ops_init(&ops, &f);
	(void)cnxman_club_init(&cl);

	ct_check(add_member(&cl, CSID_A, 3, 1, 1) != NULL, "A advertised LOCKDIRWT 3");
	ct_check(add_member(&cl, CSID_B, 0, 0, 0) != NULL, "B has advertised none");

	ct_check_eq_u32((unsigned long)cnxman_ldwv_rebuild(&cl.club, &ops),
			(unsigned long)VMS_LDWV_E_WEIGHTS, "the rebuild is refused");
	ct_check_eq_u32(cl.club.ldwv_build_refused, 1u, "and counted in the CLUB");
	ct_check(cl.club.ldwv.valid == 0u, "no vector is left behind");
	ct_check(f.logs > 0u, "and one %CNXMAN line says why");
	ct_check(cnxman_dir_lookup_received(&cl.club, 12u, CSID_B, &ops) == 0,
		 "with no vector, no received lookup is ours");
}

int main(void)
{
	printf("=== test_dlm_ldwv (FC-P4.3 lock directory weight vector, R1) ===\n");
	worked_example_p6_33();
	all_zero_weights();
	copies_are_logically_equivalent();
	index_rule();
	discard_and_refusals();
	club_build_csv_order();
	club_build_refusal();
	return ct_summary("test_dlm_ldwv");
}
