#!/bin/bash
# run_sysboot_cluster_params_e2e.sh - CI/ctest entry point for
# sysboot_cluster_params_e2e (vms-46c gap #2, conversational boot).
#
# Same shape as run_boot_scsnode_hostname_e2e.sh (read that file's header
# first): THE GATE ITSELF (tests/qemu/test_sysboot_cluster_params_e2e.sh)
# drives two real docker + QEMU boots of the mastered bootable image and takes
# real minutes. It requires the same explicit opt-in (OVMX_QEMU_FULL_E2E=1) and
# real infrastructure (docker; the bootable image, built here only if missing
# and OVMX_BUILD_BOOT_IMAGE=1) the other full-e2e gates use, and never skips
# silently in CI (the caller treats exit 77 as a hard failure).
#
# Env knobs (same defaults/meaning as run_boot_scsnode_hostname_e2e.sh):
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already present.
#   BOOT_TIMEOUT           forwarded to the gate itself.
#
# HARD OUTER BOUND: the gate bounds each QEMU invocation with `timeout`, but
# nothing bounds the SCRIPT; this wrapper is the backstop for the
# unknown-unknown hang class, the same way run_boot_scsnode_hostname_e2e.sh
# does: `timeout --kill-after=30 <budget>` wraps the ENTIRE docker run, and a
# named, unique container is force-removed on any exit path so a killed wrapper
# cannot leave an orphaned container running server-side.
#
# Usage:
#   docker build -t ovmx-boot -f distro/Dockerfile.bootable .
#   OVMX_QEMU_FULL_E2E=1 tests/qemu/run_sysboot_cluster_params_e2e.sh

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
# 2 boots, each up to BOOT_TIMEOUT, plus generous slack for the SYSBOOT> and
# DCL interaction (sends/waitfors well under BOOT_TIMEOUT each) and image I/O.
# Comfortably under the CI job's own 30-minute job timeout.
TOTAL_BUDGET=$(( BOOT_TIMEOUT * 2 + 300 ))
CONTAINER_NAME="ovmx-sysboot-clu-e2e-$$"
cleanup() { docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: sysboot_cluster_params_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_sysboot_cluster_params_e2e.sh"
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

timeout --kill-after=30 "$TOTAL_BUDGET" \
    docker run --rm --name "$CONTAINER_NAME" \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT}" \
    -v "$REPO_ROOT/tests/qemu/test_sysboot_cluster_params_e2e.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
rc=$?
if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    echo "FATAL: sysboot_cluster_params_e2e exceeded its hard ${TOTAL_BUDGET}s budget and was killed -- this is a gate defect (a hang), not a test failure to investigate case-by-case." >&2
fi
exit "$rc"
