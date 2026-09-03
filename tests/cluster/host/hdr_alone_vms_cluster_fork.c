/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cluster_fork.c - vms_cluster_fork.h, alone, from a blank slate
 * (FC-P0.5).
 *
 * This translation unit's ONLY #include -- project or standard -- is
 * vms_cluster_fork.h. It is stricter than its siblings on purpose: the fork
 * header carries its own fixed-width-type selection block (the same one
 * vms_cluster_codec.h carries), so it must need NOTHING included ahead of it.
 * If someone ever deletes that block because "stdint is always there", this
 * file stops compiling.
 */
#include "vms_cluster_fork.h"

int ovmx_hdr_alone_vms_cluster_fork(void);
int ovmx_hdr_alone_vms_cluster_fork(void)
{
	/* Touch the vocabulary so the header cannot degrade to an empty file. */
	return (int)CF_OWNER__COUNT + (int)CF_OK +
	       (int)sizeof(struct cf_work) + (int)sizeof(struct cf_stats) +
	       (int)sizeof(struct cf_lanbuf) + (int)sizeof(struct cf_config);
}
