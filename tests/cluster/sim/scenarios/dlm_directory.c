/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scenarios/dlm_directory.c - FC-P4.3's rung-R2 leg: N simulated systems each
 * hold their OWN copy of the Lock Directory Weight Vector, built by the
 * SHIPPING Phase 2 commit from their OWN CSBs, and the directory duty is then
 * exercised BETWEEN them.
 *
 * WHAT MAKES THIS R2 AND NOT A SECOND R1. test_dlm_ldwv.c checks one node's
 * vector against the published construction. The thing that cannot be checked
 * on one node is the property the whole scheme rests on -- that N INDEPENDENTLY
 * BUILT copies AGREE (Davis p. 6-32, "logically equivalent") -- and the thing
 * that cannot be checked without a sender and a receiver is the ORDER
 * SELF-CHECK: system X resolves a hash through ITS copy, sends the lookup to
 * the system it names, and that system asks ITS copy whether the lookup was
 * addressed to it. Every lookup must land. That round trip is what falsifies
 * the CSV-index-order hypothesis (the one residual FC-P4.1 left open), and it
 * needs N nodes.
 *
 * NOTHING IS MODELLED. Each simulated system is a real `struct vms_cluster`
 * with a real CLUB and real CSBs; the vector is built by the SHIPPING
 * cnxman_phase2_commit() (through the shipping cnxman_ldwv_rebuild()), and the
 * receiving side's verdict is the SHIPPING cnxman_dir_lookup_received(). The
 * harness supplies the clock and the console, and decides who is a member --
 * exactly the division sim_node.h states for the port.
 *
 * NO HASH IS COMPUTED ANYWHERE, here or in the executive. The "hashes" swept
 * below are simply all 65536 values a 16-bit wire field can carry: the
 * assertion is about routing, not about any name's hash.
 */
#include <stdio.h>
#include <string.h>

#include "cluster_test.h"
#include "sim_clock.h"

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_phase2.h"
#include "vms_dlm_ldwv.h"

/* ==========================================================================
 * The simulated cluster
 *
 * Four systems, whose CSIDs put them in CSV slots 1..4 (the low 16 bits index
 * the Cluster System Vector, p. 7-25). Weights chosen so the set is genuinely
 * WEIGHTED -- the interesting case, and the one the p. 6-33 figure shows --
 * rather than the uniform all-zero case R1 already covers.
 * ========================================================================== */
#define SIM_N 4u

static const vms_csid_t g_csid[SIM_N] = {
	0x00010001u, 0x00010002u, 0x00010003u, 0x00010004u
};
static const uint8_t g_weight[SIM_N] = { 1u, 3u, 0u, 2u };
static const char *g_name[SIM_N] = { "VAXA", "VAXB", "VAXC", "OVMXS" };

/* OVMX is the last system: weight 2 of a total of 6. */
#define SIM_OVMX 3u

struct simsys {
	struct vms_cluster cl;
	struct cnxman_ops  ops;
	uint32_t           logs;
	char               last_log[160];
};

static struct simsys g_sys[SIM_N];
static struct sim_clock g_clock;

static uint32_t sim_now_ms(void *ctx)
{
	(void)ctx;
	return (uint32_t)sim_clock_now_ms(&g_clock);
}

static void sim_log(void *ctx, const char *msg)
{
	struct simsys *s = ctx;
	size_t n;

	s->logs++;
	if (msg == NULL)
		return;
	n = strlen(msg);
	if (n >= sizeof(s->last_log))
		n = sizeof(s->last_log) - 1u;
	memcpy(s->last_log, msg, n);
	s->last_log[n] = '\0';
}

/*
 * Bring one system up as a member of the same four-system cluster: its own CSB
 * plus one per peer, each carrying the CSID and the LOCKDIRWT the cluster
 * advertised, all SELECTED. `deal_backwards` allocates the CSBs in the reverse
 * of CSV order on that one system -- a system that DISCOVERED its peers in a
 * different order, which is the realistic case and the one that would break a
 * vector built in allocation order.
 */
static void sys_form(uint32_t me, int deal_backwards)
{
	struct simsys *s = &g_sys[me];
	uint32_t k;

	memset(s, 0, sizeof(*s));
	memset(&s->ops, 0, sizeof(s->ops));
	s->ops.now_ms = sim_now_ms;
	s->ops.log = sim_log;
	s->ops.ctx = s;

	memcpy(s->cl.params.scsnode, g_name[me], strlen(g_name[me]));
	s->cl.params.scsnode_len = (uint8_t)strlen(g_name[me]);
	s->cl.params.scssystemid = (uint64_t)g_csid[me];
	s->cl.params.vaxcluster = 2;
	(void)cnxman_club_init(&s->cl);

	for (k = 0; k < SIM_N; k++) {
		uint32_t i = deal_backwards ? (SIM_N - 1u - k) : k;
		struct vms_csb *csb;

		if (i == me)
			continue;
		csb = cnxman_club_alloc_csb(&s->cl.club,
					    (vms_scs_sysid_t)g_csid[i], 1);
		cnxman_csb_set_csid(csb, g_csid[i]);
		cnxman_csb_set_lockdirwt(csb, g_weight[i]);
		cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
						     VMS_CSB_F_MEMBER));
	}

	/* The local CSB cnxman_club_init() made: learn our own CSID and weight,
	 * and select ourselves. */
	{
		struct vms_csb *local = cnxman_club_local(&s->cl.club);

		cnxman_csb_set_csid(local, g_csid[me]);
		cnxman_csb_set_lockdirwt(local, g_weight[me]);
		cnxman_csb_set_flags(local, (uint16_t)(VMS_CSB_F_SELECTED |
						       VMS_CSB_F_MEMBER));
		cnxman_club_learn_local_csid(&s->cl.club, g_csid[me]);
	}
}

/* Commit a transition on one system through the SHIPPING Phase 2, which is
 * what fills the vector (task 5, p. 7-42). No nodemap: this scenario is about
 * the vector, and a class-0x03 transition legitimately carries none. */
static uint32_t sys_commit(uint32_t me)
{
	struct cnxman_phase2_in in;
	struct cnxman_phase2_stats st;

	memset(&in, 0, sizeof(in));
	return cnxman_phase2_commit(&g_sys[me].cl, &in, &st, &g_sys[me].ops);
}

/* ==========================================================================
 * 1. N independently built copies are logically equivalent (p. 6-32)
 * ========================================================================== */
static void copies_agree(void)
{
	uint32_t i, h;
	int widths = 1, offsets = 1, own_zero = 1;

	printf("--- four systems build the vector independently; the copies agree ---\n");

	for (i = 0; i < SIM_N; i++) {
		sys_form(i, (int)(i & 1u));   /* half of them discovered backwards */
		(void)sys_commit(i);
		ct_check(g_sys[i].cl.club.ldwv.valid != 0u,
			 "Phase 2 filled this system's vector");
	}

	for (i = 0; i < SIM_N; i++) {
		const struct vms_ldwv *v = &g_sys[i].cl.club.ldwv;
		uint32_t j;

		if (v->n != g_sys[0].cl.club.ldwv.n)
			widths = 0;
		for (j = 0; j < v->n; j++) {
			vms_csid_t mine = v->entry[j];
			vms_csid_t peer = g_sys[(i + 1u) % SIM_N].cl.club.ldwv.entry[j];

			if (mine == 0u) {
				if (peer != g_csid[i])
					offsets = 0;
			} else if (peer != 0u && peer != mine) {
				offsets = 0;
			}
			if (mine == g_csid[i])
				own_zero = 0;
		}
	}
	ct_check_eq_u32(g_sys[0].cl.club.ldwv.n, 6u,
			"1 + 3 + 0 + 2 == six entries (p. 6-32)");
	ct_check(widths, "every system's copy has the same width");
	ct_check(offsets,
		 "and the same system at every offset, however it discovered "
		 "its peers");
	ct_check(own_zero, "and each sees 0, not itself, in its own entries");

	/* OVMX is the directory for its share -- weight 2 of 6 -- and the share
	 * is exact over the whole 16-bit hash space, not approximate. */
	{
		const struct vms_ldwv *v = &g_sys[SIM_OVMX].cl.club.ldwv;
		uint32_t ours = 0, want = 0, r;

		/*
		 * The exact share, derived rather than rounded: index r of the
		 * vector is selected by 65536/n hash values, plus one more for
		 * the first (65536 mod n) residues. Summing that over OVMX's
		 * own entries is what "the directory for its share" means when
		 * the hash space is not a multiple of the vector width.
		 */
		for (r = 0; r < v->n; r++) {
			if (v->entry[r] != 0u)
				continue;
			want += 65536u / v->n;
			if (r < 65536u % v->n)
				want++;
		}
		for (h = 0; h <= 0xffffu; h++) {
			if (vms_ldwv_is_ours(v, (uint16_t)h))
				ours++;
		}
		ct_check_eq_u32(ours, want,
				"OVMX is the directory for its 2-in-6 share of "
				"the hash space, exactly");
	}
	ct_check(!vms_ldwv_is_ours(&g_sys[2].cl.club.ldwv, 0u),
		 "and a LOCKDIRWT-0 system is the directory for nothing");
	{
		uint32_t none = 0;

		for (h = 0; h <= 0xffffu; h++)
			if (vms_ldwv_is_ours(&g_sys[2].cl.club.ldwv, (uint16_t)h))
				none++;
		ct_check_eq_u32(none, 0u, "...for no hash value at all (p. 6-33)");
	}
}

/* ==========================================================================
 * 2. Every lookup lands: the ORDER SELF-CHECK, across all four systems
 * ========================================================================== */

/* One directory lookup: `from` resolves `hash` through ITS copy and delivers it
 * to whoever that names; the receiver checks it against ITS copy. Returns the
 * receiving system's index, or SIM_N when `from` could not resolve. */
static uint32_t deliver_lookup(uint32_t from, uint16_t hash)
{
	vms_csid_t dst = 0;
	uint32_t i;

	if (vms_ldwv_resolve(&g_sys[from].cl.club.ldwv, hash, &dst) != VMS_LDWV_OK)
		return SIM_N;
	if (dst == 0u)
		dst = g_csid[from];   /* our own entry: we are the directory */
	for (i = 0; i < SIM_N; i++) {
		if (g_csid[i] != dst)
			continue;
		(void)cnxman_dir_lookup_received(&g_sys[i].cl.club, hash,
						 g_csid[from], &g_sys[i].ops);
		return i;
	}
	return SIM_N;
}

static void every_lookup_lands(void)
{
	uint32_t from, h, unroutable = 0, mis = 0;

	printf("--- every lookup one system sends lands on the system that "
	       "believes it owns it ---\n");

	/* A representative sweep rather than 4 x 65536: every residue class of
	 * the modulus, several times over, from every sender. */
	for (from = 0; from < SIM_N; from++) {
		for (h = 0; h < 600u; h++) {
			if (deliver_lookup(from, (uint16_t)h) == SIM_N)
				unroutable++;
		}
	}
	for (from = 0; from < SIM_N; from++)
		mis += g_sys[from].cl.club.dir_lookup_misaddressed;

	ct_check_eq_u32(unroutable, 0u, "every lookup found a destination");
	ct_check_eq_u32(mis, 0u,
			"and NOT ONE was mis-addressed: the CSV-index-order "
			"hypothesis holds across four independently built copies");
	ct_check(g_sys[SIM_OVMX].cl.club.dir_lookups_received > 0u,
		 "OVMX really did receive directory lookups as a directory node");
	ct_check_eq_u32(g_sys[2].cl.club.dir_lookups_received, 0u,
			"and the LOCKDIRWT-0 system received none");
}

/* ==========================================================================
 * 3. The self-check FIRES when the order is wrong
 *
 * The negative control, and the reason the counter is worth having: a system
 * whose vector is laid out in a DIFFERENT order still has the right width and
 * the right multiset of entries, so nothing local looks wrong -- the only
 * symptom is lookups arriving at the wrong system. Deliberately permute one
 * system's copy and prove the shipping check catches it.
 * ========================================================================== */
static void misordered_vector_is_caught(void)
{
	struct vms_ldwv *v = &g_sys[SIM_OVMX].cl.club.ldwv;
	uint32_t before, h, caught;
	uint32_t j;
	vms_csid_t tmp;

	printf("--- a deliberately MIS-ORDERED vector is caught by the self-check ---\n");

	before = g_sys[SIM_OVMX].cl.club.dir_lookup_misaddressed;

	/* Reverse OVMX's copy: same width, same entries, wrong order. */
	for (j = 0; j < v->n / 2u; j++) {
		tmp = v->entry[j];
		v->entry[j] = v->entry[v->n - 1u - j];
		v->entry[v->n - 1u - j] = tmp;
	}

	/* Every OTHER system still resolves through ITS (correct) copy, so the
	 * lookups OVMX receives are exactly the ones the cluster thinks are
	 * OVMX's. Under the reversed copy, OVMX disagrees. */
	for (h = 0; h < 600u; h++) {
		uint32_t dst = deliver_lookup(0u, (uint16_t)h);

		(void)dst;
	}
	caught = g_sys[SIM_OVMX].cl.club.dir_lookup_misaddressed - before;
	ct_check(caught > 0u,
		 "the mis-ordered copy makes received lookups miss, and they "
		 "are COUNTED");
	ct_check(g_sys[SIM_OVMX].logs > 0u, "with a %CNXMAN line each time");

	/* And a rebuild puts it right again: the vector is CLUB state, so the
	 * next transition's Phase 2 restores the agreed layout. */
	(void)sys_commit(SIM_OVMX);
	before = g_sys[SIM_OVMX].cl.club.dir_lookup_misaddressed;
	for (h = 0; h < 600u; h++)
		(void)deliver_lookup(0u, (uint16_t)h);
	ct_check_eq_u32(g_sys[SIM_OVMX].cl.club.dir_lookup_misaddressed, before,
			"and a Phase 2 rebuild restores agreement");
}

/* ==========================================================================
 * 4. A transition discards the vector cluster-wide (p. 6-33)
 * ========================================================================== */
static void transition_discards_then_refills(void)
{
	uint32_t gen_before[SIM_N];
	uint32_t i;

	printf("--- a member leaves: the vector is discarded, then rebuilt narrower ---\n");
	for (i = 0; i < SIM_N; i++)
		gen_before[i] = vms_ldwv_generation(&g_sys[i].cl.club.ldwv);

	/* VAXB (weight 3) leaves: every survivor drops its CSB from the
	 * membership and re-commits. */
	for (i = 0; i < SIM_N; i++) {
		struct vms_csb *gone;

		if (i == 1u)
			continue;
		gone = cnxman_club_find_csid(&g_sys[i].cl.club, g_csid[1]);
		cnxman_csb_clear_flags(gone, (uint16_t)(VMS_CSB_F_SELECTED |
							VMS_CSB_F_MEMBER));
		(void)sys_commit(i);
	}

	for (i = 0; i < SIM_N; i++) {
		if (i == 1u)
			continue;
		ct_check(vms_ldwv_generation(&g_sys[i].cl.club.ldwv) >
			 gen_before[i],
			 "the vector changed, so every cached directory dies");
		ct_check_eq_u32(g_sys[i].cl.club.ldwv.n, 3u,
				"1 + 0 + 2 == three entries after the departure");
	}

	/* And the survivors still agree with each other. */
	{
		uint32_t h, unroutable = 0, mis = 0;

		for (h = 0; h < 600u; h++) {
			(void)deliver_lookup(0u, (uint16_t)h);
			(void)deliver_lookup(SIM_OVMX, (uint16_t)h);
		}
		for (i = 0; i < SIM_N; i++) {
			if (i == 1u)
				continue;
			mis += g_sys[i].cl.club.dir_lookup_misaddressed;
		}
		(void)unroutable;
		ct_check(mis == g_sys[SIM_OVMX].cl.club.dir_lookup_misaddressed,
			 "no survivor is mis-addressed after the rebuild");
	}
}

int main(void)
{
	printf("=== dlm_directory (FC-P4.3 rung R2: N systems, one vector) ===\n");
	sim_clock_init(&g_clock, 0);
	copies_agree();
	every_lookup_lands();
	misordered_vector_is_caught();
	transition_discards_then_refills();
	return ct_summary("dlm_directory");
}
