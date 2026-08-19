#!/bin/bash
# run_mount_e2e.sh - ctest entry point for mount_e2e (vms-651).
#
# THE GATE ITSELF (tests/qemu/test_mount_e2e.sh) drives a REAL docker + QEMU
# boot of the mastered bootable image, twice, against a SECOND, real virtio
# disk it formats on the HOST first. Same convention as
# tests/qemu/run_parts_demo_e2e.sh (read that file's header for the reasoning
# this one shares in full): registered with ctest so it is discoverable by
# name, but gated behind an explicit opt-in so a bare `ctest` run never pays
# for a real docker+QEMU boot it cannot use.
#
# THE ONE THING THIS SCRIPT DOES THAT run_parts_demo_e2e.sh DOES NOT: it
# formats the second disk BEFORE `docker run`, on the HOST, with the host's
# own INITIALIZE.EXE (the userspace mkfs, tools/vms_initialize.c) -- because
# the final "ovmx-boot" runner image (distro/Dockerfile.bootable's `runner`
# stage) ships only qemu + boot artifacts, not the build's host tools. A
# blank, unformatted disk fails to mount(2) as vmsfs (same rule STARTUP.EXE's
# own SYSDISK mount enforces), so the second disk has to be VMSFS-formatted
# before QEMU ever sees it, and that formatting has to happen outside the
# runner image. The formatted image is bind-mounted into the container at
# /work so test_mount_e2e.sh (running inside) can attach it as the guest's
# second virtio disk without needing any host tool of its own.
#
# Env knobs:
#   OVMX_QEMU_FULL_E2E     must be "1" or this script SKIPs (exit 77).
#   OVMX_BOOT_IMAGE        image tag to run (default: ovmx-boot).
#   OVMX_BUILD_BOOT_IMAGE  "1" to build OVMX_BOOT_IMAGE if it is not already
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
    echo "SKIP: mount_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot, real minutes)."
    echo "      Run directly: OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=$IMAGE tests/qemu/run_mount_e2e.sh"
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

# Locate a host-built INITIALIZE.EXE to format the second disk with.
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
# MOUNTFAIL). A real MOUNT target is an ODS-2 volume -- format one (vms-37e).
"$INIT_EXE" --ods2 "$WORKDIR/dka100.img" WORK 16 || { echo "FAIL: INITIALIZE of the second disk"; exit 1; }

exec docker run --rm \
    -e "BOOT_TIMEOUT=${BOOT_TIMEOUT:-90}" \
    -e "RUN_TIMEOUT=${RUN_TIMEOUT:-60}" \
    -v "$REPO_ROOT/tests/qemu/test_mount_e2e.sh:/test.sh:ro" \
    -v "$WORKDIR:/work" \
    --entrypoint bash \
    "$IMAGE" \
    /test.sh
