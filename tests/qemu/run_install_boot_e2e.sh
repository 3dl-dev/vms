#!/bin/bash
# run_install_boot_e2e.sh - ctest/CI entry point for install_boot_e2e
# (vms-96ec). Same convention as run_product_install_e2e.sh (read that
# file's header): formats a blank target disk on the HOST with a host-built
# INITIALIZE.EXE, then runs tests/qemu/test_install_boot_e2e.sh INSIDE the
# ovmx-boot image, which PRODUCT INSTALLs the real OS kit onto that target
# and then BOOTS the target as its own system disk to a login prompt.
#
# Gated behind OVMX_QEMU_FULL_E2E=1 so a bare `ctest` never pays for a real
# docker+QEMU boot -- IDENTICAL to run_product_install_e2e.sh/run_upgrade_
# e2e.sh. CI does NOT rely on that opt-in as cover: .github/workflows/ci.yml
# runs this gate for real with OVMX_QEMU_FULL_E2E=1 and treats a SKIP
# (exit 77) as a hard failure, exactly like the upgrade-e2e job. The
# opt-in-in-ctest / enforced-in-CI split is the same one every qemu-full-boot
# gate uses; what this bead adds is the missing CI job that actually boots a
# /DESTINATION-installed target.
#
# The target disk is sized larger than product_install_e2e's (64 MB, not
# 16): unlike that test -- which only mounts the target as a data volume --
# this one BOOTS it, and a booting system writes (OVMXVMSSYS.PAR;2, login
# and operator logs, SYS$SCRATCH scratch), so the volume needs headroom
# beyond the kit payload itself.
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
    echo "SKIP: install_boot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_install_boot_e2e.sh"
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

# Locate a host-built INITIALIZE.EXE to format the blank target with.
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

echo "--- formatting the blank install target (label WORK, 64 MB) with $INIT_EXE ---"
"$INIT_EXE" "$WORKDIR/dka100.img" WORK 64 || { echo "FAIL: INITIALIZE of the target disk"; exit 1; }

exec docker run --rm \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-90}" \
    -e "RUN_TIMEOUT=${RUN_TIMEOUT:-90}" \
    -v "$REPO_ROOT/tests/qemu/test_install_boot_e2e.sh:/test.sh:ro" \
    -v "$WORKDIR:/work" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
