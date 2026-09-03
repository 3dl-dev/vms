// SPDX-License-Identifier: GPL-2.0
/*
 * vms_dlm_ldwv.c - the Lock Directory Weight Vector (FC-P4.3).
 *
 * The contract, the page cites and the reason the hash is never computed here
 * are in vms_dlm_ldwv.h. Read it first; this file is the behaviour.
 *
 * This TU is PURE: no seam call, no allocation, no clock, no library -- so it
 * runs identically in both kmods, in the host unit tests and in the rung-2
 * N-node simulator.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_dlm_ldwv.h"

/* ==========================================================================
 * Small shared helpers (a pure TU builds on the host too, where the
 * substrate's memset is not in scope -- the same rule vms_cnxman_phase2.c
 * follows)
 * ========================================================================== */

static void ldwv_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

static void ldwv_log(const struct cnxman_ops *ops, const char *msg)
{
	if (ops != NULL && ops->log != NULL)
		ops->log(ops->ctx, msg);
}

/* The CSV slot a CSID names: the low 16 bits index the Cluster System Vector
 * (p. 7-25). This is the ONE spelling of that rule in this file. */
static uint32_t ldwv_csv_slot(vms_csid_t csid)
{
	return (uint32_t)(csid & 0xffffu);
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */

void vms_ldwv_init(struct vms_ldwv *v)
{
	if (v == NULL)
		return;
	ldwv_bzero(v, (uint32_t)sizeof(*v));
}

void vms_ldwv_invalidate(struct vms_ldwv *v)
{
	if (v == NULL)
		return;
	v->valid = 0u;
	v->n = 0u;
	v->n_members = 0u;
	v->weights_learned = 0u;
	v->generation++;   /* every change is a change: see the header, SS4 */
}

uint32_t vms_ldwv_generation(const struct vms_ldwv *v)
{
	return (v != NULL) ? v->generation : 0u;
}

/* ==========================================================================
 * The p. 6-32 entry count
 *
 * "Entries per system = that system's LOCKDIRWT. If LOCKDIRWT is 0 on every
 * member, the vector is forced to hold one entry per system."
 * ========================================================================== */

/*
 * Weigh one member. Returns the number of entries it takes, given whether the
 * all-zero rule is in force. A member whose LOCKDIRWT was never learned is not
 * weighable at all -- the caller has already refused by then (see
 * ldwv_survey()).
 */
static uint32_t ldwv_member_entries(const struct vms_ldwv_member *m, int all_zero)
{
	if (all_zero)
		return 1u;
	return (uint32_t)m->lockdirwt;
}

/*
 * Survey a member set: how many members, whether any weight is unknown,
 * whether every learned weight is 0, and the resulting entry count. One pass,
 * no state: the array form and the CLUB form both fold into it.
 */
struct ldwv_survey {
	uint32_t n_members;
	uint32_t entries;
	uint8_t  any_unknown;    /* a member with no learned LOCKDIRWT      */
	uint8_t  any_learned;    /* a member WITH a learned LOCKDIRWT       */
	uint8_t  all_zero;       /* every learned weight is 0               */
	uint8_t  pad;
};

static void ldwv_survey_init(struct ldwv_survey *s)
{
	ldwv_bzero(s, (uint32_t)sizeof(*s));
	s->all_zero = 1u;
}

static void ldwv_survey_add(struct ldwv_survey *s, const struct vms_ldwv_member *m)
{
	s->n_members++;
	if (!m->lockdirwt_valid) {
		s->any_unknown = 1u;
		return;
	}
	s->any_learned = 1u;
	if (m->lockdirwt != 0u)
		s->all_zero = 0u;
}

/*
 * The refusals, in one place (header SS3). A member set that mixes learned and
 * unlearned weights cannot be laid out: every entry after the unweighable
 * member would sit at an offset no other member's copy agrees with.
 */
static enum vms_ldwv_status ldwv_survey_verdict(const struct ldwv_survey *s)
{
	if (s->n_members == 0u)
		return VMS_LDWV_E_NOMEMBERS;
	/*
	 * All weights unknown is NOT a mixture: nobody has advertised a
	 * LOCKDIRWT, so the honest reading is the all-zero one and p. 6-32's
	 * one-entry-per-system rule applies (research note SS1/SS4.1). A
	 * MIXTURE is refused.
	 */
	if (s->any_unknown && s->any_learned)
		return VMS_LDWV_E_WEIGHTS;
	if (s->entries == 0u)
		return VMS_LDWV_E_NOMEMBERS;
	if (s->entries > VMS_LDWV_MAX_ENTRIES)
		return VMS_LDWV_E_TOOBIG;
	return VMS_LDWV_OK;
}

uint32_t vms_ldwv_entry_count(const struct vms_ldwv_member *m,
			      uint32_t n_members, int *all_zero)
{
	struct ldwv_survey s;
	uint32_t i;

	if (all_zero != NULL)
		*all_zero = 0;
	if (m == NULL || n_members == 0u)
		return 0u;

	ldwv_survey_init(&s);
	for (i = 0u; i < n_members; i++)
		ldwv_survey_add(&s, &m[i]);
	if (s.any_unknown && s.any_learned)
		return 0u;      /* unlayoutable; vms_ldwv_build says why */
	if (s.any_unknown)
		s.all_zero = 1u;
	for (i = 0u; i < n_members; i++)
		s.entries += ldwv_member_entries(&m[i], (int)s.all_zero);
	if (all_zero != NULL)
		*all_zero = (int)s.all_zero;
	return s.entries;
}

/* ==========================================================================
 * Laying the entries down
 * ========================================================================== */

/*
 * Append one system's CONTIGUOUS run (p. 6-32). A system's OWN entries read 0
 * in its own copy (p. 6-32, Fig. 6-18 p. 6-33), which is what makes "entry ==
 * 0" mean "this node is the directory" rather than "system zero".
 * Returns 0 if the run does not fit -- the caller has already sized it, so
 * that can only be a bug, and it fails loudly rather than truncating.
 */
static int ldwv_append_run(struct vms_ldwv *v, vms_csid_t csid,
			   uint32_t count, int is_local)
{
	uint32_t i;

	if (v->n + count > VMS_LDWV_MAX_ENTRIES)
		return 0;
	for (i = 0u; i < count; i++)
		v->entry[v->n + i] = is_local ? 0u : (uint32_t)csid;
	v->n += count;
	return 1;
}

enum vms_ldwv_status vms_ldwv_build(struct vms_ldwv *v,
				    const struct vms_ldwv_member *m,
				    uint32_t n_members)
{
	struct ldwv_survey s;
	enum vms_ldwv_status st;
	uint32_t i;

	if (v == NULL || m == NULL)
		return VMS_LDWV_E_INVAL;

	ldwv_survey_init(&s);
	for (i = 0u; i < n_members; i++)
		ldwv_survey_add(&s, &m[i]);
	if (s.any_unknown && !s.any_learned)
		s.all_zero = 1u;
	if (!(s.any_unknown && s.any_learned)) {
		for (i = 0u; i < n_members; i++)
			s.entries += ldwv_member_entries(&m[i], (int)s.all_zero);
	}

	st = ldwv_survey_verdict(&s);
	/* Whatever happens next, the OLD vector is gone (p. 6-33). */
	vms_ldwv_invalidate(v);
	if (st != VMS_LDWV_OK)
		return st;

	for (i = 0u; i < n_members; i++) {
		uint32_t run = ldwv_member_entries(&m[i], (int)s.all_zero);

		if (!ldwv_append_run(v, m[i].csid, run, (int)m[i].is_local)) {
			vms_ldwv_invalidate(v);
			return VMS_LDWV_E_TOOBIG;
		}
	}

	v->n_members = (uint8_t)((s.n_members > 255u) ? 255u : s.n_members);
	v->weights_learned = s.any_learned;
	v->valid = 1u;
	return VMS_LDWV_OK;
}

/* ==========================================================================
 * The index rule (p. 6-31) -- ONE spelling, used by everything
 * ========================================================================== */

enum vms_ldwv_status vms_ldwv_index(const struct vms_ldwv *v, uint16_t hash16,
				    uint32_t *out_index)
{
	if (v == NULL || out_index == NULL)
		return VMS_LDWV_E_INVAL;
	if (!v->valid || v->n == 0u)
		return VMS_LDWV_E_NOVEC;
	*out_index = (uint32_t)hash16 % v->n;
	return VMS_LDWV_OK;
}

enum vms_ldwv_status vms_ldwv_resolve(const struct vms_ldwv *v, uint16_t hash16,
				      vms_csid_t *out_csid)
{
	enum vms_ldwv_status st;
	uint32_t idx = 0u;

	if (out_csid == NULL)
		return VMS_LDWV_E_INVAL;
	st = vms_ldwv_index(v, hash16, &idx);
	if (st != VMS_LDWV_OK)
		return st;
	*out_csid = (vms_csid_t)v->entry[idx];   /* 0 == this node (p. 6-32) */
	return VMS_LDWV_OK;
}

int vms_ldwv_is_ours(const struct vms_ldwv *v, uint16_t hash16)
{
	vms_csid_t csid = 0u;

	if (vms_ldwv_resolve(v, hash16, &csid) != VMS_LDWV_OK)
		return 0;
	return (csid == 0u) ? 1 : 0;
}

/* ==========================================================================
 * The CLUB-facing half: CSV-index order, over real CSBs
 * ========================================================================== */

/* Is this CSB a member of the committed cluster whose CSID we have LEARNED?
 * Both halves matter: an unlearned CSID has no CSV slot, so it has no place in
 * the vector, and asserting one would be an invented identity (INV-6). */
static int ldwv_csb_counts(const struct vms_csb *csb)
{
	if (!csb->in_use || !csb->csid_valid)
		return 0;
	return (csb->flags & VMS_CSB_F_SELECTED) != 0u;
}

/* Fill `m` from a CSB. The local system is the one flagged VMS_CSB_F_LOCAL --
 * a fact the CLUB holds, not a comparison against a CSID we might not have. */
static void ldwv_member_from_csb(const struct vms_csb *csb,
				 struct vms_ldwv_member *m)
{
	ldwv_bzero(m, (uint32_t)sizeof(*m));
	m->csid = csb->csid;
	m->lockdirwt = csb->lockdirwt;
	m->lockdirwt_valid = csb->lockdirwt_valid;
	m->is_local = (uint8_t)((csb->flags & VMS_CSB_F_LOCAL) != 0u);
}

/*
 * The next member in CSV-INDEX ORDER after slot `after`, or 0 when there is
 * none. A selection walk rather than a sorted copy: the CLUB holds up to 96
 * CSBs and a VAX kernel stack has no room for a member array (design SS3.9 --
 * "nothing here is copied onto one"). `*out` receives the member.
 */
static int ldwv_next_member(const struct vms_club *club, uint32_t after,
			    struct vms_ldwv_member *out, uint32_t *out_slot)
{
	const struct vms_csb *best = NULL;
	uint32_t best_slot = 0u;
	uint32_t i;

	for (i = 0u; i < club->n_csb; i++) {
		const struct vms_csb *csb = &club->csb[i];
		uint32_t slot;

		if (!ldwv_csb_counts(csb))
			continue;
		slot = ldwv_csv_slot(csb->csid);
		if (slot <= after)
			continue;
		if (best == NULL || slot < best_slot) {
			best = csb;
			best_slot = slot;
		}
	}
	if (best == NULL)
		return 0;
	ldwv_member_from_csb(best, out);
	*out_slot = best_slot;
	return 1;
}

/* Pass 1: survey the committed membership without materialising it. */
static void ldwv_survey_club(const struct vms_club *club, struct ldwv_survey *s)
{
	struct vms_ldwv_member m;
	uint32_t slot = 0u, next = 0u;

	ldwv_survey_init(s);
	while (ldwv_next_member(club, slot, &m, &next)) {
		ldwv_survey_add(s, &m);
		slot = next;
	}
	if (s->any_unknown && !s->any_learned)
		s->all_zero = 1u;
	if (s->any_unknown && s->any_learned)
		return;   /* unlayoutable; the verdict says why */

	slot = 0u;
	while (ldwv_next_member(club, slot, &m, &next)) {
		s->entries += ldwv_member_entries(&m, (int)s->all_zero);
		slot = next;
	}
}

/* Pass 2: lay the runs down in the same order pass 1 walked. */
static enum vms_ldwv_status ldwv_fill_club(struct vms_club *club,
					   const struct ldwv_survey *s)
{
	struct vms_ldwv_member m;
	uint32_t slot = 0u, next = 0u;

	while (ldwv_next_member(club, slot, &m, &next)) {
		uint32_t run = ldwv_member_entries(&m, (int)s->all_zero);

		if (!ldwv_append_run(&club->ldwv, m.csid, run, (int)m.is_local))
			return VMS_LDWV_E_TOOBIG;
		slot = next;
	}
	return VMS_LDWV_OK;
}

static const char *ldwv_refusal_line(enum vms_ldwv_status st)
{
	switch (st) {
	case VMS_LDWV_E_WEIGHTS:
		return "%CNXMAN, lock directory weight vector not rebuilt: "
		       "LOCKDIRWT known for some members and not others";
	case VMS_LDWV_E_TOOBIG:
		return "%CNXMAN, lock directory weight vector not rebuilt: "
		       "weighted member set exceeds this node's vector capacity";
	case VMS_LDWV_E_NOMEMBERS:
		return "%CNXMAN, lock directory weight vector not rebuilt: "
		       "no selected member with a known CSID";
	default:
		return "%CNXMAN, lock directory weight vector not rebuilt";
	}
}

enum vms_ldwv_status cnxman_ldwv_rebuild(struct vms_club *club,
					 const struct cnxman_ops *ops)
{
	struct ldwv_survey s;
	enum vms_ldwv_status st;

	if (club == NULL)
		return VMS_LDWV_E_INVAL;

	ldwv_survey_club(club, &s);
	st = ldwv_survey_verdict(&s);

	/* The old vector is discarded whatever the outcome (p. 6-33). */
	vms_ldwv_invalidate(&club->ldwv);
	if (st != VMS_LDWV_OK) {
		club->ldwv_build_refused++;
		ldwv_log(ops, ldwv_refusal_line(st));
		return st;
	}

	st = ldwv_fill_club(club, &s);
	if (st != VMS_LDWV_OK) {
		vms_ldwv_invalidate(&club->ldwv);
		club->ldwv_build_refused++;
		ldwv_log(ops, ldwv_refusal_line(st));
		return st;
	}

	club->ldwv.n_members = (uint8_t)((s.n_members > 255u) ? 255u : s.n_members);
	club->ldwv.weights_learned = s.any_learned;
	club->ldwv.valid = 1u;
	return VMS_LDWV_OK;
}

int cnxman_dir_lookup_received(struct vms_club *club, uint16_t hash16,
			       vms_csid_t from_csid, const struct cnxman_ops *ops)
{
	(void)from_csid;   /* named in the log line below, not in the decision */

	if (club == NULL)
		return 0;
	club->dir_lookups_received++;
	if (vms_ldwv_is_ours(&club->ldwv, hash16))
		return 1;

	/*
	 * Either our layout ORDER is wrong or our vector is stale relative to
	 * the sender's -- both falsify something worth knowing, and neither is
	 * a licence to guess (header SS5). Counted and announced, never served
	 * silently and never refused here.
	 */
	club->dir_lookup_misaddressed++;
	ldwv_log(ops, "%CNXMAN, directory lookup received for a hash this node "
		      "does not hold a directory entry slot for");
	return 0;
}
