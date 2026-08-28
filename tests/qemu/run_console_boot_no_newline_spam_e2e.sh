#!/bin/bash
# run_console_boot_no_newline_spam_e2e.sh - ctest entry point for the vms-dec
# console newline-spam gate.
#
# THE GATE ITSELF (tests/qemu/test_console_boot_no_newline_spam.sh) boots the
# REAL runtime (the pre-mastered ODS-2 distribution disk baked into the
# ovmx-boot image) and, CRUCIALLY, types RETURNs DURING the boot -- before
# "Username:" is visible -- the way an operator mashing RETURN at a slow boot
# does. It then asserts the console shows no run of echoed blank lines
# ("newline spam") before the prompt, and that login still works.
#
# This is the coverage CI structurally lacked: every other boot harness waits
# for "Username:" and only THEN feeds input, so none ever provoked the echo
# that produced the operator-reported spam (vms-dec).
#
# It needs a real docker + QEMU boot (real minutes), so -- exactly like
# run_dcl_acceptance_e2e.sh -- it is registered with ctest for discoverability
# but gated behind OVMX_QEMU_FULL_E2E=1 so a bare `ctest` run never pays for a
# boot it cannot use.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not present (default 0).
#   BOOT_TIMEOUT / HAMMER_RETURNS / MAX_BLANK_RUN  forwarded to the gate.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: console_boot_no_newline_spam_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_console_boot_no_newline_spam_e2e.sh"
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
    -e "HAMMER_RETURNS=${HAMMER_RETURNS:-15}" \
    -e "MAX_BLANK_RUN=${MAX_BLANK_RUN:-3}" \
    -v "$REPO_ROOT/tests/qemu/test_console_boot_no_newline_spam.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
