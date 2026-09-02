/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_api.h - the personality readback surface: the $GETSYI
 * cluster-item-code projection (FC-P3.7 slice).
 *
 * Design: docs/design-faithful-cluster-executive.md SS3.2 (layering --
 * `vms_cluster_api.c` is "the personality surface: SYSGEN param load, cluster
 * start, CSB/CLUB readback, diagnostics", sitting between the executive
 * boundary and `vms_cnxman.c`), SS3.5 ("`VMS_IOCTL_CLUSTER_GET_CLUB` ->
 * `$GETSYI` CLUSTER_MEMBER/CLUSTER_NODES/CLUSTER_VOTES/CLUSTER_QUORUM/
 * CLUSTER_FSYSID/CLUSTER_FTIME/NODE_CSID/SCSNODE").
 *
 * SCOPE OF THIS SLICE (FC-P3.7, rung R1, blocked-by P3.6 only). The plan row
 * for THIS item names one rung -- R1 host unit -- and one dependency -- P3.6.
 * `VMS_IOCTL_CLUSTER_START`/`VMS_IOCTL_SYSGEN_LOAD` (FC-P0.10/P0.11) and the
 * fork-mutex-guarded `vms_cnxman.c` glue that will actually serve an ioctl
 * (`cnxman_get_club()`, already declared in vms_cnxman.h, FC-P3.8) have not
 * landed yet. So this file holds exactly what FC-P3.7 owns and can be host
 * unit tested today: the PURE projection from a real `struct vms_cluster` to
 * the $GETSYI item-code shaped view below. The ioctl number, the fork-mutex
 * glue and the `sys$getsyi`/CRTL cutover that CALLS this projection are
 * FC-P3.8/FC-P3.9's job (design SS3.5's "Retired"/"ioctls + mirrors" rows) --
 * naming them here would be inventing seam surface this item was not asked to
 * freeze. This is a documented scope boundary, not a silent omission.
 *
 * INV-6. Every field below is read straight from `struct vms_cluster` (SYSGEN
 * params already loaded) or from the CLUB (FC-P3.6/FC-P3.7's own fields) --
 * nothing is copied off a wire frame and nothing has a default. A field the
 * executive has not learned travels with its `_valid` companion clear, so a
 * caller blanks the F$GETSYI column instead of printing an invented value.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 * Pure: no seam call, no allocation, no clock.
 */
#ifndef OVMX_VMS_CLUSTER_API_H
#define OVMX_VMS_CLUSTER_API_H

#include "vms_cluster.h"

/*
 * The $GETSYI item codes FC-P3.7 owns, one field each. Fixed-width, no 64-bit
 * scalar (vms_cluster_snapshot.h rule 3): SCSSYSTEMID-shaped quantities carry
 * a `_lo`/`_hi` pair. Not a wire ABI (this struct never crosses an ioctl in
 * this slice), but held to the same discipline as vms_cluster_snapshot.h
 * because FC-P3.8 is expected to hand it across one unchanged.
 */
struct vms_getsyi_cluster_view {
	/* SYI$_CLUSTER_MEMBER: this node is a member of a cluster right now.
	 * Always meaningful -- cl->state itself is never "unlearned", it
	 * starts truthfully at VMS_CLUSTER_OFF. */
	uint8_t  cluster_member;

	/* SYI$_CLUSTER_NODES: the CLUB's own member count (FC-P3.6's
	 * cnxman_club_recount_members(), p. 7-49's SELECTED-flag count). */
	uint32_t cluster_nodes;

	/* SYI$_CLUSTER_VOTES: THIS system's own advertised VOTES, the SYSGEN
	 * value STARTUP.EXE loaded (design SS3.5's VMS_IOCTL_SYSGEN_LOAD) --
	 * always known once the params struct exists, never a peer's value. */
	uint16_t cluster_votes;

	/* SYI$_CLUSTER_QUORUM: club->quorum, FC-P3.7's own computed field. */
	uint16_t cluster_quorum;

	/* SYI$_CLUSTER_FSYSID: the founding member's SCSSYSTEMID. */
	uint32_t cluster_fsysid_lo;
	uint32_t cluster_fsysid_hi;
	uint8_t  cluster_fsysid_valid;   /* 0 until the CLUB learned it */

	/* SYI$_CLUSTER_FTIME: when the cluster formed, VMS absolute time. */
	uint32_t cluster_ftime_lo;
	uint32_t cluster_ftime_hi;
	uint8_t  cluster_ftime_valid;    /* 0 until the CLUB learned it */

	/* SYI$_NODE_CSID: this node's own CSID, LEARNED from the cluster
	 * (vms_cluster.h SS2: "never chosen"). */
	uint32_t node_csid;
	uint8_t  node_csid_valid;        /* 0 while this node is still NEW */
};

/*
 * Project `cl`'s real executive state into the $GETSYI cluster item codes
 * FC-P3.7 owns. `out` is zeroed first, so every field starts at its honest
 * "not learned" value before anything the executive actually knows overwrites
 * it. A NULL `cl` or `out` leaves `out` untouched (NULL `cl`) or zeroed
 * (NULL-safe on `cl` alone is not offered -- there is nothing to project).
 */
void cluster_api_getsyi_project(const struct vms_cluster *cl,
				struct vms_getsyi_cluster_view *out);

#endif /* OVMX_VMS_CLUSTER_API_H */
