/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_quorum.c - vms_cnxman_quorum.h, alone, from a blank
 * slate (FC-P3.7).
 *
 * Same discipline as hdr_alone_vms_cnxman_csb.c: this translation unit's ONLY
 * project #include is vms_cnxman_quorum.h. A compile-only object; nothing
 * here runs.
 */
/* OVMX_CLUSTER_HOST is supplied by CMakeLists.txt, matching the selection
 * mechanism vms_cluster.h documents. */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_quorum.h"

int ovmx_hdr_alone_vms_cnxman_quorum(void);
int ovmx_hdr_alone_vms_cnxman_quorum(void)
{
	return (int)sizeof(uint16_t);
}
