#!/bin/bash
# run_cluster_param_adoption_e2e.sh - ctest/CI entry point for the cluster-param
# ACROSS-REBOOT adoption R1 release proof (vms-495, epic vms-098 R1.3) and its
# paired MEASURED negative control.
#
# Same shape as run_cluster_params_recnx_e2e.sh (read that file's header first):
# THE GATES THEMSELVES (tests/qemu/test_cluster_param_adoption.sh and
# tests/qemu/test_cluster_param_adoption_negctl.sh) drive REAL docker + QEMU
# boots of the mastered bootable image and take real minutes -- the positive
# does FOUR boots (author + reboot, twice, for the positive and the control
# bracket). Registered with ctest but gated behind the same explicit opt-in
# (OVMX_QEMU_FULL_E2E=1) every full-boot QEMU gate uses, so a bare `ctest`
# SKIPs (exit 77).
#
# Env knobs (same defaults/meaning as run_cluster_params_recnx_e2e.sh):
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already present.
#   BOOT_TIMEOUT           forwarded to the gates themselves (default 180).
#   SETTLE_SECS            forwarded to the positive gate (default 60).
#
# HARD OUTER BOUND: each gate bounds its own QEMU invocations with `timeout`, but
# nothing bounds the docker run, so each `run_one` is wrapped in
# `timeout --kill-after=30` and given a unique, named container force-removed on
# any exit path (the run_sysboot_cluster_params_e2e.sh backstop for the
# unknown-unknown hang class). The positive's budget covers four boots plus two
# writeback settles; the negctl's covers one boot.
#
# Usage:
#   docker build -t ovmx-boot -f distro/Dockerfile.bootable .
#   OVMX_QEMU_FULL_E2E=1 tests/qemu/run_cluster_param_adoption_e2e.sh

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
SETTLE_SECS="${SETTLE_SECS:-60}"

CONTAINER_PREFIX="ovmx-clu-adopt-e2e-$$"
cleanup() {
    docker kill "${CONTAINER_PREFIX}-neg" >/dev/null 2>&1 || true
    docker kill "${CONTAINER_PREFIX}-pos" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: cluster_param_adoption_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
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

# run_one  container-suffix  test-script-basename  hard-budget-seconds
run_one() {
    local suffix="$1" script="$2" budget="$3" rc
    echo ""
    echo "############################################################"
    echo "# $script"
    echo "############################################################"
    timeout --kill-after=30 "$budget" \
        docker run --rm --name "${CONTAINER_PREFIX}-${suffix}" \
        -e "BOOT_TIMEOUT=${BOOT_TIMEOUT}" \
        -e "SETTLE_SECS=${SETTLE_SECS}" \
        -v "$REPO_ROOT/tests/qemu/$script:/test.sh:ro" \
        --entrypoint bash \
        "$IMAGE" \
        /test.sh
    rc=$?
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        echo "FATAL: $script exceeded its hard ${budget}s budget and was killed -- a gate hang, not a per-case failure." >&2
    fi
    return "$rc"
}

rc=0
# The MEASURED negative control first: if the store->scsd read path is broken
# such that the identity is not really read from the store, this catches it
# before the positive can pass for the wrong reason. One boot.
run_one neg test_cluster_param_adoption_negctl.sh "$(( BOOT_TIMEOUT + 300 ))" || rc=1
# The positive across-reboot proof: four boots (author+reboot, positive and
# control bracket) plus two writeback settles.
run_one pos test_cluster_param_adoption.sh "$(( BOOT_TIMEOUT * 4 + SETTLE_SECS * 2 + 600 ))" || rc=1

if [ "$rc" -eq 0 ]; then
    echo ""
    echo "ALL cluster_param_adoption gates PASSED (negctl + positive across-reboot)."
fi
exit "$rc"
