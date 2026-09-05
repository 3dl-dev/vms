/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hdr_alone_vms_cluster_emit_guard.c - vms_cluster_emit_guard.h, alone, from a
 * blank slate (E82).
 *
 * Same discipline as hdr_alone_vms_cluster.c: the ONLY project #include is the
 * header under test, so this proves the emit-time wire-safety guard's
 * interface drags in nothing but the codec header it names. A compile-only
 * object; nothing here runs.
 */
#include <stdint.h>
#include <stddef.h>

#include "vms_cluster_emit_guard.h"

int ovmx_hdr_alone_vms_cluster_emit_guard(void);
int ovmx_hdr_alone_vms_cluster_emit_guard(void)
{
	return (int)sizeof(struct cm_guard) +
	       (int)sizeof(struct cm_guard_facts) +
	       (int)sizeof(struct cm_guard_frame) +
	       (int)sizeof(struct cm_guard_finding) +
	       (int)CM_GUARD_C__COUNT;
}
