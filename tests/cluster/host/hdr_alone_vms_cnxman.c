/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cnxman.c - vms_cnxman.h, alone, from a blank slate (FC-P0.1).
 *
 * This translation unit's ONLY project #include is vms_cnxman.h. If vms_cnxman.h
 * silently relies on some other header having been included first -- another
 * pure header, vms_internal.h, or exec_kbackend.h -- this file fails to
 * compile even though the same header works fine inside vms_pe.c, which pulls
 * in a dozen other includes first. See test_headers_host.c for the full
 * rationale. Not linked into anything; a compile-only object proves the point.
 */
/* OVMX_CLUSTER_HOST is supplied by CMakeLists.txt (target_compile_definitions),
 * matching the selection mechanism vms_cluster.h itself documents. */
#include <stdint.h>
#include <stddef.h>

#include "vms_cnxman.h"

int ovmx_hdr_alone_vms_cnxman(void);
int ovmx_hdr_alone_vms_cnxman(void)
{
	return 0;
}
