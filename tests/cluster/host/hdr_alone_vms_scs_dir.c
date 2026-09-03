/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_scs_dir.c - vms_scs_dir.h, alone, from a blank slate
 * (FC-P2.3, on FC-P0.1's pattern).
 *
 * This translation unit's ONLY project #include is vms_scs_dir.h. If that
 * header silently relies on some other header having been included first --
 * vms_scs_fsm.h, vms_scs.h or the codec -- this file fails to compile even
 * though the same header works fine inside vms_scs_dir.c, which pulls its
 * dependencies in itself. Not linked into anything; a compile-only object
 * proves the point.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_scs_dir.h"

int ovmx_hdr_alone_vms_scs_dir(void);
int ovmx_hdr_alone_vms_scs_dir(void)
{
	return (int)sizeof(struct scs_dir) + (int)SCS_DIR_STATE__COUNT;
}
