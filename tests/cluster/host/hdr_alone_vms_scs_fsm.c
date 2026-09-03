/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_scs_fsm.c - vms_scs_fsm.h, alone, from a blank slate
 * (FC-P2.2, on FC-P0.1's pattern).
 *
 * This translation unit's ONLY project #include is vms_scs_fsm.h. If that
 * header silently relies on some other header having been included first --
 * vms_scs.h, the codec, vms_internal.h or exec_kbackend.h -- this file fails
 * to compile even though the same header works fine inside vms_scs_fsm.c,
 * which pulls its dependencies in itself. Not linked into anything; a
 * compile-only object proves the point.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_scs_fsm.h"

int ovmx_hdr_alone_vms_scs_fsm(void);
int ovmx_hdr_alone_vms_scs_fsm(void)
{
	return (int)sizeof(struct scs_cdt) + (int)VMS_SCS_CDT_STATE__COUNT;
}
