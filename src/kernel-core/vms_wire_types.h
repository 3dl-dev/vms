/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vms_wire_types.h - the ONE sanctioned fixed-width integer vocabulary for the
 * executive-resident VMScluster wire code (codec + per-family codec TUs).
 *
 * The cluster codec and its per-family harvest TUs need uint8/16/32/64_t and
 * size_t, but design SS3.2.2 forbids a cluster-core TU from naming a substrate
 * header directly (that is what the include gate, tools/ci/
 * cluster_core_includes_gate.sh, enforces). This header is the sanctioned
 * exception: it is the SINGLE place the substrate's types are selected, exactly
 * as src/kernel/vms_internal.h is for the rest of the kernel. It is deliberately
 * NOT named to match the gate's cluster-TU scan set (the vms_cluster, vms_pe,
 * vms_scs, vms_cnxman, vms_dlm_scs, vms_mscp prefixes), so a wire TU includes
 * THIS (a quoted kernel-core header, gate-clean) rather than a bare substrate
 * header, and the substrate type choice lives here only.
 *
 * Host rung-1 (OVMX_CLUSTER_HOST, or neither kbackend defined): plain ISO C
 * <stdint.h>/<stddef.h>, so the codec host-tests build with no kernel headers.
 *
 * Clean-room (Rule 8): OVMX's own selection over PUBLIC host type headers only.
 */
#ifndef OVMX_VMS_WIRE_TYPES_H
#define OVMX_VMS_WIRE_TYPES_H

#if defined(OVMX_KBACKEND_NETBSD) && defined(_KERNEL)
#  include <sys/types.h>
#  include <sys/stdint.h>
#elif defined(OVMX_KBACKEND_LINUX) || defined(__KERNEL__)
#  include <linux/types.h>
#else
#  include <stddef.h>
#  include <stdint.h>
#endif

#endif /* OVMX_VMS_WIRE_TYPES_H */
