/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cnxman_quorum.c - the Quorum Algorithm: CEVOTES/QUORUM computation,
 * tracking only (FC-P3.7).
 *
 * The contract, the grounding and the INV-6 rules are in
 * vms_cnxman_quorum.h. This file is the arithmetic: walk the CLUB's own
 * SELECTED, params_valid CSBs (never a caller-supplied membership list),
 * apply the published max{} formula, and store the result back into the CLUB
 * fields FC-P3.6 declared but deliberately left unwritten.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_cluster.h"
#include "vms_cnxman.h"
#include "vms_cnxman_csb.h"
#include "vms_cnxman_quorum.h"

/*
 * p. 7-6: "the selected set of proposed members" is the CSB table's own
 * SELECTED flag (p. 7-49) -- never a caller's opinion. A CSB that never
 * learned its PARAMS record (params_valid == 0) contributes nothing to any
 * sum below: an un-advertised VOTES is unknown, not zero (INV-6).
 */
static int quorum_csb_counts(const struct vms_csb *csb)
{
	if (csb == NULL || !csb->in_use)
		return 0;
	if ((csb->flags & VMS_CSB_F_SELECTED) == 0u)
		return 0;
	return csb->params_valid != 0u;
}

/*
 * p. 7-4/7-5: a member's votes are AVAILABLE (count toward PRESENT) only
 * while it is reachable. The LOCAL CSB always is -- it is this system, not a
 * connection that can be down. A remote CSB is reachable only in state OPEN:
 * p. 7-30 holds membership (SELECTED stays set) across a reconnect window,
 * but that is precisely the case where the system is NOT currently
 * contributing votes.
 */
static int quorum_csb_present(const struct vms_csb *csb)
{
	if (!quorum_csb_counts(csb))
		return 0;
	if ((csb->flags & VMS_CSB_F_LOCAL) != 0u)
		return 1;
	return csb->state == (uint8_t)VMS_CNXMAN_CSB_OPEN;
}

void cnxman_quorum_recompute(struct vms_club *club)
{
	uint32_t i;
	uint32_t max_expected = 0u;
	uint32_t sum_votes = 0u;
	uint32_t present_votes = 0u;
	uint32_t new_cevotes;

	if (club == NULL)
		return;

	for (i = 0; i < club->n_csb; i++) {
		const struct vms_csb *csb = &club->csb[i];

		if (!quorum_csb_counts(csb))
			continue;
		if ((uint32_t)csb->expected_votes > max_expected)
			max_expected = (uint32_t)csb->expected_votes;
		sum_votes += (uint32_t)csb->votes;
		if (quorum_csb_present(csb))
			present_votes += (uint32_t)csb->votes;
	}

	/*
	 * p. 7-6: New CEVOTES = max{EXPECTED_VOTES; SUM VOTES; Old CEVOTES}.
	 * club->cevotes IS "Old CEVOTES" on entry -- reading the CLUB's own
	 * persisted field back into the max{}, rather than keeping a separate
	 * running-max cache, is what makes the value never decrease on its
	 * own (pp. 7-10/7-11) with no extra bookkeeping to keep in step.
	 */
	new_cevotes = (uint32_t)club->cevotes;
	if (max_expected > new_cevotes)
		new_cevotes = max_expected;
	if (sum_votes > new_cevotes)
		new_cevotes = sum_votes;
	if (new_cevotes > 0xffffu)
		new_cevotes = 0xffffu;  /* the CLUB field is 16 bits; VMS's own
					 * VOTES/EXPECTED_VOTES SYSGEN params
					 * are themselves 16-bit, so this clamp
					 * never actually triggers */

	club->cevotes = (uint16_t)new_cevotes;
	club->quorum = (uint16_t)((new_cevotes + 2u) / 2u);
	club->expected_votes = (uint16_t)max_expected;
	club->quorum_lost = (uint8_t)(present_votes < (uint32_t)club->quorum);

	/* club->qdisk_votes is a TRACKED readback (design SS3.4 lists it among
	 * the fields this item computes), kept in step here even though it
	 * plays no part in the max{} above -- see cnxman_quorum_qdskvotes()'s
	 * own doc comment for why folding it in is P8's job, not this one's. */
	club->qdisk_votes = cnxman_quorum_qdskvotes(club);
}

uint16_t cnxman_quorum_qdskvotes(const struct vms_club *club)
{
	const struct vms_csb *local;

	if (club == NULL || club->local_csb < 0)
		return 0u;
	if ((uint32_t)club->local_csb >= club->n_csb)
		return 0u;

	local = &club->csb[(uint32_t)club->local_csb];
	if (!local->in_use || !local->params_valid)
		return 0u;
	return local->qdskvotes;
}
