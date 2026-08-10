#!/bin/bash
# run_boot_scsnode_hostname_e2e.sh - ctest entry point for
# boot_scsnode_hostname_e2e (vms-b6a7).
#
# Same shape as run_sysgen_versioning_e2e.sh (read that file's header first):
# THE GATE ITSELF (tests/qemu/test_boot_scsnode_hostname_e2e.sh) drives TWO
# real docker + QEMU boots per case (four boots total) of the mastered
# bootable image and takes real minutes. It is registered with ctest but
# does NOT run under a bare `ctest` invocation -- real infrastructure
# availability gates it (docker; the bootable image, built here if missing
# and OVMX_BUILD_BOOT_IMAGE=1), AND it requires the same explicit opt-in
# (OVMX_QEMU_FULL_E2E=1) parts_demo_e2e and sysgen_versioning_e2e use.
#
# Env knobs (same defaults/meaning as run_sysgen_versioning_e2e.sh):
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already present.
#   BOOT_TIMEOUT           forwarded to the gate itself.
#   SETTLE_SECS            forwarded to the gate itself (writeback settle).
#
# Usage:
#   docker build -t ovmx-boot -f distro/Dockerfile.bootable .
#   OVMX_QEMU_FULL_E2E=1 tests/qemu/run_boot_scsnode_hostname_e2e.sh

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: boot_scsnode_hostname_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_boot_scsnode_hostname_e2e.sh"
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

exec docker run --rm \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}" \
    -e "SETTLE_SECS=${SETTLE_SECS:-60}" \
    -v "$REPO_ROOT/tests/qemu/test_boot_scsnode_hostname_e2e.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
