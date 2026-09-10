#!/bin/bash
# run_ssh_boot_e2e.sh (rd vms-843a) -- build the SSH test-overlay bootable image
# (distro/Dockerfile.bootable --build-arg OVMX_TEST_ENABLE_SSH=1) and run the
# inbound SSH cold-boot proof (test_ssh_boot_e2e.sh) INSIDE it. Proves the aux
# server (TCPIP$INETD) binds :22 over BGn:, ACP-stages SYS$SYSTEM:VMSSSHD.EXE (the
# wrapped OpenSSH sshd, via the vms-21b ACP-stage->execve mechanism), and launches
# it in inetd mode on the accepted connection, so an inbound password login for a
# SYSUAF user lands an authenticated DCL session end-to-end (SSH -> SYSUAF -> DCL).
#
# Requires OVMX_QEMU_FULL_E2E=1 (a real docker build + QEMU boot -- real minutes).
# The CI job / rail runner MUST set it; a bare run SKIPs (77) rather than silently
# passing. The test-overlay build arg keeps the SHIPPED image unchanged (the arg
# defaults to 0 -- shipped ships no SSH); only this proof build enables SSH.
set -u
SKIP=77

[ "${OVMX_QEMU_FULL_E2E:-0}" = "1" ] || {
    echo "SKIP: ssh_boot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot)."
    exit "$SKIP"
}
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_SSH_IMAGE:-ovmx-boot-ssh}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1 || [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
    echo "--- building SSH test-overlay bootable image ($IMAGE, OVMX_TEST_ENABLE_SSH=1) ---"
    # BuildKit required: Dockerfile.bootable uses RUN --mount=type=secret (module
    # signing key). No secret is supplied here -> the Dockerfile's else branch
    # generates an ephemeral key (fine for a test-overlay image; the shipped
    # release image's reproducibility is a separate gate).
    DOCKER_BUILDKIT=1 docker build -t "$IMAGE" \
        --build-arg OVMX_TEST_ENABLE_SSH=1 \
        -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
        || { echo "FATAL: SSH test-overlay image build failed"; exit 1; }
fi

KVM_ARG=""
[ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"

echo "--- booting + probing SSH (inbound :22 via hostfwd) ---"
docker run --rm $KVM_ARG \
    -v "$REPO_ROOT/tests/qemu/test_ssh_boot_e2e.sh:/test.sh:ro" \
    --entrypoint bash "$IMAGE" /test.sh
