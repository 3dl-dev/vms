#!/bin/bash
# run_cluster_param_adoption_e2e.sh - ctest/CI entry point for the cluster-param
# ADOPTION-ON-REBOOT e2e (vms-495, epic vms-098 R1.3) and its paired MEASURED
# negative control.
#
# Same shape as run_cluster_params_recnx_e2e.sh (read that file's header first):
# THE GATES THEMSELVES (tests/qemu/test_cluster_param_adoption.sh and
# tests/qemu/test_cluster_param_adoption_negctl.sh) drive REAL docker + QEMU
# boots of the mastered bootable image -- and this pair power-CYCLES the disk
# (author on boot 1, adopt on boot 2), so it runs MORE boots than the single-boot
# gates and takes proportionally longer. Registered with ctest but gated behind
# the same explicit opt-in (OVMX_QEMU_FULL_E2E=1) every full-boot QEMU gate uses,
# so a bare `ctest` SKIPs (exit 77).
#
# Env knobs (same defaults/meaning as run_cluster_params_recnx_e2e.sh):
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already present.
#   BOOT_TIMEOUT           forwarded to the gates themselves.
#
# Usage:
#   docker build -t ovmx-boot -f distro/Dockerfile.bootable .
#   OVMX_QEMU_FULL_E2E=1 tests/qemu/run_cluster_param_adoption_e2e.sh

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: cluster_param_adoption_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU reboot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_cluster_param_adoption_e2e.sh"
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

run_one() {  # test-script-basename
    local script="$1"
    echo ""
    echo "############################################################"
    echo "# $script"
    echo "############################################################"
    docker run --rm \
        -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}" \
        -v "$REPO_ROOT/tests/qemu/$script:/test.sh:ro" \
        --entrypoint bash \
        "$IMAGE" \
        /test.sh
}

rc=0
# The MEASURED negative control first: if an unauthored rebooted disk yields
# NODEB/1026, the identity is not really coming from the persisted store, and
# the positive would pass for the wrong reason. Catch that before the positive.
run_one test_cluster_param_adoption_negctl.sh || rc=1
# The positive author-on-boot1 / adopt-on-boot2 round trip + control-disk bracket.
run_one test_cluster_param_adoption.sh || rc=1

if [ "$rc" -eq 0 ]; then
    echo ""
    echo "ALL cluster_param_adoption gates PASSED (negctl + positive)."
fi
exit "$rc"
