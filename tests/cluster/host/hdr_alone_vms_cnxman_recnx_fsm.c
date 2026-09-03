/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman_recnx_fsm.c - vms_cnxman_recnx_fsm.h, alone, from a
 * blank slate (FC-P3.6).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test. A compile-only object; nothing here runs.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman_recnx_fsm.h"

int ovmx_hdr_alone_vms_cnxman_recnx_fsm(void);
int ovmx_hdr_alone_vms_cnxman_recnx_fsm(void)
{
	return (int)sizeof(struct cnxman_recnx) +
	       (int)sizeof(struct cnxman_recnx_rec);
}
