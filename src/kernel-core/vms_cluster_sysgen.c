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
	/* The commit itself is what makes the record real. Set AFTER the copy,
	 * never from *in, so no caller can hand the executive a "these are
	 * loaded" claim it did not make (see struct vms_cluster.params_valid). */
	cl->params_valid = 1u;
	return 1;
}

int cluster_sysgen_sw_version(const struct vms_cluster *cl,
                              uint8_t out[VMS_CLUSTER_SWVER_LEN])
{
	uint32_t n, i;

	if (cl == NULL || out == NULL)
		return 0;
	if (!cl->params_valid || cl->params.sw_version_len == 0u)
		return 0;

	n = (uint32_t)cl->params.sw_version_len;
	if (n > (uint32_t)VMS_CLUSTER_SWVER_LEN)
		n = (uint32_t)VMS_CLUSTER_SWVER_LEN;

	for (i = 0; i < (uint32_t)VMS_CLUSTER_SWVER_LEN; i++)
		out[i] = (i < n) ? cl->params.sw_version[i] : (uint8_t)' ';
	return 1;
}

int cluster_sysgen_credits(const struct vms_cluster *cl, uint8_t *out)
{
	if (cl == NULL || out == NULL)
		return 0;
	if (!cl->params_valid)
		return 0;
	if (cl->params.cluster_credits > 0xffu)
		return 0;

	*out = (uint8_t)cl->params.cluster_credits;
	return 1;
}
