/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_dlm_hash.c - vms_dlm_hash.h, alone, from a blank slate
 * (FC-P0.1's rule, applied to rd vms-66fe's new pure header).
 *
 * This translation unit's ONLY project #include is vms_dlm_hash.h. If it
 * silently relies on some other header having been included first, this file
 * fails to compile even though the header works fine next to its siblings.
 * See test_headers_host.c for the full rationale. Not linked into anything;
 * a compile-only object proves the point.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_dlm_hash.h"

int ovmx_hdr_alone_vms_dlm_hash(void);
int ovmx_hdr_alone_vms_dlm_hash(void)
{
	return 0;
}
