#!/bin/bash
# run_decnet_startnet_boot_e2e.sh (rd vms-1f69) -- run the boot-time STARTNET /
# running-NETACP / hellos-on-the-wire proof (test_decnet_startnet_boot_e2e.sh)
# inside the shipped bootable image. Requires OVMX_QEMU_FULL_E2E=1 (two real QEMU
# boots + a writeback settle); a bare run SKIPs (77) -- CI treats 77 as failure.
set -u
SKIP=77
[ "${OVMX_QEMU_FULL_E2E:-0}" = "1" ] || {
    echo "SKIP: decnet_startnet_boot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU double boot)."
    exit "$SKIP"
}
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_BOOT_IMAGE:-ovmx-boot}"
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    if [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
        DOCKER_BUILDKIT=1 docker build -t "$IMAGE" -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
            || { echo "FATAL: bootable image build failed"; exit 1; }
    else
        echo "FATAL: image '$IMAGE' not found (build it, or OVMX_BUILD_BOOT_IMAGE=1)"; exit 1
    fi
fi
KVM_ARG=""
[ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"
docker run --rm $KVM_ARG \
    -v "$REPO_ROOT/tests/qemu/test_decnet_startnet_boot_e2e.sh:/test.sh:ro" \
    --entrypoint bash "$IMAGE" /test.sh
