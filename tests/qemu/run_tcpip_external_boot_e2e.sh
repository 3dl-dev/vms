#!/bin/bash
# run_tcpip_external_boot_e2e.sh (rd vms-d0c4) -- start REAL TELNET and FTP
# servers outside the VM and run the external-peer proof
# (test_tcpip_external_boot_e2e.sh) in the TCP/IP test-overlay bootable image.
#
# The servers are busybox telnetd and busybox ftpd (upstream programs, unmodified)
# in their own container, pinned by tag. The boot container joins that
# container's network namespace (--network container:...), so QEMU's user-mode
# gateway opens the guest's connections to the server container's address --
# an address off the guest's 10.0.2.0/24, reached only through the default route
# the test sets. Nothing is installed on the host; both containers and the
# scratch FTP root are removed on exit.
#
# Requires OVMX_QEMU_FULL_E2E=1 (a docker build + QEMU boot -- real minutes). The
# CI job MUST set it; a bare run SKIPs (77) rather than silently passing. Reuses
# the image the daytime / reboot proofs build (ovmx-boot-tcpip).
set -u
SKIP=77

[ "${OVMX_QEMU_FULL_E2E:-0}" = "1" ] || {
    echo "SKIP: tcpip_external_boot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot)."
    exit "$SKIP"
}
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_DAYTIME_IMAGE:-ovmx-boot-tcpip}"
SERVER_IMAGE="${OVMX_NETSRV_IMAGE:-busybox:1.36.1}"
SRV="ovmx-tcpip-ext-srv-$$"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1 || [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
    echo "--- building TCP/IP test-overlay bootable image ($IMAGE, OVMX_TEST_ENABLE_TCPIP=1) ---"
    DOCKER_BUILDKIT=1 docker build -t "$IMAGE" \
        --build-arg OVMX_TEST_ENABLE_TCPIP=1 \
        -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
        || { echo "FATAL: TCP/IP test-overlay image build failed"; exit 1; }
fi
docker image inspect "$SERVER_IMAGE" >/dev/null 2>&1 || docker pull -q "$SERVER_IMAGE" \
    || { echo "FATAL: cannot pull the server image $SERVER_IMAGE"; exit 1; }

ROOT=$(mktemp -d /tmp/ovmx-tcpip-ext.XXXXXX)
cleanup() {
    # the server wrote back.bin as its own user: remove it from inside
    docker exec "$SRV" rm -f /srv/back.bin >/dev/null 2>&1
    docker rm -f "$SRV" >/dev/null 2>&1
    rm -rf "$ROOT"
}
trap cleanup EXIT

# The file the round trip carries: 256 blocks of random bytes (every byte value,
# 0xFF -- the TELNET IAC -- and CR/LF included), more than one 4096-byte $QIO.
head -c 131072 /dev/urandom >"$ROOT/seed.bin"
chmod 644 "$ROOT/seed.bin"
chmod 777 "$ROOT"                       # ftpd (root in its container) writes back.bin

echo "--- starting the external servers ($SERVER_IMAGE: telnetd :23, ftpd :21) ---"
docker run -d --name "$SRV" -v "$ROOT:/srv" "$SERVER_IMAGE" sh -c \
    'telnetd -F -p 23 -l /bin/sh & tcpsvd -vE 0.0.0.0 21 ftpd -w -a root /srv & wait' >/dev/null \
    || { echo "FATAL: could not start the server container"; exit 1; }
SRV_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SRV")
[ -n "$SRV_IP" ] || { echo "FATAL: the server container has no address"; exit 1; }
for i in $(seq 1 20); do
    docker exec "$SRV" sh -c 'netstat -ltn | grep -q ":21 " && netstat -ltn | grep -q ":23 "' && break
    sleep 0.5
done

KVM_ARG=""
[ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"

echo "--- booting OVMX; TELNET/FTP to $SRV_IP through the guest's gateway ---"
docker run --rm $KVM_ARG --network "container:$SRV" -e SRV_IP="$SRV_IP" \
    -v "$ROOT:/srv" \
    -v "$REPO_ROOT/tests/qemu/test_tcpip_external_boot_e2e.sh:/test.sh:ro" \
    --entrypoint bash "$IMAGE" /test.sh
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "--- server log ---"; docker logs "$SRV" 2>&1 | tail -30
fi
exit "$rc"
