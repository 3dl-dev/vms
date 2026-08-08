#!/bin/bash
# run_parts_demo_e2e.sh - ctest entry point for parts_demo_e2e (vms-5dd).
#
# THE GATE ITSELF (tests/qemu/test_parts_demo_e2e.sh) drives a REAL docker +
# QEMU boot of the mastered bootable image and takes real minutes (a blank-
# disk install, then a ~10000-record RMS indexed-file load under QEMU TCG
# emulation - see that file's header for the budget). It is registered with
# ctest, per this item's requirement, but it does NOT run by default under a
# bare `ctest` invocation, for the same reason tests/qemu/CMakeLists.txt gives
# for not add_test()-ing the test_syssvc_* kernel-executive binaries: an entry
# that is expensive and only pays off in an environment nobody's plain `ctest`
# run actually is would either (a) silently balloon the ~20-minute host
# "Build & Test" CI job (which today does no docker build at all) past its
# time budget on every push to main and every nightly run, or (b) get
# excluded there and never actually exercised, which is the exact
# permanently-skipped-everywhere shape that file calls out as dishonest.
#
# The middle ground: real infrastructure availability gates it (docker; the
# bootable image, built here if missing and OVMX_BUILD_BOOT_IMAGE=1), AND it
# requires an explicit opt-in (OVMX_QEMU_FULL_E2E=1) that no existing CI job
# sets today. That makes this gate:
#   - discoverable and runnable by name (`ctest -R parts_demo_e2e`, per this
#     item's "wire it as a ctest gate" requirement);
#   - CAPABLE of a real pass/fail on any machine with docker (this one
#     included - it is not a permanent fake skip, unlike the vms.ko case);
#   - safe to leave registered without redesigning the CI job architecture,
#     which is outside this item's tests/qemu/** + distro/boot lane.
# Wiring a dedicated CI job that sets OVMX_QEMU_FULL_E2E=1 (the equivalent of
# the persistent-boot job's own docker build + `docker run .../test.sh` steps)
# is a separate, explicit decision for whoever owns .github/workflows/ci.yml.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if it is not already
#                          present (default: 0 - require a prebuilt image,
#                          same convention as test_parts_mastering.sh).
#   BOOT_TIMEOUT / SETUP_TIMEOUT / RUN_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: parts_demo_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_parts_demo_e2e.sh"
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
    -e "SETUP_TIMEOUT=${SETUP_TIMEOUT:-60}" \
    -e "RUN_TIMEOUT=${RUN_TIMEOUT:-1500}" \
    -v "$REPO_ROOT/tests/qemu/test_parts_demo_e2e.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
