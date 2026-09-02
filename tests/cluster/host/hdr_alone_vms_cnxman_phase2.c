/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_phase2.c - vms_cnxman_phase2.h, alone, from a blank
 * slate (FC-P3.5 + FC-P3.12).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test. A compile-only object; nothing here runs.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_phase2.h"

int ovmx_hdr_alone_vms_cnxman_phase2(void);
int ovmx_hdr_alone_vms_cnxman_phase2(void)
{
	return (int)sizeof(struct cnxman_phase2_in) +
	       (int)sizeof(struct cnxman_phase2_stats) +
	       (int)CNXMAN_PHASE2_M_GROUNDED;
}
