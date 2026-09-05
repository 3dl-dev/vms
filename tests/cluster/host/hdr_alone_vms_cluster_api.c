/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cluster_api.c - vms_cluster_api.h, alone, from a blank slate
 * (FC-P3.7).
 *
 * Same discipline as hdr_alone_vms_cnxman_csb.c: this translation unit's ONLY
 * project #include is vms_cluster_api.h. A compile-only object; nothing here
 * runs.
 */
/* OVMX_CLUSTER_HOST is supplied by CMakeLists.txt, matching the selection
 * mechanism vms_cluster.h documents. */
#include <stdint.h>
#include <stddef.h>

#include "vms_cluster_api.h"

int ovmx_hdr_alone_vms_cluster_api(void);
int ovmx_hdr_alone_vms_cluster_api(void)
{
	return (int)sizeof(struct vms_getsyi_cluster_view);
}
