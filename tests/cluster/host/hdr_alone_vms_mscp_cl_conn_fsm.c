/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_mscp_cl_conn_fsm.c - vms_mscp_cl_conn_fsm.h, alone, from a
 * blank translation unit.
 *
 * This translation unit's ONLY project #include is vms_mscp_cl_conn_fsm.h. If
 * that header does not pull its own dependencies, this fails to compile --
 * which is the point. It is NOT the same proof as compiling
 * vms_mscp_cl_conn_fsm.c, which pulls its dependencies in itself. Not linked
 * into any binary; the compile IS the test (see the sibling hdr_alone_* TUs).
 */
#include "vms_mscp_cl_conn_fsm.h"

int ovmx_hdr_alone_vms_mscp_cl_conn_fsm(void);
int ovmx_hdr_alone_vms_mscp_cl_conn_fsm(void)
{
	return (int)MSCP_CL_CONN_STATE__COUNT;
}
