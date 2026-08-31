#!/bin/bash
# run_scs_autostart_e2e.sh - ctest/CI entry point for the SCS boot-autostart
# e2e (vms-5ad, child of vms-110b).
#
# Same shape as run_cluster_param_adoption_e2e.sh (read that file's header
# first): THE GATE ITSELF (tests/qemu/test_scs_autostart.sh) drives REAL
# docker + QEMU boots of the mastered bootable image -- three arms, five
# boots total (two power-cycles), so it takes proportionally longer than a
# single-boot gate. Registered with ctest but gated behind the same explicit
# opt-in (OVMX_QEMU_FULL_E2E=1) every full-boot QEMU gate uses, so a bare
# `ctest` SKIPs (exit 77).
#
# Env knobs (same defaults/meaning as run_cluster_param_adoption_e2e.sh):
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already present.
#   BOOT_TIMEOUT           forwarded to the gate itself.
#
# Usage:
#   docker build -t ovmx-boot -f distro/Dockerfile.bootable .
#   OVMX_QEMU_FULL_E2E=1 tests/qemu/run_scs_autostart_e2e.sh

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: scs_autostart_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU reboot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_scs_autostart_e2e.sh"
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

echo ""
echo "############################################################"
echo "# test_scs_autostart.sh"
echo "############################################################"
docker run --rm \
    --device=/dev/kvm \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}" \
    -v "$REPO_ROOT/tests/qemu/test_scs_autostart.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
