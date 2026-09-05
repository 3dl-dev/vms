/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_api.c - the personality readback surface: the $GETSYI
 * cluster-item-code projection (FC-P3.7 slice).
 *
 * The contract, the scope boundary and the grounding are in
 * vms_cluster_api.h. This file is the projection itself: read the real
 * `struct vms_cluster` (SYSGEN params + CLUB) and fill the $GETSYI view,
 * blanking any field the executive has not learned rather than printing an
 * invented value (INV-6).
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */

#include "vms_cluster.h"
#include "vms_cluster_api.h"

/* No libc call in this TU -- see vms_cnxman_csb.c's csb_bzero for the same
 * discipline and the same reason: a pure kernel-core TU must build and run
 * identically on the host, in both kmods and in the N-node simulator. */
static void getsyi_view_zero(struct vms_getsyi_cluster_view *out)
{
	uint8_t *b = (uint8_t *)out;
	uint32_t i;

	for (i = 0; i < (uint32_t)sizeof(*out); i++)
		b[i] = 0u;
}

void cluster_api_getsyi_project(const struct vms_cluster *cl,
				struct vms_getsyi_cluster_view *out)
{
	const struct vms_club *club;

	if (out == NULL)
		return;
	getsyi_view_zero(out);
	if (cl == NULL)
		return;

	club = &cl->club;

	/* SYI$_CLUSTER_MEMBER: cl->state is never "unlearned" -- it starts
	 * truthfully at VMS_CLUSTER_OFF and this is a plain comparison, not a
	 * validity-gated read. */
	out->cluster_member = (uint8_t)(cl->state == VMS_CLUSTER_MEMBER);

	/* SYI$_CLUSTER_NODES: FC-P3.6's own SELECTED-flag member count. */
	out->cluster_nodes = club->cluster_nodes;

	/* SYI$_CLUSTER_VOTES: this system's own SYSGEN VOTES, not a peer's. */
	out->cluster_votes = cl->params.votes;

	/* SYI$_CLUSTER_QUORUM: FC-P3.7's own computed field
	 * (cnxman_quorum_recompute(), vms_cnxman_quorum.c). */
	out->cluster_quorum = club->quorum;

	/* SYI$_CLUSTER_FSYSID / SYI$_CLUSTER_FTIME: honestly absent until the
	 * CLUB has actually learned them (a transition has completed). */
	if (club->fsysid_valid) {
		out->cluster_fsysid_lo = (uint32_t)(club->fsysid & 0xffffffffu);
		out->cluster_fsysid_hi = (uint32_t)((club->fsysid >> 32) & 0xffffffffu);
		out->cluster_fsysid_valid = 1u;
	}
	if (club->ftime_valid) {
		out->cluster_ftime_lo = (uint32_t)(club->ftime & 0xffffffffu);
		out->cluster_ftime_hi = (uint32_t)((club->ftime >> 32) & 0xffffffffu);
		out->cluster_ftime_valid = 1u;
	}

	/* SYI$_NODE_CSID: LEARNED, never chosen (vms_cluster.h SS2). */
	if (club->local_csid_valid) {
		out->node_csid = club->local_csid;
		out->node_csid_valid = 1u;
	}
}
