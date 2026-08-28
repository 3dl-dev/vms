#!/bin/bash
# run_dcl_acceptance_e2e.sh - ctest entry point for dcl_acceptance_e2e.
#
# THE GATE ITSELF (tests/qemu/test_dcl_acceptance_e2e.sh) boots the REAL
# runtime (the pre-mastered ODS-2 distribution disk baked into the ovmx-boot
# image), logs in SYSTEM/MANAGER, and RUNS THE BASIC DCL/SHOW COMMANDS A USER
# TYPES -- asserting VMS-faithful output for each, with a negative control.
# It is the missing acceptance gate: test_release_acceptance_e2e.sh only ever
# checked PRODUCT SHOW PRODUCT's version, so V0.5-2 shipped with SHOW USERS
# empty, SHOW DEVICE not Mounted, SHOW QUOTA fabricated, and WRITE F$xxx()
# printing the literal -- none caught.
#
# It needs a real docker + QEMU boot (real minutes), so -- exactly like
# run_parts_demo_e2e.sh / run_release_acceptance_e2e.sh -- it is registered
# with ctest for discoverability but gated behind OVMX_QEMU_FULL_E2E=1 so a
# bare `ctest` run never pays for a boot it cannot use.
#
# THE EXPECTED VALUES ARE SINGLE-SOURCED (INV-1): this script reads the boot
# banner (OVMX_PRODUCT_NAME + OVMX_PRODUCT_VERSION) and the VMS-compat version
# (OVMX_VMS_COMPAT_VERSION_X86_64) out of src/libvms/include/ovmx_identity.h
# and passes them to the gate -- never a literal here (the same pattern
# .github/workflows/ci.yml's executive-integral step uses for the banner).
#
# EXPECT THIS GATE TO FAIL today: the commands it asserts are the ones that
# shipped broken in V0.5-2; it goes green only as the in-flight fixes land.
# CI (.github/workflows/ci.yml's dcl-acceptance-e2e job) treats a SKIP (77) as
# a hard failure so it can never silently no-op on the release-cut path.
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
IDENTITY="$REPO_ROOT/src/libvms/include/ovmx_identity.h"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: dcl_acceptance_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_dcl_acceptance_e2e.sh"
    exit "$SKIP"
fi

command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

# INV-1 single source: derive the expected banner + compat version from
# ovmx_identity.h, never a literal in this script.
idval() { sed -n "s/^#define[[:space:]]\+$1[[:space:]]\+\"\([^\"]*\)\".*/\1/p" "$IDENTITY" | head -1; }
PRODUCT_NAME=$(idval OVMX_PRODUCT_NAME)
PRODUCT_VERSION=$(idval OVMX_PRODUCT_VERSION)
COMPAT_VERSION=$(idval OVMX_VMS_COMPAT_VERSION_X86_64)
[ -n "$PRODUCT_NAME" ] && [ -n "$PRODUCT_VERSION" ] || { echo "FATAL: could not read OVMX_PRODUCT_NAME/VERSION from $IDENTITY"; exit 1; }
[ -n "$COMPAT_VERSION" ] || { echo "FATAL: could not read OVMX_VMS_COMPAT_VERSION_X86_64 from $IDENTITY"; exit 1; }
EXPECTED_BOOT_BANNER="$PRODUCT_NAME $PRODUCT_VERSION"

echo "expected boot banner (from ovmx_identity.h): $EXPECTED_BOOT_BANNER"
echo "expected compat version (from ovmx_identity.h): $COMPAT_VERSION"

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
    -e "EXPECTED_BOOT_BANNER=${EXPECTED_BOOT_BANNER}" \
    -e "EXPECTED_COMPAT_VERSION=${COMPAT_VERSION}" \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}" \
    -e "CMD_TIMEOUT=${CMD_TIMEOUT:-30}" \
    -v "$REPO_ROOT/tests/qemu/test_dcl_acceptance_e2e.sh:/test.sh:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
