/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_dlm_dir.c - vms_dlm_dir.h, alone, from a blank slate
 * (FC-P0.1's rule, applied to rd vms-8219's new pure header).
 *
 * This translation unit's ONLY project #include is vms_dlm_dir.h. If it
 * silently relies on some other header having been included first -- another
 * pure header, vms_internal.h, or exec_kbackend.h -- this file fails to
 * compile even though the same header works fine inside vms_dlm_scs.c,
 * which pulls in three other includes first. See test_headers_host.c for the
 * full rationale. Not linked into anything; a compile-only object proves the
 * point.
 */
/* OVMX_CLUSTER_HOST is supplied by CMakeLists.txt (target_compile_definitions),
 * matching the selection mechanism vms_cluster.h itself documents. */
#include <stdint.h>
#include <stddef.h>

#include "vms_dlm_dir.h"

int ovmx_hdr_alone_vms_dlm_dir(void);
int ovmx_hdr_alone_vms_dlm_dir(void)
{
	return 0;
}
