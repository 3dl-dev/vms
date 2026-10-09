/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_dlm_echo_guard.c - vms_dlm_echo_guard.h, alone, from a blank
 * slate (FC-P0.1's rule, applied to rd vms-b5b0's new pure header).
 *
 * This translation unit's ONLY project #include is vms_dlm_echo_guard.h. If it
 * silently relies on some other header having been included first, this file
 * fails to compile even though the same header works fine inside
 * vms_dlm_scs.c. See test_headers_host.c for the full rationale.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_dlm_echo_guard.h"

int ovmx_hdr_alone_vms_dlm_echo_guard(void);
int ovmx_hdr_alone_vms_dlm_echo_guard(void)
{
	return 0;
}
