#!/bin/bash
# run_tcpip_external_boot_e2e.sh (rd vms-d0c4) -- a SEPARATE server host with real
# TELNET and FTP daemons, an Ethernet link (tap) between it and the OVMX VM, and
# the external-peer proof (test_tcpip_external_boot_e2e.sh) run against it.
#
# THE SERVER HOST is its own container (own network namespace, own processes):
# alpine's busybox telnetd and busybox ftpd (upstream daemons, unmodified, the
# distribution's package), listening on 192.168.77.1 -- an address of that host
# which is not on the link to the VM. The LINK is tap0, created in the server
# host's namespace with 10.0.3.1/24 (the VM's gateway); QEMU's virtio NIC is the
# other end. The boot container joins the server's namespace only so QEMU can
# open tap0. There is no QEMU user-mode NAT and no proxy: the VM's TCP segments
# go on the wire to the server host's kernel unchanged.
#
# EVIDENCE: tcpdump on tap0 (wire.pcap, wire.txt), the FTP daemon's own log
# (tcpsvd + ftpd -vv; busybox telnetd keeps none -- its session is in the pcap)
# and the guest console (ovmx-console.log) are written to the evidence directory, and the runner asserts
# from the capture that the TCP connections ran between the VM's address and the
# server's. OVMX_EVIDENCE_DIR keeps them (else they go with the scratch dir).
#
# Requires OVMX_QEMU_FULL_E2E=1 (a docker build + QEMU boot -- real minutes). The
# CI job MUST set it; a bare run SKIPs (77) rather than silently passing. Nothing
# is installed on the host; both containers and the scratch dir are removed.
set -u
SKIP=77

[ "${OVMX_QEMU_FULL_E2E:-0}" = "1" ] || {
    echo "SKIP: tcpip_external_boot_e2e requires OVMX_QEMU_FULL_E2E=1 (real docker+QEMU boot)."
    exit "$SKIP"
}
command -v docker >/dev/null 2>&1 || { echo "SKIP: docker not available"; exit "$SKIP"; }
[ -c /dev/net/tun ] || { echo "FATAL: /dev/net/tun is missing -- the VM's link cannot be made"; exit 1; }

REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
IMAGE="${OVMX_DAYTIME_IMAGE:-ovmx-boot-tcpip}"
SERVER_IMAGE="${OVMX_NETSRV_IMAGE:-alpine:3.20}"
SRV="ovmx-tcpip-ext-srv-$$"
SRV_IP=192.168.77.1
VM_IP=10.0.3.15

if ! docker image inspect "$IMAGE" >/dev/null 2>&1 || [ "${OVMX_BUILD_BOOT_IMAGE:-0}" = "1" ]; then
    echo "--- building TCP/IP test-overlay bootable image ($IMAGE, OVMX_TEST_ENABLE_TCPIP=1) ---"
    DOCKER_BUILDKIT=1 docker build -t "$IMAGE" \
        --build-arg OVMX_TEST_ENABLE_TCPIP=1 \
        -f "$REPO_ROOT/distro/Dockerfile.bootable" "$REPO_ROOT" \
        || { echo "FATAL: TCP/IP test-overlay image build failed"; exit 1; }
fi

ROOT=$(mktemp -d /tmp/ovmx-tcpip-ext.XXXXXX)
mkdir -p "$ROOT/srv" "$ROOT/evidence"
cleanup() {
    # the server wrote as its own root: hand the files back before removing
    docker exec "$SRV" sh -c "chmod -R a+rwX /srv /evidence" >/dev/null 2>&1
    docker rm -f "$SRV" >/dev/null 2>&1
    rm -rf "$ROOT"
}
trap cleanup EXIT

# The file the round trip carries: 256 blocks of random bytes (every byte value,
# 0xFF -- the TELNET IAC -- and CR/LF included), more than one 4096-byte $QIO.
head -c 131072 /dev/urandom >"$ROOT/srv/seed.bin"
chmod 644 "$ROOT/srv/seed.bin"
chmod 777 "$ROOT/srv" "$ROOT/evidence"

echo "--- starting the server host ($SERVER_IMAGE: busybox telnetd :23, ftpd :21 on $SRV_IP; link tap0 10.0.3.1/24) ---"
docker run -d --name "$SRV" --cap-add NET_ADMIN --cap-add NET_RAW --device /dev/net/tun \
    -v "$ROOT/srv:/srv" -v "$ROOT/evidence:/evidence" "$SERVER_IMAGE" sh -c "
        set -e
        apk add -q --no-cache busybox-extras tcpdump iproute2 >/evidence/apk.log 2>&1
        ip tuntap add dev tap0 mode tap
        ip addr add 10.0.3.1/24 dev tap0
        ip link set tap0 up
        ip addr add $SRV_IP/32 dev lo
        tcpdump -i tap0 -U -n -w /evidence/wire.pcap 'host $VM_IP' 2>/evidence/tcpdump.log &
        busybox-extras telnetd -F -b $SRV_IP -p 23 -l /bin/sh 2>>/evidence/telnetd.log &
        busybox-extras tcpsvd -vE $SRV_IP 21 busybox-extras ftpd -vv -w -a root /srv 2>>/evidence/ftpd.log &
        sleep 1; touch /evidence/ready
        wait" >/dev/null || { echo "FATAL: could not start the server host container"; exit 1; }
for i in $(seq 1 120); do
    [ -f "$ROOT/evidence/ready" ] && break
    docker inspect -f '{{.State.Running}}' "$SRV" 2>/dev/null | grep -q true \
        || { echo "FATAL: the server host exited during setup"; docker logs "$SRV"; cat "$ROOT/evidence/apk.log"; exit 1; }
    sleep 1
done
[ -f "$ROOT/evidence/ready" ] || { echo "FATAL: the server host never came up"; docker logs "$SRV"; exit 1; }
docker exec "$SRV" sh -c 'ip -br addr; netstat -ltn'

KVM_ARG=""
[ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"

echo "--- booting OVMX on tap0; TELNET/FTP to $SRV_IP through gateway 10.0.3.1 ---"
docker run --rm $KVM_ARG --network "container:$SRV" --cap-add NET_ADMIN --device /dev/net/tun \
    -e SRV_IP="$SRV_IP" -v "$ROOT/srv:/srv" -v "$ROOT/evidence:/evidence" \
    -v "$REPO_ROOT/tests/qemu/test_tcpip_external_boot_e2e.sh:/test.sh:ro" \
    --entrypoint bash "$IMAGE" /test.sh
rc=$?

# --- the wire and the server's own record ------------------------------------
sleep 1
docker exec "$SRV" sh -c 'pkill -INT tcpdump; sleep 1' >/dev/null 2>&1
docker exec "$SRV" sh -c "tcpdump -n -r /evidence/wire.pcap 2>/dev/null" >"$ROOT/evidence/wire.txt"
PASS=0; FAIL=0
check() { if [ "$2" -eq 0 ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
syn() { grep -E "IP $VM_IP\.[0-9]+ > $SRV_IP\.$1: Flags \[S\]" "$ROOT/evidence/wire.txt" >/dev/null; }
syn 23; check "wire: the VM ($VM_IP) opened TCP to $SRV_IP:23 on the link (SYN captured on tap0)" $?
syn 21; check "wire: the VM opened the FTP control connection to $SRV_IP:21" $?
grep -E "IP $VM_IP\.[0-9]+ > $SRV_IP\.([0-9]+): Flags \[S\]" "$ROOT/evidence/wire.txt" \
    | grep -vE "\.(21|23): Flags" >/dev/null
check "wire: the VM opened FTP PASV data connections to $SRV_IP (non-21/23 ports)" $?
grep -E "IP $SRV_IP\.(21|23) > $VM_IP\.[0-9]+: Flags \[P\.\]" "$ROOT/evidence/wire.txt" >/dev/null
check "wire: the server host sent data back to the VM" $?
grep -q "RETR seed.bin" "$ROOT/evidence/ftpd.log" && grep -q "STOR back.bin" "$ROOT/evidence/ftpd.log"
check "ftpd's own log records RETR seed.bin and STOR back.bin" $?
grep -qE "tcpsvd: start [0-9]+ $SRV_IP:21-$VM_IP:[0-9]+" "$ROOT/evidence/ftpd.log"
check "tcpsvd logs the VM's address ($VM_IP) as the FTP client" $?
echo "  wire: $(wc -l <"$ROOT/evidence/wire.txt") packets between $VM_IP and the server host"

if [ -n "${OVMX_EVIDENCE_DIR:-}" ]; then
    mkdir -p "$OVMX_EVIDENCE_DIR"
    docker exec "$SRV" sh -c "chmod -R a+rwX /evidence" >/dev/null 2>&1
    cp "$ROOT/evidence/wire.pcap" "$ROOT/evidence/wire.txt" "$ROOT/evidence/ftpd.log" \
       "$ROOT/evidence/ovmx-console.log" "$OVMX_EVIDENCE_DIR/" 2>/dev/null
    echo "  evidence kept in $OVMX_EVIDENCE_DIR"
fi
if [ "$rc" -ne 0 ] || [ "$FAIL" -ne 0 ]; then
    echo "--- ftpd log ---"; tail -40 "$ROOT/evidence/ftpd.log"
    echo "--- wire (head) ---"; head -40 "$ROOT/evidence/wire.txt"
    exit 1
fi
exit 0
