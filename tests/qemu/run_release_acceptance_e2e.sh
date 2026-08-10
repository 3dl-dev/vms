#!/bin/bash
# run_release_acceptance_e2e.sh - ctest entry point for release_acceptance_e2e
# (vms-a86f, epic vms-a84 RELEASE ENGINEERING). Same convention as
# run_upgrade_e2e.sh (read that file's header for the shared reasoning):
# registered with ctest so it is discoverable by name, gated behind an
# explicit opt-in so a bare `ctest` run never pays for a real docker+QEMU
# boot it cannot use.
#
# THIS TEST NEEDS A REAL RELEASE BUNDLE, CUT BY tools/cut-release.sh (vms-d73)
# -- it does not build or hand-fake one. Point OVMX_RELEASE_DIR at a
# prebuilt bundle directory (vmlinuz, initramfs-ovmx-slim.cpio.gz,
# ovmx-distrib.img, release-manifest.json), exactly like run_upgrade_e2e.sh
# requires OVMX_BASELINE_RELEASE_DIR/OVMX_UPGRADE_RELEASE_DIR prebuilt.
#
# THE VERSION EXPECTED BY THE GATE comes from the bundle's OWN
# release-manifest.json "product_version" field (an independent record
# tools/cut-release.sh wrote at cut time from the exact commit it archived)
# -- never a literal in this script. OVMX_EXPECTED_PRODUCT_VERSION_OVERRIDE
# exists ONLY to drive the negative-control demonstration: it substitutes a
# caller-supplied (deliberately wrong, for the control) value instead of the
# manifest's real one, so the SAME gate script can be shown reddening on a
# genuine version disagreement. See .github/workflows/ci.yml's
# release-acceptance-e2e job for how CI exercises both the real gate and the
# negative control.
#
# Local reproduction:
#   tools/cut-release.sh --out-dir /tmp/rel-acceptance
#   OVMX_QEMU_FULL_E2E=1 OVMX_BUILD_BOOT_IMAGE=1 \
#   OVMX_RELEASE_DIR=/tmp/rel-acceptance \
#       tests/qemu/run_release_acceptance_e2e.sh
#
#   # Negative control -- must exit nonzero:
#   OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=ovmx-boot \
#   OVMX_RELEASE_DIR=/tmp/rel-acceptance \
#   OVMX_EXPECTED_PRODUCT_VERSION_OVERRIDE=BOGUS-VERSION-DOES-NOT-EXIST \
#   OVMX_SKIP_PARTS_SCENARIO=1 \
#       tests/qemu/run_release_acceptance_e2e.sh; echo "rc=$?"
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E                    must be "1" or this script SKIPs
#                                          (exit 77).
#   OVMX_RELEASE_DIR                      path to a tools/cut-release.sh
#                                          bundle (the release artifact).
#   OVMX_EXPECTED_PRODUCT_VERSION_OVERRIDE  when set, used INSTEAD of the
#                                          bundle's release-manifest.json
#                                          product_version. Only for the
#                                          negative-control demonstration --
#                                          never set this for a real gate run.
#   OVMX_SKIP_PARTS_SCENARIO               "1" forwarded to the gate script
#                                          to skip the RMS-load scenario and
#                                          check only login + version
#                                          (default "0"; used to keep the
#                                          negative control cheap).
#   OVMX_BOOT_IMAGE                       image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE                 "1" to build OVMX_BOOT_IMAGE if not
#                                          already present (default: 0).
#                                          Only used for its qemu-system-x86_64
#                                          binary and general boot tooling --
#                                          this test boots the RELEASE_DIR
#                                          bundle's OWN kernel/initrd/
#                                          distrib.img, never the image's
#                                          baked-in ones.
#   BOOT_TIMEOUT / SETUP_TIMEOUT / RUN_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: release_acceptance_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      See this script's header for the cut-release + local reproduction recipe."
    exit "$SKIP"
fi

command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

RELEASE_DIR="${OVMX_RELEASE_DIR:-}"
if [ -z "$RELEASE_DIR" ]; then
    echo "SKIP: release_acceptance_e2e requires OVMX_RELEASE_DIR (a tools/cut-release.sh bundle)."
    echo "      See this script's header for the recipe."
    exit "$SKIP"
fi
for f in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img release-manifest.json; do
    [ -f "$RELEASE_DIR/$f" ] || { echo "FATAL: $RELEASE_DIR/$f missing -- is this a real tools/cut-release.sh output directory?"; exit 1; }
done

# THE VERSION THE GATE MUST SEE. Read from the cut's own manifest by default
# -- an independent record of what was actually cut, not a literal here.
MANIFEST_VERSION=$(sed -n 's/.*"product_version": "\([^"]*\)".*/\1/p' "$RELEASE_DIR/release-manifest.json" | head -1)
[ -n "$MANIFEST_VERSION" ] || { echo "FATAL: could not read product_version out of $RELEASE_DIR/release-manifest.json"; exit 1; }

if [ -n "${OVMX_EXPECTED_PRODUCT_VERSION_OVERRIDE:-}" ]; then
    EXPECTED_VERSION="$OVMX_EXPECTED_PRODUCT_VERSION_OVERRIDE"
    echo "NEGATIVE CONTROL: overriding the expected version to '$EXPECTED_VERSION' (cut actually recorded '$MANIFEST_VERSION') -- this run is EXPECTED to fail the version-match assertion."
else
    EXPECTED_VERSION="$MANIFEST_VERSION"
    echo "expected product version (from $RELEASE_DIR/release-manifest.json): $EXPECTED_VERSION"
fi

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
    -e "EXPECTED_PRODUCT_VERSION=${EXPECTED_VERSION}" \
    -e "SKIP_PARTS_SCENARIO=${OVMX_SKIP_PARTS_SCENARIO:-0}" \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-180}" \
    -e "SETUP_TIMEOUT=${SETUP_TIMEOUT:-60}" \
    -e "RUN_TIMEOUT=${RUN_TIMEOUT:-1500}" \
    -v "$REPO_ROOT/tests/qemu/test_release_acceptance_e2e.sh:/test.sh:ro" \
    -v "$RELEASE_DIR:/release:ro" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
