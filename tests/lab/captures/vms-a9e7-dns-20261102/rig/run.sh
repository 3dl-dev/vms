#!/bin/bash
# run.sh (rd vms-a9e7) -- the DNS interop rig: a SEPARATE server host running a
# real BIND 9 (alpine's "bind" package: upstream named, unmodified) authoritative
# for lab.test, plus busybox telnetd, on a real Ethernet link (tap) to a booted
# OVMX VM. The VM's DCL resolver queries BIND over a BGn: UDP channel; the link
# is captured with tcpdump and BIND logs every query with its client address.
#
# The server host is its own container (own network namespace and processes).
# tap0 lives in that namespace as 10.0.3.1/24 (the VM's gateway); BIND and
# telnetd listen on 192.168.77.1, an address of the server host that is NOT on
# the link, so the VM reaches them only through its default route. No QEMU
# user-mode NAT, no proxy, no stub: the query datagram the VM sends is what
# named receives. The boot container joins the server's namespace only so QEMU
# can open tap0 (the vms-d0c4 rig's topology).
#
# Usage: rig/run.sh <bootable-image> <evidence-dir>
#   bootable-image: built with
#     docker build -t <img> --build-arg OVMX_TEST_ENABLE_TCPIP=1 -f distro/Dockerfile.bootable .
set -u
IMAGE="${1:?bootable image}"; OUT="${2:?evidence dir}"
RIG=$(cd "$(dirname "$0")" && pwd)
SRV="ovmx-a9e7-dns-srv-$$"; SRV_IP=192.168.77.1; VM_IP=10.0.3.15
[ -c /dev/net/tun ] || { echo "FATAL: /dev/net/tun missing"; exit 1; }
ROOT=$(mktemp -d /tmp/ovmx-a9e7-dns.XXXXXX); mkdir -p "$ROOT/evidence"; chmod 777 "$ROOT/evidence"
cleanup() { docker exec "$SRV" sh -c "chmod -R a+rwX /evidence" >/dev/null 2>&1; docker rm -f "$SRV" >/dev/null 2>&1; rm -rf "$ROOT"; }
trap cleanup EXIT

docker run -d --name "$SRV" --cap-add NET_ADMIN --cap-add NET_RAW --device /dev/net/tun \
    -v "$RIG:/rig:ro" -v "$ROOT/evidence:/evidence" alpine:3.20 sh -c "
        set -e
        apk add -q --no-cache bind busybox-extras tcpdump iproute2 >/evidence/apk.log 2>&1
        ip tuntap add dev tap0 mode tap
        ip addr add 10.0.3.1/24 dev tap0
        ip link set tap0 up
        ip addr add $SRV_IP/32 dev lo
        named -V | head -1 >/evidence/named-version.txt
        tcpdump -i tap0 -U -n -w /evidence/wire.pcap 'host $VM_IP' 2>/evidence/tcpdump.log &
        named -g -c /rig/named.conf >/evidence/named.log 2>&1 &
        busybox-extras telnetd -F -b $SRV_IP -p 23 -l /bin/sh &
        sleep 2; touch /evidence/ready; wait" >/dev/null || { echo "FATAL: server host did not start"; exit 1; }
for i in $(seq 1 180); do
    [ -f "$ROOT/evidence/ready" ] && break
    docker inspect -f '{{.State.Running}}' "$SRV" | grep -q true || { docker logs "$SRV"; cat "$ROOT/evidence/apk.log"; exit 1; }
    sleep 1
done
docker exec "$SRV" sh -c 'cat /evidence/named-version.txt; ip -br addr; netstat -lnu'

KVM_ARG=""; [ -w /dev/kvm ] && KVM_ARG="--device /dev/kvm"
docker run --rm $KVM_ARG --network "container:$SRV" --cap-add NET_ADMIN --device /dev/net/tun \
    -e SRV_IP="$SRV_IP" -v "$ROOT/evidence:/evidence" -v "$RIG/guest.sh:/guest.sh:ro" \
    --entrypoint bash "$IMAGE" /guest.sh
rc=$?

sleep 1
docker exec "$SRV" sh -c 'pkill -TERM named; pkill -INT tcpdump; sleep 1; tcpdump -n -vv -r /evidence/wire.pcap > /evidence/wire.txt 2>/dev/null; chmod -R a+rwX /evidence'
E="$ROOT/evidence"; PASS=0; FAIL=0
check() { if [ "$2" -eq 0 ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
grep -qE "$VM_IP\.[0-9]+ > $SRV_IP\.53: .*A\? srvhost\.lab\.test\." "$E/wire.txt"; check "wire: the VM's A? srvhost.lab.test. query to $SRV_IP:53" $?
grep -qE "$SRV_IP\.53 > $VM_IP\.[0-9]+: .*srvhost\.lab\.test\. A $SRV_IP" "$E/wire.txt"; check "wire: BIND's answer srvhost.lab.test. A $SRV_IP" $?
grep -qE "alias\.lab\.test\. CNAME srvhost\.lab\.test\." "$E/wire.txt"; check "wire: BIND's CNAME answer for alias.lab.test" $?
grep -qE "$SRV_IP\.53 > $VM_IP\.[0-9]+: .*NXDomain" "$E/wire.txt"; check "wire: BIND's NXDomain for nosuch.lab.test" $?
[ "$(grep -cE "$VM_IP\.[0-9]+ > 192\.168\.77\.2\.53: .*A\? deadhost\.lab\.test\." "$E/wire.txt")" -ge 2 ]; check "wire: the VM's query to the dead server 192.168.77.2 (retried, never answered)" $?
! grep -qE "192\.168\.77\.2\.53 > " "$E/wire.txt"; check "wire: nothing ever answered from 192.168.77.2" $?
grep -qE "$VM_IP > $SRV_IP: ICMP echo request" "$E/wire.txt"; check "wire: ICMP echo to the resolved address" $?
grep -qE "$VM_IP\.[0-9]+ > $SRV_IP\.23: Flags \[S\]" "$E/wire.txt"; check "wire: TELNET SYN to the resolved address" $?
grep -qE "client .*$VM_IP#[0-9]+ \(srvhost\.lab\.test\): query: srvhost\.lab\.test IN A" "$E/named.log"; check "named's own log (querylog): srvhost.lab.test IN A from $VM_IP" $?
grep -qE "client .*$VM_IP#[0-9]+ \(nosuch\.lab\.test\): query: nosuch\.lab\.test IN A" "$E/named.log"; check "named's own log (querylog): nosuch.lab.test IN A from $VM_IP" $?

mkdir -p "$OUT"
cp "$E"/wire.pcap "$E"/wire.txt "$E"/named.log "$E"/named-version.txt "$E"/ovmx-console.log "$OUT"/ 2>/dev/null
echo "  evidence in $OUT"
[ "$rc" -eq 0 ] && [ "$FAIL" -eq 0 ] && echo "=== vms-a9e7 DNS interop: PASS ===" && exit 0
echo "=== vms-a9e7 DNS interop: FAIL (guest rc=$rc, wire/log FAIL=$FAIL) ==="; exit 1
