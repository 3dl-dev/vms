/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cluster_opa0.c - vms_cluster_opa0.h, alone, from a blank slate
 * (rd vms-c56). The header is included by both substrates' exec_kbackend
 * rinds, so it must drag in nothing and warn about nothing under -Wall -Wextra
 * in a TU that uses none of it. A compile-only object; nothing here runs.
 */
#include "vms_cluster_opa0.h"

int ovmx_hdr_alone_vms_cluster_opa0(void);
int ovmx_hdr_alone_vms_cluster_opa0(void)
{
	return (int)VMS_OPA0_LINE_MAX + (int)VMS_OPA0_N_FORMS;
}
