/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_lock_nb.c - THE NETBSD SUBSTRATE'S ioctl MIRROR, compiled on
 * the host so its own _Static_asserts are EVALUATED (rd vms-5cd).
 *
 * WHY THIS FILE EXISTS, measured. src/kernel-netbsd/vms_lock_nb.h carries a
 * byte-for-byte duplicate of every CLUSTER_DIAG args struct, each pinned by a
 * _Static_assert whose whole job is to fail LOUDLY when the kernel-core view it
 * mirrors changes. Nothing in the host build compiled it: the only consumer is
 * tests/netbsd/guest/vmslock.c, which builds on the NetBSD-VAX rail. So on
 * 2026-10-22 the mirror was found with `vms_cluster_diag_dlm_args == 208` while
 * the row it mirrors had been 224 for hours (rd vms-28bd grew the view 200 ->
 * 216 and moved the assert in the other two copies only). An assert that is
 * never compiled is not a tripwire.
 *
 * It is the hdr_alone idiom (FC-P0.1) pointed at the OTHER substrate: nothing
 * but <stdint.h>/<stddef.h> plus the header itself, in its own object library
 * with its own include directory -- src/kernel-netbsd and src/kernel both
 * carry a vms_internal.h, so the two include paths must not be mixed in one
 * target.
 *
 * WHAT IT DOES AND DOES NOT PROVE. It proves the mirror's sizes agree with the
 * numbers the kernel-core header pins for the same structs, under an LP64 host
 * compiler; every field in these rows is fixed-width, so the VAX's ILP32 layout
 * is the same. It does not prove the NetBSD module BUILDS -- that is the VAX
 * rail's job (R4) -- and it does not replace vms_devtab.c's core-vs-Linux
 * cross-copy assert.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_lock_nb.h"

int ovmx_hdr_alone_vms_lock_nb(void);
int ovmx_hdr_alone_vms_lock_nb(void)
{
	/* A touch per mirrored row this file is the tripwire for, so a future
	 * `-Wunused` pass cannot quietly drop the translation unit. */
	return (int)(sizeof(struct vms_cluster_diag_dlm_args) +
		     sizeof(struct vms_dlm_scs_view_wire) +
		     sizeof(struct vms_cluster_diag_join_args));
}
