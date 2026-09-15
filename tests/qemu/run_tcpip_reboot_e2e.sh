#!/bin/bash
# run_tcpip_reboot_e2e.sh (rd vms-b97) -- build/reuse the TCP/IP test-overlay
# bootable image (distro/Dockerfile.bootable --build-arg OVMX_TEST_ENABLE_TCPIP=1)
# and run the config-survives-reboot proof (test_tcpip_reboot_e2e.sh) INSIDE it.
# Proves TCP/IP config set before a reboot is reapplied at startup by
# TCPIP$REAPPLY.COM WITHOUT the persisted store growing (the apply/persist split,
# vms-b679, end to end on a real double boot).
#
# Requires OVMX_QEMU_FULL_E2E=1 (a real docker build + two QEMU boots + a writeback
# settle -- real minutes). The CI job MUST set it; a bare run SKIPs (77) rather
# than silently passing. Reuses the SAME image the daytime e2e builds
# (ovmx-boot-tcpip); the test-overlay build arg keeps the SHIPPED image
# byte-identical (the arg defaults to 0).
set -u
SKIP=77

[ "${OVMX_QEMU_FULL_E2E:-0}" = "1" ] || {
    echo "SKIP: tcpip_reboot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU double boot)."
    exit "$SKIP"
}
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_DAYTIME_IMAGE:-ovmx-boot-tcpip}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1 || [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
    echo "--- building TCP/IP test-overlay bootable image ($IMAGE, OVMX_TEST_ENABLE_TCPIP=1) ---"
    DOCKER_BUILDKIT=1 docker build -t "$IMAGE" \
        --build-arg OVMX_TEST_ENABLE_TCPIP=1 \
        -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
        || { echo "FATAL: TCP/IP test-overlay image build failed"; exit 1; }
fi

KVM_ARG=""
[ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"

echo "--- booting: set route -> reboot -> reapplied without store growth ---"
docker run --rm $KVM_ARG \
    -v "$REPO_ROOT/tests/qemu/test_tcpip_reboot_e2e.sh:/test.sh:ro" \
    --entrypoint bash "$IMAGE" /test.sh
