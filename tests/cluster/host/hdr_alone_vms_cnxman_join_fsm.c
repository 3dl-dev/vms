/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_join_fsm.c - vms_cnxman_join_fsm.h, alone, from a blank
 * slate (FC-P3.3).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test. A compile-only object; nothing here runs.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_join_fsm.h"

int ovmx_hdr_alone_vms_cnxman_join_fsm(void);
int ovmx_hdr_alone_vms_cnxman_join_fsm(void)
{
	return (int)sizeof(struct cnxman_join) +
	       (int)sizeof(struct cnxman_join_cfg) +
	       (int)CNXMAN_JOIN_STATE__COUNT;
}
