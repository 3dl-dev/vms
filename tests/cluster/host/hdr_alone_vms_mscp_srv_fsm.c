/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_mscp_srv_fsm.c - vms_mscp_srv_fsm.h, alone, from a blank slate
 * (FC-P6.3, on FC-P0.1's pattern).
 *
 * This translation unit's ONLY project #include is vms_mscp_srv_fsm.h. If that
 * header silently relies on some other header having been included first --
 * the MSCP codec, vms_cluster.h -- this file fails to compile even though the
 * same header works fine inside vms_mscp_srv_fsm.c, which pulls its
 * dependencies in itself. Not linked into anything; a compile-only object
 * proves the point.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_mscp_srv_fsm.h"

int ovmx_hdr_alone_vms_mscp_srv_fsm(void);
int ovmx_hdr_alone_vms_mscp_srv_fsm(void)
{
	return (int)sizeof(struct mscp_srv_fsm) + (int)MSCP_SRV_EV__COUNT +
	       (int)MSCP_SRV_ST__COUNT;
}
