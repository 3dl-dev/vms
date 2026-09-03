/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_cluster_sysgen.h - load the cluster SYSGEN parameters into the
 * executive's ONE struct vms_cluster (FC-P0.10).
 *
 * Design: docs/design-faithful-cluster-executive.md, plan row FC-P0.10.
 * struct vms_cluster_params (vms_cluster.h section 2) is already the typed
 * SYSGEN store the cluster stack reads; this pair (.h/.c) is the ONE place
 * that VALIDATES a caller-supplied set of those fields and, only if valid,
 * commits it into a real struct vms_cluster -- so VMS_IOCTL_SYSGEN_LOAD's
 * per-substrate dispatcher (vms_devtab.c on Linux, vms_netbsd.c on NetBSD)
 * has no validation logic of its own to keep in sync between the two.
 *
 * PURE, like vms_cnxman_csb.c and vms_cluster_api.c beside it: no kernel
 * header, no substrate idiom, no SS$_ status vocabulary (that translation is
 * the ioctl dispatcher's job, exactly as vms_cluster_api.c's $GETSYI
 * projection returns nothing but the view it filled). Builds at R1 with a
 * plain host compiler (-DOVMX_CLUSTER_HOST) and is exercised in milliseconds
 * there, including the negctl this item's plan row names.
 *
 * INCLUDES: kernel-core headers only (CI gate tools/ci/cluster_core_includes_gate.sh).
 */
#ifndef OVMX_VMS_CLUSTER_SYSGEN_H
#define OVMX_VMS_CLUSTER_SYSGEN_H

#include "vms_cluster.h"

/*
 * cluster_sysgen_load - validate *in, then, only if valid, copy it into
 * cl->params.
 *
 * The one rule vms_cluster.h's own struct vms_cluster_params comment states
 * ("identity ... fatal if absent with vaxcluster >= 1, as on VMS"): a node
 * configured to ever join or always run in a cluster (VAXCLUSTER 1 or 2)
 * needs a configured SCSNODE. VAXCLUSTER 0 never attempts to join, so an
 * unconfigured node name in that mode is not an error -- SYSGEN was simply
 * never asked to load a cluster identity.
 *
 * Returns 1 (valid; cl->params now holds *in) or 0 (invalid; cl->params is
 * left at whatever it already honestly held -- INV-6: never a half-written
 * or fabricated identity). The caller maps 0 to SS$_BADPARAM.
 */
int cluster_sysgen_load(struct vms_cluster *cl,
                        const struct vms_cluster_params *in);

#endif /* OVMX_VMS_CLUSTER_SYSGEN_H */
