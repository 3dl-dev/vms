#!/bin/bash
# run_install_menu.sh - ctest entry point for the install menu e2e
# (vms-dcf). Same convention as run_product_install_e2e.sh (read that
# file's header for the shared reasoning): registered with ctest so it is
# discoverable by name, gated behind an explicit opt-in so a bare `ctest`
# run never pays for a real docker+QEMU boot it cannot use.
#
# Formats the second disk on the HOST first, exactly like
# run_product_install_e2e.sh does and for the same reason: OVMX's own
# INITIALIZE DCL verb does not yet resolve a VMS device name to its
# backing block device (see test_install_menu.sh's header), so this test
# proves the menu's PRESERVE branch (the oracle's own bracketed default),
# which needs the target already formatted.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not already
#                          present (default: 0 - require a prebuilt image).
#   OVMX_INITIALIZE_EXE    path to a host-built INITIALIZE.EXE (default:
#                          searches ../../build*/bin/INITIALIZE.EXE relative
#                          to this script).
#   BOOT_TIMEOUT / RUN_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: install_menu_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_install_menu.sh"
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

INIT_EXE="${OVMX_INITIALIZE_EXE:-}"
if [ -z "$INIT_EXE" ]; then
    for candidate in "$REPO_ROOT"/build/bin/INITIALIZE.EXE "$REPO_ROOT"/build-static/bin/INITIALIZE.EXE; do
        if [ -x "$candidate" ]; then INIT_EXE="$candidate"; break; fi
    done
fi
if [ -z "$INIT_EXE" ] || [ ! -x "$INIT_EXE" ]; then
    echo "SKIP: no host-built INITIALIZE.EXE found (build one: cmake -B build -DBUILD_TOOLS=ON && cmake --build build)."
    echo "      Set OVMX_INITIALIZE_EXE=/path/to/INITIALIZE.EXE to point at one directly."
    exit "$SKIP"
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

echo "--- formatting the second disk (DKA100:, label WORK) with $INIT_EXE ---"
# --ods2: the runtime MOUNTs a GENUINE ODS-2 volume over the ACP (atomic flip,
# vms-208); INITIALIZE's default legacy vmsfs layout is refused (%OVMX-F-
# MOUNTFAIL). A real install target is an ODS-2 volume -- format one (vms-37e).
"$INIT_EXE" --ods2 "$WORKDIR/dka100.img" WORK 16 || { echo "FAIL: INITIALIZE of the second disk"; exit 1; }

exec docker run --rm \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-90}" \
    -e "RUN_TIMEOUT=${RUN_TIMEOUT:-90}" \
    -v "$REPO_ROOT/tests/qemu/test_install_menu.sh:/test.sh:ro" \
    -v "$WORKDIR:/work" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
