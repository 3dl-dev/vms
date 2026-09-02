/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_coord_fsm.c - vms_cnxman_coord_fsm.h, alone, from a
 * blank slate (FC-P3.12).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test. A compile-only object; nothing here runs.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_coord_fsm.h"

int ovmx_hdr_alone_vms_cnxman_coord_fsm(void);
int ovmx_hdr_alone_vms_cnxman_coord_fsm(void)
{
	return (int)sizeof(struct cnxman_coord) +
	       (int)sizeof(struct cnxman_coord_link_ops) +
	       (int)sizeof(struct cnxman_coord_rebuild_ops) +
	       (int)CNXMAN_COORD_STATE__COUNT;
}
