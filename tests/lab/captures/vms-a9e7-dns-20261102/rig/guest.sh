#!/bin/bash
# guest.sh (rd vms-a9e7) -- runs INSIDE the TCP/IP test-overlay bootable image
# (distro/Dockerfile.bootable --build-arg OVMX_TEST_ENABLE_TCPIP=1). Boots OVMX
# in QEMU on tap0 (a real link to the server host; see run.sh), logs in, and
# drives the DCL resolver against the server host's BIND:
#   1. with no name service configured, PING srvhost fails honestly (NORESOLVE);
#   2. TCPIP SET NAME_SERVICE /SERVER=192.168.77.1 /DOMAIN=lab.test;
#   3. PING srvhost      -> DNS A query for srvhost.lab.test, ICMP echo to the answer;
#   4. PING alias.lab.test -> the CNAME is followed to srvhost's A record;
#   5. PING nosuch       -> BIND's NXDOMAIN is reported as no such name;
#   6. TELNET srvhost    -> resolves, connects to the server host's telnetd, and the
#      (the interactive remote-shell data path is the TELNET client's own item, not this one's).
set -uo pipefail
: "${SRV_IP:?}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-900}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
[ -f "$INITRD" ] || INITRD=/boot/initramfs-ovmx-slim.cpio.gz
IMG=/boot/ovmx-distrib.img
for f in "$KERNEL" "$INITRD" "$IMG"; do [ -f "$f" ] || { echo "FATAL: missing $f"; exit 1; }; done
DISK=/tmp/dns.img; LOG=/tmp/dns-console.log; FIFO=/tmp/dns.in
rm -f "$DISK" "$LOG" "$FIFO"; cp "$IMG" "$DISK"; mkfifo "$FIFO"
ACCEL="-accel tcg"; [ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

PASS=0; FAIL=0
record() { if [ "$2" -eq 0 ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
count() { grep -aoF -- "$1" "$LOG" 2>/dev/null | wc -l; }
waitmore() { local pat="$1" base="${2:-0}" lim="${3:-60}" w=0
    while [ $w -lt $((lim*4)) ]; do
        [ "$(count "$pat")" -gt "$base" ] && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w+1))
    done; return 1; }
PROMPT='$ '
dcl() { local before; before=$(count "$PROMPT"); send "$1"; waitmore "$PROMPT" "$before" "${2:-60}"; }

echo "=== OVMX DCL resolver against a real BIND on a separate host (vms-a9e7) ==="
# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" qemu-system-x86_64 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "console=ttyS0 loglevel=3 quiet" \
    -m 512M -smp 2 $ACCEL \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:a9:e7:01 \
    -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
qp=$!
exec 4>"$FIFO"
trap 'kill $qp 2>/dev/null' EXIT
send() { printf '%s\r' "$1" >&4; }

w=0; until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge 300 ]; do send ''; sleep 1; w=$((w+1)); done
if waitmore 'Username:' 0 300; then rc=0; else rc=1; fi
record "OVMX boots to login (virtio NIC on tap0)" "$rc"
[ "$rc" -eq 0 ] || { tail -40 "$LOG"; exit 1; }
send 'SYSTEM'; sleep 1; send 'MANAGER'
if waitmore 'Welcome to OpenVMX' 0 60 && waitmore "$PROMPT" 0 90; then rc=0; else rc=1; fi
record "SYSTEM logs in" "$rc"
sleep 2

dcl 'TCPIP SET INTERFACE SE0 /HOST=10.0.3.15 /NETWORK_MASK=255.255.255.0'
waitmore '%TCPIP-I-INFO, interface configured' 0 5; record "SET INTERFACE SE0 10.0.3.15/24" $?
dcl 'TCPIP SET ROUTE /DEFAULT /GATEWAY=10.0.3.1'
waitmore '%TCPIP-I-INFO, route added' 0 5; record "SET ROUTE /DEFAULT /GATEWAY=10.0.3.1" $?

# 1. no name service yet
before=$(count 'no name service is')
dcl 'PING srvhost /COUNT=1' 60
waitmore 'no name service is' "$before" 5
record "before SET NAME_SERVICE: PING srvhost fails %PING-E-NORESOLVE (no name service configured)" $?

# 2. configure BIND on the server host
dcl "TCPIP SET NAME_SERVICE /SYSTEM /SERVER=$SRV_IP /DOMAIN=lab.test"
waitmore '%TCPIP-I-INFO, name service configured' 0 5
record "TCPIP SET NAME_SERVICE /SERVER=$SRV_IP /DOMAIN=lab.test" $?

# 3. unqualified name, qualified with /DOMAIN, answered by BIND
dcl 'PING srvhost /COUNT=2' 90
waitmore '56 bytes from srvhost: icmp_seq=1' 0 5
record "PING srvhost: resolved via BIND (srvhost.lab.test) and the echo came back" $?

# 4. CNAME followed
dcl 'PING alias.lab.test /COUNT=1' 90
waitmore '56 bytes from alias.lab.test: icmp_seq=1' 0 5
record "PING alias.lab.test: CNAME -> srvhost.lab.test followed to its A record" $?

# 5. NXDOMAIN
before=$(count 'reports no such name')
dcl 'PING nosuch /COUNT=1' 90
waitmore 'unknown host nosuch.lab.test - the name server reports no such name' "$before" 5
record "PING nosuch: BIND's NXDOMAIN reported as no such name (nosuch.lab.test)" $?

# 5b. a dead server: 192.168.77.2 is not an address of the server host, so the
# queries (routed via the gateway) are never answered; the resolver gives up
# honestly after its retries instead of hanging or inventing an address.
dcl "TCPIP SET NAME_SERVICE /SYSTEM /SERVER=192.168.77.2 /DOMAIN=lab.test"
waitmore '%TCPIP-I-INFO, name service configured' 1 5
record "TCPIP SET NAME_SERVICE /SERVER=192.168.77.2 (a dead server)" $?
before=$(count 'no reply from the name server')
dcl 'PING deadhost /COUNT=1' 120
waitmore 'cannot resolve deadhost.lab.test - no reply from the name server' "$before" 5
record "PING deadhost: a dead name server times out as 'no reply from the name server'" $?
dcl "TCPIP SET NAME_SERVICE /SYSTEM /SERVER=$SRV_IP /DOMAIN=lab.test"
sleep 1

# 6. TELNET by name
send 'TELNET srvhost'
waitmore 'Connected to srvhost.' 0 60
record "TELNET srvhost: resolved and connected to the server host's telnetd" $?
sleep 3
dcl 'exit' 30
waitmore '%TELNET-I-SESSEND, connection to srvhost closed' 0 10
record "TELNET session ends" $?

kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null
[ -d /evidence ] && tr -d '\r' <"$LOG" >/evidence/ovmx-console.log
[ "$FAIL" -ne 0 ] && { echo "--- console ---"; tr -d '\r' <"$LOG" | tail -120; }
echo "  RESULTS: $PASS/$((PASS+FAIL)) checks passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
