#!/bin/bash
# run_job_control_console_e2e.sh - ctest entry point for job_control_console_e2e
# (vms-8d2).
#
# Same opt-in/SKIP convention as run_parts_demo_e2e.sh and
# run_boot_scsnode_hostname_e2e.sh: the gate itself
# (tests/qemu/test_job_control_console.sh) drives a REAL docker + QEMU boot
# of the mastered bootable image. It is registered with ctest (per this
# item's test-coverage requirement) but does NOT run under a bare `ctest`
# invocation -- see run_parts_demo_e2e.sh's header for the full reasoning.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if it is not already
#                          present (default: 0 - require a prebuilt image).
#   BOOT_TIMEOUT / CMD_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: job_control_console_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_job_control_console_e2e.sh"
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
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-60}" \
    -e "CMD_TIMEOUT=${CMD_TIMEOUT:-30}" \
    -v "$REPO_ROOT/tests/qemu/test_job_control_console.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
