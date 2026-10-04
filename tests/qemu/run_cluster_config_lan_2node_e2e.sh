#!/bin/bash
# run_cluster_config_lan_2node_e2e.sh - ctest/CI entry point for
# cluster_config_lan_2node_e2e (vms-23b9 rung 2).
#
# Same shape as run_cluster_config_lan_e2e.sh (rung 1) -- read that file's
# header first. THE GATE ITSELF (tests/qemu/test_cluster_config_lan_2node_e2e.sh)
# drives FOUR real QEMU boots (two per node: configure, then power-cycle
# networked) inside ONE docker container and takes real minutes -- longer than
# rung 1's two boots, since node A's second boot is held open for the whole
# run so this script can prove it independently learns node B's admission.
#
# Env knobs (same meaning as rung 1's runner):
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already present.
#   BOOT_TIMEOUT           per-node boot-1 (configure) timeout, forwarded.
#   JOIN_TIMEOUT           per-node boot-2 (networked) timeout, forwarded.
#   SETTLE_SECS            forwarded (writeback settle).
#   JOIN_POLL              forwarded (membership poll window).
#   RIG_NEGCTL             "1" runs the negative control (both nodes VOTES=0).
#
# HARD OUTER BOUND: `timeout --kill-after=30 <budget>` wraps the ENTIRE docker
# run; a named, unique container is force-removed on any exit path so a killed
# wrapper cannot leave an orphaned container running server-side.
#
# Usage:
#   docker build -t ovmx-boot -f distro/Dockerfile.bootable .
#   OVMX_QEMU_FULL_E2E=1 tests/qemu/run_cluster_config_lan_2node_e2e.sh

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
JOIN_TIMEOUT="${JOIN_TIMEOUT:-900}"
SETTLE_SECS="${SETTLE_SECS:-60}"
JOIN_POLL="${JOIN_POLL:-150}"
RIG_NEGCTL="${RIG_NEGCTL:-0}"
# 2 configure boots (each up to BOOT_TIMEOUT) + 2 settle sleeps + node A's
# networked boot held open for the ENTIRE remaining run (bounded at
# JOIN_TIMEOUT) + generous slack for the interactive drive and image I/O.
TOTAL_BUDGET=$(( BOOT_TIMEOUT * 2 + SETTLE_SECS * 2 + JOIN_TIMEOUT + 300 ))
CONTAINER_NAME="ovmx-cluster-config-lan-2node-e2e-$$"
cleanup() { docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: cluster_config_lan_2node_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boots, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_cluster_config_lan_2node_e2e.sh"
    exit "$SKIP"
fi

command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    if [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
        echo "--- building bootable image ($IMAGE) ---"
        docker build -t "$IMAGE" -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
            || { echo "FAIL: bootable image build"; exit 1; }
    else
        echo "FATAL: image '$IMAGE' not found. Either:"
        echo "  docker build -t $IMAGE -f distro/Dockerfile.bootable ."
        echo "or re-run with OVMX_BUILD_BOOT_IMAGE=1 to build it here."
        exit 1
    fi
fi

# --device /dev/kvm is opportunistic (KVM if present, TCG fallback inside the
# gate itself); not requesting it here would not deny KVM to a CI runner that
# has it, but the flag itself is a no-op if /dev/kvm does not exist on the
# host, so it is always safe to pass.
KVM_ARGS=()
[ -e /dev/kvm ] && KVM_ARGS=(--device /dev/kvm)

timeout --kill-after=30 "$TOTAL_BUDGET" \
    docker run --rm --name "$CONTAINER_NAME" \
    "${KVM_ARGS[@]}" \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT}" \
    -e "JOIN_TIMEOUT=${JOIN_TIMEOUT}" \
    -e "SETTLE_SECS=${SETTLE_SECS}" \
    -e "JOIN_POLL=${JOIN_POLL}" \
    -e "RIG_NEGCTL=${RIG_NEGCTL}" \
    -v "$REPO_ROOT/tests/qemu/test_cluster_config_lan_2node_e2e.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
rc=$?
if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    echo "FATAL: cluster_config_lan_2node_e2e exceeded its hard ${TOTAL_BUDGET}s budget and was killed -- this is a gate defect (a hang), not a test failure to investigate case-by-case." >&2
fi
exit "$rc"
