/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_phase2.c - the p. 7-42 PHASE 2 COMMIT.
 *
 * The contract and the reason this is ONE implementation with two callers are
 * in vms_cnxman_phase2.h. Read it first; this file is the behaviour.
 *
 * This TU is PURE: no seam call, no allocation, no clock but ops->now_ms -- so
 * it runs identically in both kmods, in the host unit tests and in the rung-2
 * N-node simulator.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_phase2.h"

/* ==========================================================================
 * Small shared helpers (this TU calls no library: a pure TU builds on the host
 * too, where the substrate's memset is not in scope)
 * ========================================================================== */

static void phase2_log(const struct cnxman_ops *ops, const char *msg)
{
	if (ops != NULL && ops->log != NULL)
		ops->log(ops->ctx, msg);
}

static uint32_t phase2_now(const struct cnxman_ops *ops)
{
	if (ops != NULL && ops->now_ms != NULL)
		return ops->now_ms(ops->ctx);
	return 0u;
}

static void phase2_bzero(void *p, uint32_t n)
{
	uint8_t *o = (uint8_t *)p;
	uint32_t i;

	for (i = 0; i < n; i++)
		o[i] = 0u;
}

uint32_t cnxman_phase2_popcount8(uint8_t v)
{
	uint32_t n = 0u;

	while (v != 0u) {
		n += (uint32_t)(v & 1u);
		v = (uint8_t)(v >> 1);
	}
	return n;
}

/* ==========================================================================
 * Task 1 -- the nodemap into the CSBs
 * ========================================================================== */

/*
 * Is CSB `csb` in the transition's nodemap? Only a CSB whose CSID this node has
 * actually LEARNED can be answered; nodemap bit = CSID low 16 bits (book
 * p. 7-34 fn, p. 7-25). An unlearned CSID answers "unknown", never "no".
 */
static int phase2_csb_in_nodemap(const struct cnxman_phase2_in *in,
				 const struct vms_csb *csb, int *known)
{
	uint32_t slot;

	*known = 0;
	if (!csb->csid_valid)
		return 0;
	slot = (uint32_t)(csb->csid & 0xffffu);
	if (slot >= CNXMAN_PHASE2_BITMAP_SLOTS)
		return 0;   /* beyond the byte the wire showed us: unknown */
	*known = 1;
	return (in->bitmap & (uint8_t)(1u << slot)) != 0u;
}

/*
 * p. 7-42 task 1: "The nodemap in the CLUB is copied into each CSB
 * corresponding to a system selected to be in the cluster", and p. 7-49's
 * SELECTED flags are what the member count is then taken from.
 *
 * Returns the number of nodemap bits this node could MATCH to a CSB. If that is
 * zero the caller leaves membership alone: rewriting selection from a map we
 * cannot read would commit a zero-member cluster, which is a fabrication with a
 * catastrophic failure mode.
 */
static uint32_t phase2_apply_nodemap(struct vms_club *club,
				     const struct cnxman_phase2_in *in,
				     struct cnxman_phase2_stats *st)
{
	uint32_t i, matched = 0u;

	for (i = 0; i < club->n_csb; i++) {
		struct vms_csb *csb = &club->csb[i];
		int known = 0, in_map;

		if (!csb->in_use)
			continue;
		in_map = phase2_csb_in_nodemap(in, csb, &known);
		if (!known)
			continue;
		matched++;
		if (in_map)
			cnxman_csb_set_flags(csb, (uint16_t)(VMS_CSB_F_SELECTED |
							     VMS_CSB_F_MEMBER));
		else
			cnxman_csb_clear_flags(csb,
					       (uint16_t)(VMS_CSB_F_SELECTED |
							  VMS_CSB_F_MEMBER));
	}
	if (matched < in->bitmap_popcount)
		st->nodemap_unmapped += (uint32_t)in->bitmap_popcount - matched;
	return matched;
}

/* ==========================================================================
 * Task 2 -- proposed quorum cells become effective
 * ========================================================================== */

/*
 * p. 7-42 task 2: "Quorum data is updated in the CLUB to reflect that what has
 * been proposed has now been accepted", by copying the PROPOSED cells to the
 * EFFECTIVE ones. FC-P3.7 owns the arithmetic that fills the proposed cells; if
 * it has not run, this copies NOTHING -- a zero quorum asserted here would be a
 * fabricated quorum, which is the one class of value INV-6 names outright.
 */
static void phase2_commit_quorum(struct vms_club *club)
{
	if (!club->proposed_valid)
		return;
	club->cevotes = club->proposed_cevotes;
	club->quorum = club->proposed_quorum;
	club->qdisk_votes = club->proposed_qdisk_votes;
	club->proposed_valid = 0u;   /* p. 7-41: ignored outside a transition */
}

/* ==========================================================================
 * Task 3 -- THE COUNT
 * ========================================================================== */

/*
 * p. 7-42 task 3: "The total number of members (excluding the quorum disk) is
 * stored in the CLUB. This is simply the total number of CSBs whose SELECTED
 * flags are set." Taken from the CSBs, not from the wire's popcount -- and the
 * two are COMPARED, because a disagreement means part of the nodemap could not
 * be read and is exactly the symptom a width bug produces.
 */
static uint32_t phase2_commit_count(struct vms_club *club,
				    const struct cnxman_phase2_in *in,
				    struct cnxman_phase2_stats *st,
				    const struct cnxman_ops *ops)
{
	uint32_t members = cnxman_club_recount_members(club);

	if (in->bitmap_valid && members != (uint32_t)in->bitmap_popcount) {
		st->count_mismatch++;
		phase2_log(ops, "%CNXMAN, committed member count differs from "
				"the transition nodemap");
	}
	if (in->bitmap_valid && members > (uint32_t)in->bitmap_popcount)
		st->bitmap_short++;
	if (members > CNXMAN_PHASE2_M_GROUNDED)
		st->m_above_grounded++;
	return members;
}

/* ==========================================================================
 * Task 4 -- this system's own CLUSTER flag
 * ========================================================================== */

/*
 * p. 7-42 task 4: "VAX_B sets the CLUSTER flag in its own CLUB, indicating that
 * it is a member of a cluster." OVMX's CLUB has no such flag; the executive's
 * equivalent is the node state SHOW CLUSTER and $GETSYI project, and it is set
 * only from the LOCAL CSB actually carrying MEMBER -- never from "a transition
 * happened".
 */
static void phase2_commit_local_membership(struct vms_cluster *cl,
					   const struct cnxman_ops *ops)
{
	struct vms_csb *local = cnxman_club_local(&cl->club);

	if (local == NULL || !cnxman_csb_is_member(local))
		return;
	if (cl->state == VMS_CLUSTER_MEMBER)
		return;   /* already a member: the coordinator's usual case */
	cl->state = VMS_CLUSTER_MEMBER;
	phase2_log(ops, "%CNXMAN, this node is a member of the cluster");
}

/* ==========================================================================
 * The four tasks, in the published order
 * ========================================================================== */

uint32_t cnxman_phase2_commit(struct vms_cluster *cl,
			      const struct cnxman_phase2_in *in,
			      struct cnxman_phase2_stats *st,
			      const struct cnxman_ops *ops)
{
	struct cnxman_phase2_stats local_st;
	struct vms_club *club;
	uint32_t members;

	if (st == NULL)
		st = &local_st;
	phase2_bzero(st, (uint32_t)sizeof(*st));
	if (cl == NULL || in == NULL)
		return 0u;
	club = &cl->club;

	/* The nodemap only exists on a class-0x02 ADD open (wire spec SS4(p): a
	 * class-0x03 removal "has no op 0x09 at all ... and so carries no
	 * bitmap"). Without one, membership stands as the CSB ladder already
	 * left it and only the count is recomputed. */
	if (in->bitmap_valid && phase2_apply_nodemap(club, in, st) == 0u) {
		st->nodemap_unreadable++;
		phase2_log(ops, "%CNXMAN, no nodemap slot matched a known "
				"system; membership left unchanged");
	}

	phase2_commit_quorum(club);
	members = phase2_commit_count(club, in, st, ops);
	phase2_commit_local_membership(cl, ops);

	club->last_transition_ms = phase2_now(ops);
	return members;
}
