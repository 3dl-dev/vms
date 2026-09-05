/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_csb.c - vms_cnxman_csb.h, alone, from a blank slate
 * (FC-P3.6).
 *
 * Same discipline as hdr_alone_vms_cluster.c: this translation unit's ONLY
 * project #include is vms_cnxman_csb.h. If it silently relies on some other
 * header having been included first, this file fails to compile even though the
 * same header works fine inside vms_cnxman_csb.c, which pulls in three others
 * before it. A compile-only object; nothing here runs.
 */
/* OVMX_CLUSTER_HOST is supplied by CMakeLists.txt, matching the selection
 * mechanism vms_cluster.h documents. */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_csb.h"

int ovmx_hdr_alone_vms_cnxman_csb(void);
int ovmx_hdr_alone_vms_cnxman_csb(void)
{
	return (int)sizeof(struct vms_csb) + (int)CNXMAN_CSB_EV__COUNT;
}
