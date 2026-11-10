#!/bin/bash
# test_tcpip_external_boot_e2e.sh (rd vms-d0c4) -- a booted OVMX reaches REAL
# servers OUTSIDE the VM over its NIC: TCPIP SET INTERFACE / SET ROUTE configure
# the address and the gateway, $ TELNET <ip> holds a session with a real telnetd,
# and $ FTP <ip> /GET then /PUT round-trips a file byte-identical with a real FTP
# server.
#
# Runs INSIDE the TCP/IP test-overlay bootable image (ovmx-boot-tcpip); the runner
# (run_tcpip_external_boot_e2e.sh) starts the servers in a sibling container
# whose network namespace this container joins, and passes:
#   SRV_IP   the server host's address (192.168.77.1) -- NOT on the guest's
#            link (10.0.3.0/24), so reached only through the default route
#   /srv     the FTP root the server serves (seed.bin), shared with the runner
#
# THE NETWORK -- a real link, no proxy. The guest's virtio NIC is a TAP device
# (tap0) in the SERVER container's network namespace; this container joins that
# namespace only to run QEMU. tap0 is 10.0.3.1/24 there; the servers listen on
# SRV_IP (192.168.77.1), an address of the server host that is NOT on the link,
# so the guest reaches it only through a default route via 10.0.3.1. Every
# packet is the guest kernel's own TCP/IP (driven by the executive BGn: socket)
# on the wire to the server host's kernel: nothing terminates or re-originates
# the connection in between (no QEMU user-mode NAT, no pump). The runner
# captures the link (tcpdump on tap0) and keeps the servers' own logs.
#
# WHAT IS ASSERTED (each a console line the guest printed, or the bytes on disk):
#   1. with the boot-time address (10.0.2.15, the wrong subnet for this link) and
#      again after SET INTERFACE but before SET ROUTE, TELNET SRV_IP fails
#      -SYSTEM-F-UNREACHABLE: the later success is the configuration's doing;
#   2. TCPIP SET INTERFACE SE0 /HOST=10.0.3.15 /NETWORK_MASK and SET ROUTE
#      /DEFAULT /GATEWAY=10.0.3.1 apply (SHOW INTERFACE / SHOW ROUTE show them);
#   3. TELNET SRV_IP: the real telnetd's shell computes a value only it can
#      ($((6*7))), the result comes back while the session is open (full duplex),
#      and the server's `exit` ends the session from the far side;
#   4. TELNET's escape character CTRL/] + QUIT ends a session from this side;
#   5. FTP /GET of a missing file fails honestly (the server's 550);
#   6. FTP /GET=seed.bin /OUTPUT=SEED.BIN stores 131072 bytes (256 blocks) on the
#      ODS-2 system disk through RMS, and FTP /PUT=SEED.BIN /REMOTE=back.bin sends
#      them back; the runner-side cmp of seed.bin and back.bin is byte-identical.
set -uo pipefail

: "${SRV_IP:?SRV_IP (the external server address) must be set by the runner}"
[ -f /srv/seed.bin ] || { echo "FATAL: /srv/seed.bin missing (runner did not share the FTP root)"; exit 1; }

BOOT_TIMEOUT="${BOOT_TIMEOUT:-600}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
[ -f "$INITRD" ] || INITRD=/boot/initramfs-ovmx-slim.cpio.gz
IMG=/boot/ovmx-distrib.img
for f in "$KERNEL" "$INITRD" "$IMG"; do
    [ -f "$f" ] || { echo "FATAL: missing boot artifact $f"; exit 1; }
done

DISK=/tmp/tcpip-ext.img
LOG=/tmp/tcpip-ext-console.log
FIFO=/tmp/tcpip-ext.in
rm -f "$DISK" "$LOG" "$FIFO"
cp "$IMG" "$DISK"
mkfifo "$FIFO"

ACCEL="-accel tcg"
[ -w /dev/kvm ] && ACCEL="-accel kvm -cpu host"

PASS=0; FAIL=0
record() { if [ "$2" -eq 0 ]; then echo "  PASS: $1"; PASS=$((PASS+1)); else echo "  FAIL: $1"; FAIL=$((FAIL+1)); fi; }
# occurrences of a fixed string in the console so far
count() { grep -aoF -- "$1" "$LOG" 2>/dev/null | wc -l; }
# wait until fixed string $1 occurs more than $2 times (default 0), up to $3 s
waitmore() { local pat="$1" base="${2:-0}" lim="${3:-60}" w=0
    while [ $w -lt $((lim*4)) ]; do
        [ "$(count "$pat")" -gt "$base" ] && return 0
        kill -0 "$qp" 2>/dev/null || return 1
        sleep 0.25; w=$((w+1))
    done; return 1; }
# run one DCL command line and wait for the next DCL prompt
PROMPT='$ '
dcl() { local before; before=$(count "$PROMPT"); send "$1"; waitmore "$PROMPT" "$before" "${2:-60}"; }

echo "=== TCP/IP to real servers outside the VM: SET INTERFACE/ROUTE, TELNET, FTP (vms-d0c4) ==="
echo "server=$SRV_IP gateway=10.0.3.1 seed=$(wc -c </srv/seed.bin) bytes"

# shellcheck disable=SC2086
timeout -k 15 "$BOOT_TIMEOUT" qemu-system-x86_64 \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -nographic -append "console=ttyS0 loglevel=3 quiet" \
    -m 512M -smp 2 $ACCEL \
    -netdev tap,id=net0,ifname=tap0,script=no,downscript=no \
    -device virtio-net-pci,netdev=net0,mac=52:54:00:0d:0c:04 \
    -nodefaults -serial stdio \
    -drive file="$DISK",format=raw,if=virtio,cache=writethrough \
    -no-reboot <"$FIFO" >"$LOG" 2>&1 &
qp=$!
exec 4>"$FIFO"
trap 'kill $qp 2>/dev/null' EXIT
send() { printf '%s\r' "$1" >&4; }
sendraw() { printf '%b' "$1" >&4; }

w=0; until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge 180 ]; do send ''; sleep 1; w=$((w+1)); done
if waitmore 'Username:' 0 180; then rc=0; else rc=1; fi
record "the TCP/IP image boots to login (virtio NIC on tap0)" "$rc"
[ "$rc" -eq 0 ] || { tail -40 "$LOG"; exit 1; }
send 'SYSTEM'; sleep 1
send 'MANAGER'
if waitmore 'Welcome to OpenVMX' 0 60 && waitmore "$PROMPT" 0 90; then rc=0; else rc=1; fi
record "SYSTEM logs in (OPER: may configure the network)" "$rc"
sleep 2

# --- 1. negative controls: no address on this link, then no route ------------
before=$(count 'UNREACHABLE')
dcl "TELNET $SRV_IP" 60
if waitmore '-SYSTEM-F-UNREACHABLE' "$before" 5; then rc=0; else rc=1; fi
record "with the boot-time 10.0.2.15 address: TELNET $SRV_IP fails -SYSTEM-F-UNREACHABLE" "$rc"

# --- 2. SET INTERFACE / SET ROUTE -----------------------------------------------
before=$(count 'interface configured')
dcl 'TCPIP SET INTERFACE SE0 /HOST=10.0.3.15 /NETWORK_MASK=255.255.255.0'
if waitmore '%TCPIP-I-INFO, interface configured' "$before" 5; then rc=0; else rc=1; fi
record "TCPIP SET INTERFACE SE0 /HOST=10.0.3.15 /NETWORK_MASK=255.255.255.0 applied" "$rc"
dcl 'TCPIP SHOW INTERFACE'
if grep -aE '^SE0 +10\.0\.3\.15 +255\.255\.255\.0 +[0-9]+ +Up' "$LOG" >/dev/null; then rc=0; else rc=1; fi
record "TCPIP SHOW INTERFACE: SE0 10.0.3.15 255.255.255.0 Up" "$rc"

before=$(count 'UNREACHABLE')
dcl "TELNET $SRV_IP" 60
if waitmore '-SYSTEM-F-UNREACHABLE' "$before" 5; then rc=0; else rc=1; fi
record "on the link but before SET ROUTE: TELNET $SRV_IP (off-link) still -SYSTEM-F-UNREACHABLE" "$rc"

dcl 'TCPIP SET ROUTE /DEFAULT /GATEWAY=10.0.3.1'
if waitmore '%TCPIP-I-INFO, route added' 0 5; then rc=0; else rc=1; fi
record "TCPIP SET ROUTE /DEFAULT /GATEWAY=10.0.3.1 applied to the live routing table" "$rc"
dcl 'TCPIP SHOW ROUTE'
if grep -aE '^default +10\.0\.3\.1 +0\.0\.0\.0 +SE0' "$LOG" >/dev/null; then rc=0; else rc=1; fi
record "TCPIP SHOW ROUTE: default via 10.0.3.1 on SE0" "$rc"

# --- 3. TELNET: a session with the real telnetd -----------------------------------
before_conn=$(count "Connected to $SRV_IP.")
send "TELNET $SRV_IP"
if waitmore "Connected to $SRV_IP." "$before_conn" 30; then rc=0; else rc=1; fi
record "TELNET $SRV_IP connects through the gateway" "$rc"
sleep 3                                 # the server's negotiation and shell start
# The marker only the remote shell can compute: 6*7 -> 42.
send 'echo OVMX-TN-$((6*7))-END'
if waitmore 'OVMX-TN-42-END' 0 30; then rc=0; else rc=1; fi
record "the remote shell's output (OVMX-TN-42-END, computed there) comes back during the session" "$rc"
before=$(count 'closed by the remote host')
send 'exit'
if waitmore 'closed by the remote host' "$before" 30; then rc=0; else rc=1; fi
record "the server's exit ends the session from the far side (%TELNET-I-SESSEND)" "$rc"
waitmore "$PROMPT" "$(( $(count "$PROMPT") - 1 ))" 30 >/dev/null
sleep 1

# --- 4. TELNET: CTRL/] + QUIT ends a session from this side -----------------------
before_conn=$(count "Connected to $SRV_IP.")
send "TELNET $SRV_IP"
waitmore "Connected to $SRV_IP." "$before_conn" 30 >/dev/null
sleep 3
before=$(count 'TELNET> ')
sendraw '\035'
if waitmore 'TELNET> ' "$before" 15; then rc=0; else rc=1; fi
record "CTRL/] gives the TELNET> escape prompt" "$rc"
before=$(count '%TELNET-I-SESSEND')
dcl 'QUIT' 30
if waitmore '%TELNET-I-SESSEND' "$before" 5; then rc=0; else rc=1; fi
record "QUIT at TELNET> closes the session from this side" "$rc"

# --- 5/6. FTP -----------------------------------------------------------------
before=$(count '%FTP-E-GETERR')
dcl "FTP $SRV_IP /GET=no-such-file.bin /OUTPUT=NOSUCH.BIN" 60
if waitmore '%FTP-E-GETERR' "$before" 5; then rc=0; else rc=1; fi
record "FTP /GET of a file the server does not have fails honestly (%FTP-E-GETERR)" "$rc"

SIZE=$(wc -c </srv/seed.bin)
dcl "FTP $SRV_IP /GET=seed.bin /OUTPUT=SEED.BIN" 120
if waitmore "%FTP-S-RETR, $SIZE bytes retrieved to SEED.BIN" 0 5; then rc=0; else rc=1; fi
record "FTP /GET=seed.bin: $SIZE bytes over a PASV data connection to $SRV_IP, stored to SEED.BIN" "$rc"
dcl 'DIRECTORY/SIZE SEED.BIN'
if grep -aE 'SEED\.BIN;1 +'"$((SIZE / 512))"'\b' "$LOG" >/dev/null; then rc=0; else rc=1; fi
record "DIRECTORY/SIZE: SEED.BIN;1 is $((SIZE / 512)) blocks on the ODS-2 system disk" "$rc"

dcl "FTP $SRV_IP /PUT=SEED.BIN /REMOTE=back.bin" 120
if waitmore "%FTP-S-STOR, $SIZE bytes stored to back.bin" 0 5; then rc=0; else rc=1; fi
record "FTP /PUT=SEED.BIN /REMOTE=back.bin: $SIZE bytes sent back" "$rc"

kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null; exec 4>&- 2>/dev/null

# keep the guest's console with the server-side evidence (the runner's /evidence)
[ -d /evidence ] && tr -d '\r' <"$LOG" >/evidence/ovmx-console.log 2>/dev/null

# the byte-identical round trip, checked on the SERVER's disk
if [ -f /srv/back.bin ] && cmp /srv/seed.bin /srv/back.bin; then rc=0; else rc=1; fi
record "back.bin on the server is byte-identical to seed.bin (GET then PUT round trip)" "$rc"

if [ "$FAIL" -ne 0 ]; then
    echo "--- console ---"; tr -d '\r' <"$LOG" | tail -150
fi
echo ""
echo "  RESULTS: $PASS/$((PASS+FAIL)) checks passed, $FAIL failed"
echo "=== test_tcpip_external_boot_e2e: $([ "$FAIL" = 0 ] && echo PASS || echo FAIL) ==="
[ "$FAIL" -eq 0 ]
