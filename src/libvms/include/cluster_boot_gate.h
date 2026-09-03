/*
 * cluster_boot_gate.h - the FC-P0.11 VAXCLUSTER boot-path decision.
 *
 * STARTUP.EXE's SYSINIT-equivalent (docs/design-faithful-cluster-executive.md
 * SS3.5 step 2) issues VMS_IOCTL_CLUSTER_START only when the cluster SYSGEN
 * parameters just loaded (VMS_IOCTL_SYSGEN_LOAD, FC-P0.10) name a real
 * VAXCLUSTER setting. This header is the ONE place that decision lives, so
 * the boot path (ovmx_init.c) and its R1 host test (tests/cluster/host/
 * test_cluster_boot_gate.c) share the identical function rather than the
 * test re-deriving a copy that could silently drift from what ships.
 *
 * VMS SYSGEN convention (also documented at struct vms_sysgen_load_args's
 * `vaxcluster` field, src/kernel/vms_ioctl.h): 0 = never a cluster member,
 * 1 = a member only when a cluster is present, 2 = always a member. This
 * item's scope (plan row FC-P0.11) is the P0 "port up" semantic ONLY --
 * bringing PEA0: up so HELLOs flow, not the join-vs-standalone DECISION
 * (that is FC-P3.9, design SS3.5 step 2's own "(VAXCLUSTER=2, or 1 with a
 * cluster present) drives the join" sentence). A port that is not up can
 * never observe whether a cluster is present, so VAXCLUSTER=1 must ALSO
 * bring the port up -- exactly what vms_pe_start() (FC-P0.9,
 * src/kernel-core/vms_pe.c) already treats as "non-zero starts the port"
 * in its own `if (cl->params.vaxcluster == 0) return SS__NOSUCHDEV;` gate.
 * VAXCLUSTER=0 is the one honest silence: no PEA0:, no HELLO (INV-6 -- no
 * fabricated port on a standalone node).
 */
#ifndef OVMX_CLUSTER_BOOT_GATE_H
#define OVMX_CLUSTER_BOOT_GATE_H

#include <stdint.h>

/*
 * cluster_start_wanted - true iff the boot path should issue
 * VMS_IOCTL_CLUSTER_START for this VAXCLUSTER value. `vaxcluster` is the
 * value the executive actually committed (i.e. VMS_IOCTL_SYSGEN_LOAD
 * returned SS$_NORMAL for it) -- never a value read back after a refused
 * load, which would gate on a state the executive never actually holds.
 */
static inline int cluster_start_wanted(uint32_t vaxcluster)
{
    return vaxcluster != 0;
}

#endif /* OVMX_CLUSTER_BOOT_GATE_H */
