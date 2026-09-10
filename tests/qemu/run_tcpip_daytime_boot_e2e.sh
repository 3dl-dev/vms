#!/bin/bash
# run_tcpip_daytime_boot_e2e.sh (rd vms-21b) -- build the daytime test-overlay
# bootable image (distro/Dockerfile.bootable --build-arg OVMX_TEST_ENABLE_TCPIP=1)
# and run the inbound daytime cold-boot proof (test_tcpip_daytime_boot_e2e.sh)
# INSIDE it. Proves the aux server launches a SYS$SYSTEM: service image (via the
# ACP-stage->execve mechanism, vms-21b) on a real cold boot, answering inbound.
#
# Requires OVMX_QEMU_FULL_E2E=1 (a real docker build + QEMU boot -- real minutes).
# The CI job / rail runner MUST set it; a bare run SKIPs (77) rather than silently
# passing. The test-overlay build arg keeps the SHIPPED image byte-identical (the
# arg defaults to 0); only this proof build enables TCP/IP auto-start.
set -u
SKIP=77

[ "${OVMX_QEMU_FULL_E2E:-0}" = "1" ] || {
    echo "SKIP: tcpip_daytime_boot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot)."
    exit "$SKIP"
}
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_DAYTIME_IMAGE:-ovmx-boot-tcpip}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1 || [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
    echo "--- building daytime test-overlay bootable image ($IMAGE, OVMX_TEST_ENABLE_TCPIP=1) ---"
    docker build -t "$IMAGE" \
        --build-arg OVMX_TEST_ENABLE_TCPIP=1 \
        -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
        || { echo "FATAL: daytime test-overlay image build failed"; exit 1; }
fi

KVM_ARG=""
[ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"

echo "--- booting + probing daytime (inbound :13 via hostfwd) ---"
docker run --rm $KVM_ARG \
    -v "$REPO_ROOT/tests/qemu/test_tcpip_daytime_boot_e2e.sh:/test.sh:ro" \
    --entrypoint bash "$IMAGE" /test.sh
