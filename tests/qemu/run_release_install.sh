#!/bin/bash
# run_release_install.sh - ctest/CI entry point for the vms-37f R1
# release-acceptance gate (tests/qemu/test_release_install.sh): media -> menu
# install -> SEPARATE container -> login -> PRODUCT SHOW.
#
# Same convention as run_install_boot_e2e.sh / run_install_menu.sh (read those
# headers): gated behind OVMX_QEMU_FULL_E2E=1 so a bare `ctest` never pays for a
# real docker+QEMU boot it cannot use, and CI does NOT rely on that opt-in as
# cover -- .github/workflows/ci.yml's release-install-e2e job runs it for real
# with OVMX_QEMU_FULL_E2E=1 and treats a SKIP (exit 77) as a hard failure.
#
# Unlike the sibling gates this one is a HOST-side orchestrator that fans out
# multiple SEPARATE `docker run`s (the container boundary is the whole point),
# so this wrapper does not itself `docker run`; it discovers the deps (image +
# a host-built INITIALIZE.EXE to format the blank target, exactly like
# run_install_menu.sh) and hands off to test_release_install.sh.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if not present.
#   OVMX_INITIALIZE_EXE    path to a host-built INITIALIZE.EXE.
#   BOOT_TIMEOUT / RUN_TIMEOUT   forwarded to the gate itself.

set -uo pipefail

SKIP=77
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"

if [ "${OVMX_QEMU_FULL_E2E:-0}" != "1" ]; then
    echo "SKIP: release_install requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boots across containers, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_release_install.sh"
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

# Host-built INITIALIZE.EXE to format the blank PRESERVE target (same discovery
# as run_install_menu.sh / run_install_boot_e2e.sh -- the menu's own INITIALIZE
# branch is a filed gap, so PRESERVE needs a pre-formatted volume).
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

exec env \
    OVMX_INITIALIZE_EXE="$INIT_EXE" \
    BOOT_TIMEOUT="${BOOT_TIMEOUT:-90}" \
    RUN_TIMEOUT="${RUN_TIMEOUT:-90}" \
    bash "$REPO_ROOT/tests/qemu/test_release_install.sh" "$IMAGE"
