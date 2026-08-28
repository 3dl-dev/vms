#!/bin/bash
# run_run_status_e2e.sh - ctest entry point for run_status_e2e (vms-707).
#
# THE GATE ITSELF (tests/qemu/test_run_status_e2e.sh) boots the REAL runtime (the
# pre-mastered ODS-2 distribution disk baked into the ovmx-boot image), logs in
# SYSTEM/MANAGER, RUNs SYS$SYSTEM:RC3.EXE, and asserts the reworked DCL RUN fork
# path (waitid(WNOWAIT) peek -> executive $STATUS readback by Linux pid -> reap)
# still, on the real runtime: (1) routes the activated image's stdout to the
# console, and (2) reports the image's completion status (SS$_NORMAL for RC3's
# clean exit) through the new readback path -- never a hang, spurious error, or
# wrong/absent status.
#
# It needs a real docker + QEMU boot (real minutes), so -- exactly like
# run_dcl_acceptance_e2e.sh -- it is registered with ctest for discoverability
# but gated behind OVMX_QEMU_FULL_E2E=1 so a bare `ctest` run never pays for a
# boot it cannot use. CI treats a SKIP (77) on the release-cut path as a hard
# failure so it can never silently no-op.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not present (default 0).
#   BOOT_TIMEOUT / CMD_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: run_status_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_run_status_e2e.sh"
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
    -e "CMD_TIMEOUT=${CMD_TIMEOUT:-30}" \
    -v "$REPO_ROOT/tests/qemu/test_run_status_e2e.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
