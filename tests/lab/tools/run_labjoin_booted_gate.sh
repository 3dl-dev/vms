#!/bin/bash
# run_labjoin_booted_gate.sh - opt-in entry point for THE REAL 0.6 cluster
# acceptance gate (rd vms-fa1a -> vms-5a23 -> CLOSES vms-110b): a BOOTED OVMX node
# joins a REAL VAX VMScluster (lab-2). Mirrors run_sysboot_cluster_params_e2e.sh:
# requires an explicit opt-in and real infrastructure, and SKIPs (exit 77) rather
# than pretending when neither is present.
#
# THIS IS NOT A GITHUB-CI GATE. lab-2 (the genuine OpenVMS VAX V7.3 cluster) lives
# on the k3s cluster and is unreachable from GitHub runners, and the in-pod boot
# is TCG-slow -- so GitHub CI runs only the hermetic plumbing/negctl gates
# (labjoin_booted_plumbing / labjoin_booted_negctl in ctest). THIS wrapper is the
# coordinator-run gate against a real lab-2 pod.
#
# EXPECTED: RED until vms-5ad (booted OVMX auto-starts SCS) lands. Today a booted
# OVMX never spawns SCS, so it never joins and this gate FAILS honestly -- the
# anti-fabrication instrument working as intended. It goes GREEN when 110b.1 lands.
#
# ANTI-FABRICATION TEETH (vms-fa1a leg (e)): the booted-OVMX node runs with
# CAP_NET_RAW DROPPED from its whole QEMU subtree (labjoin_pod_boot.sh via capsh),
# and the verdict (lj_booted_gate_verdict) requires BOTH the four-leg join AND
# that dropped-cap evidence. A green therefore CANNOT be the 0.6 crutch (a probe
# riding the pod's ambient CAP_NET_RAW): with the cap denied a userspace AF_PACKET
# raw open EPERMs, so a real join can only be the executive's KERNEL socket doing
# the L2 I/O. Requires the lab image built with libcap2-bin (capsh); the run
# FATALs honestly if capsh is absent rather than launching without the drop.
#
# Env:
#   OVMX_LAB2_JOIN   must be "1" or this SKIPs (exit 77).
#   LAB2_POD         target pod (default: vaxlab-0). MUST be a healthy CN_2 pod.
#   ART_DIR          dir with vmlinuz + initramfs-ovmx-slim.cpio.gz +
#                    ovmx-distrib.img (extract from distro/Dockerfile.bootable).
#   JOIN_DUR         join window seconds (default 300 -- TCG boot + join).
#   OVMX_PREFIX      SCSNODE prefix for mk_sysgen --alloc (default OVMXJ).
#
# Usage:
#   OVMX_LAB2_JOIN=1 LAB2_POD=vaxlab-0 ART_DIR=/tmp/ovmx-boot-art \
#       tests/lab/tools/run_labjoin_booted_gate.sh
set -uo pipefail
SKIP=77
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "${OVMX_LAB2_JOIN:-0}" != "1" ]; then
    echo "SKIP: the booted-OVMX-joins-a-real-VAX gate needs OVMX_LAB2_JOIN=1 and a live lab-2 pod."
    echo "      GitHub CI proves the harness logic via the labjoin_booted_plumbing / _negctl ctests;"
    echo "      this heavy gate is coordinator-run against lab-2. See tests/lab/README.md."
    exit "$SKIP"
fi
command -v kubectl >/dev/null 2>&1 || { echo "SKIP: kubectl not available -- no lab-2 access"; exit "$SKIP"; }

POD="${LAB2_POD:-vaxlab-0}"
NS="${NS:-ovmx-lab}"
kubectl -n "$NS" get pod "$POD" >/dev/null 2>&1 || { echo "SKIP: pod $NS/$POD not reachable"; exit "$SKIP"; }

ART_DIR="${ART_DIR:?ART_DIR required: dir with vmlinuz, initramfs-ovmx-slim.cpio.gz, ovmx-distrib.img}"
for f in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img; do
    [ -f "$ART_DIR/$f" ] || { echo "FATAL: $ART_DIR/$f missing (extract from distro/Dockerfile.bootable)"; exit 1; }
done

TAG="labjoin-$(date +%s)-$$"
exec "$HERE/labjoin_booted.sh" "$POD" "$TAG" "$ART_DIR" "${JOIN_DUR:-300}"
