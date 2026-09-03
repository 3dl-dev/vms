/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_sysgen.c - see vms_cluster_sysgen.h for the contract and the
 * grounding. This TU is the whole body: one validation rule, one commit.
 */
#include "vms_cluster.h"
#include "vms_cluster_sysgen.h"

/*
 * cluster_sysgen_params_valid - vms_cluster.h section 2's own rule: SCSNODE
 * is fatal-if-absent once VAXCLUSTER says this node ever joins a cluster.
 */
static int cluster_sysgen_params_valid(const struct vms_cluster_params *in)
{
	if (in->vaxcluster >= 1 && in->scsnode_len == 0)
		return 0;
	return 1;
}

int cluster_sysgen_load(struct vms_cluster *cl,
                        const struct vms_cluster_params *in)
{
	if (cl == NULL || in == NULL)
		return 0;

	if (!cluster_sysgen_params_valid(in))
		return 0;

	cl->params = *in;
	return 1;
}
