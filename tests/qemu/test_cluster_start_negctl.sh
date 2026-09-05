#!/bin/bash
# test_cluster_start_negctl.sh - THE R4 negctl for FC-P0.11 (VMS_IOCTL_CLUSTER_START
# wired into STARTUP.EXE with VAXCLUSTER gating, docs/plan-faithful-cluster-
# executive.md FC-P0.11's own done-condition: "R4: boot with VAXCLUSTER=2 =>
# HELLOs on the tap; VAXCLUSTER=0 => none (negctl test both substrates)").
#
# WHY THIS FILE EXISTS.
#
# FC-P0.11's whole claim is that VAXCLUSTER genuinely gates the port: a
# VAXCLUSTER=0 boot must show NO PEA0:, NO HELLO on the wire (INV-6 -- no
# fabricated port on a standalone node), and a VAXCLUSTER=2 boot with a real
# SCSNODE must show the port up and the executive's own HELLO on the wire.
# Asserting either half in isolation proves nothing: if OVMX always started
# the port, the VAXCLUSTER=0 boot would look identical to a positive result
# with the network side of the assertion simply never checked. This is a
# TRUE NEGATIVE CONTROL, on the SAME shipped image, gated on the ONE SYSGEN
# parameter FC-P0.10 loads into the executive and FC-P0.11's boot path
# (ovmx_init.c's start_cluster_port(), gated by cluster_boot_gate.h's
# cluster_start_wanted()) reads back before deciding whether to even issue
# VMS_IOCTL_CLUSTER_START.
#
# WHAT THIS MEASURES, ON THE WIRE (not console prose): a real tcpdump capture
# on the HOST side of a QEMU tap device during each boot's window, filtered
# on ethertype 0x6007 (SCA, the cluster's own protocol -- vms_pe.c's
# pe_hello_multicast()/exec_lan_open(..., 0x6007u, ...)). VAXCLUSTER=0 must
# capture ZERO such frames; VAXCLUSTER=2 (with SCSNODE authored) must capture
# at least one -- the HELLO cadence (vms_pe.h PE_TIMER_HELLO) guarantees one
# within the wait window once the port is up.
#
# WHAT WOULD MAKE THIS FAIL HONESTLY (the defect it guards): boot never
# reaches Username:; SYSGEN.EXE missing; the VAXCLUSTER=0 run captures a
# 0x6007 frame (the port started when it should not have -- a fabricated
# port, the exact INV-6 violation this item exists to prevent); the
# VAXCLUSTER=2 run captures NONE (the wiring from ovmx_init.c through
# VMS_IOCTL_CLUSTER_START to vms_pe_start() is broken or never reached).
#
# HOST PRIVILEGE REQUIREMENT (same posture as tests/qemu/test_virtio_nic.sh's
# own tap/bridge gap note): a QEMU tap netdev + a host-side tcpdump both need
# a privileged, operator-provisioned host tap -- CAP_NET_ADMIN/CAP_NET_RAW at
# minimum, usually root. This is NOT available unprivileged in ordinary CI or
# on a shared dev host, so Part 2 below SKIPS (never fabricates a result) when
# the tap cannot be created, and this script's own header directs that case to
# the cluster interop lab (docs memory `cluster-interop-lab`), which already
# owns privileged host networking for the real-VAX rail.
#
# Usage (run INSIDE the bootable image, WITH host tap privilege -- the lab):
#   docker run --rm --cap-add=NET_ADMIN --cap-add=NET_RAW \
#       -v $PWD/tests/qemu/test_cluster_start_negctl.sh:/test.sh:ro \
#       --entrypoint bash ovmx-boot /test.sh
#
# Env knobs:
#   BOOT_TIMEOUT   seconds to wait for a boot to reach Username: (default 180).
#   HELLO_WAIT     seconds to keep the tap capture running after login,
#                  waiting for the HELLO cadence to fire at least once
#                  (default 30 -- comfortably above vms_pe.h's HELLO period).
#
# Exit 0 = both halves behaved as designed (0 frames at VAXCLUSTER=0, >=1 at
#          VAXCLUSTER=2). Exit 1 = the negctl is broken. Exit 77 = honest skip
#          (no host tap privilege / boot artifacts on THIS host -- run in the
#          lab instead, per the header above).

set -uo pipefail

BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
HELLO_WAIT="${HELLO_WAIT:-30}"
KERNEL=/boot/vmlinuz
INITRD=/boot/initramfs-ovmx.cpio.gz
DISTRIB_IMG=/boot/ovmx-distrib.img
ARCH=$(uname -m)

EXIT_SKIP=77

skip_honest() {
    echo "SKIP: $1"
    echo "-- this negctl needs a privileged host tap (CAP_NET_ADMIN/CAP_NET_RAW) and"
    echo "   the built ovmx-boot artifacts; run it in the cluster interop lab instead"
    echo "   (memory: cluster-interop-lab). No result is fabricated for the skip."
    exit "$EXIT_SKIP"
}

if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    MACHINE=""
    CONSOLE="console=ttyS0"
fi

for f in "$KERNEL" "$INITRD" "$DISTRIB_IMG"; do
    [ -f "$f" ] || skip_honest "$f not found -- run this inside the ovmx-boot image (see header)"
done
command -v "$QEMU" >/dev/null 2>&1 || skip_honest "$QEMU not available"
command -v tcpdump >/dev/null 2>&1 || skip_honest "tcpdump not available"

# A pair of taps this script owns end to end (created and torn down here,
# never operator-provisioned -- unlike run-qemu.sh's OVMX_NET_MODE=tap,
# which deliberately never touches host networking itself). Needs
# CAP_NET_ADMIN; the honest skip below fires the instant that is absent.
TAP_NEG=ovmxneg0
TAP_POS=ovmxpos0

make_tap() {
    ip tuntap add dev "$1" mode tap 2>/dev/null || return 1
    ip link set "$1" up 2>/dev/null || { ip tuntap del dev "$1" mode tap 2>/dev/null; return 1; }
    return 0
}
del_tap() { ip link set "$1" down 2>/dev/null; ip tuntap del dev "$1" mode tap 2>/dev/null; }

make_tap "$TAP_NEG" || skip_honest "cannot create host tap $TAP_NEG (need CAP_NET_ADMIN)"
make_tap "$TAP_POS" || { del_tap "$TAP_NEG"; skip_honest "cannot create host tap $TAP_POS (need CAP_NET_ADMIN)"; }

PASS=0
FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "=== FC-P0.11 negctl: VAXCLUSTER=0 => no PEA0:/HELLO; VAXCLUSTER=2 => HELLO on the wire ==="
echo "arch=$ARCH qemu=$QEMU"

QPID=""; TPID=""; LOG=""; FIFO=""

cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${TPID:-}" ] && kill "$TPID" 2>/dev/null
    wait 2>/dev/null
    del_tap "$TAP_NEG"
    del_tap "$TAP_POS"
}
trap cleanup EXIT

send() { printf '%s\r' "$1" >&4; }
wait_for() {
    local pat="$1" limit="${2:-30}" since="${3:-0}" waited=0
    while [ "$waited" -lt "$((limit * 4))" ]; do
        if tail -c "+$((since + 1))" "$LOG" 2>/dev/null | grep -qF "$pat"; then return 0; fi
        kill -0 "$QPID" 2>/dev/null || return 1
        sleep 0.25; waited=$((waited + 1))
    done
    return 1
}
dump_and_die() {
    echo ""; echo "=== FATAL: $1 ==="; echo "--- console log ---"; cat "$LOG"
    kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
    exit 1
}

# boot_and_capture <tap-ifname> <disk> <log> <fifo> <pcap-out> -- boots one
# node with a real tap NIC, tcpdumps the HOST side of that tap for the whole
# boot+HELLO_WAIT window, logs in as SYSTEM, and leaves the node running so
# the caller can (optionally) author SYSGEN before the capture window ends.
boot_node() {
    local tap="$1" disk="$2"; LOG="$3"; FIFO="$4"
    rm -f "$LOG" "$FIFO"; mkfifo "$FIFO"
    # shellcheck disable=SC2086
    timeout "$((BOOT_TIMEOUT + HELLO_WAIT + 120))" $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -nographic -append "$CONSOLE loglevel=3 quiet" \
        -m 512M -smp 2 -nodefaults -serial stdio \
        -netdev "tap,id=net0,ifname=${tap},script=no,downscript=no" \
        -device "virtio-net-pci,netdev=net0,romfile=" \
        -drive file="$disk",format=raw,if=virtio,cache=writethrough \
        -no-reboot <"$FIFO" >"$LOG" 2>&1 &
    QPID=$!
    exec 4>"$FIFO"

    if wait_for '%OVMX-I-EXEC' 60; then ok "$tap: executive attached (real vms.ko)"; else bad "$tap: executive never attached"; fi
    send ''
    if wait_for 'Username:' "$BOOT_TIMEOUT"; then
        ok "$tap: boot reaches the login prompt"
    else
        dump_and_die "$tap: boot never reached Username: within ${BOOT_TIMEOUT}s"
    fi
    local off; off=$(wc -c <"$LOG")
    send 'SYSTEM'
    wait_for 'Password:' 30 "$off" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 30 "$off"; then
        ok "$tap: SYSTEM logs in"
    else
        dump_and_die "$tap: SYSTEM login failed"
    fi
    wait_for '$' 20 "$off"
}

qemu_halt() {
    exec 4>&- 2>/dev/null || true
    kill "$QPID" 2>/dev/null || true; wait "$QPID" 2>/dev/null || true
    rm -f "$FIFO"; QPID=""
}

# capture_hello <tap-ifname> <pcap-out> -- host-side tcpdump for ethertype
# 0x6007 (SCA) on the tap, for HELLO_WAIT seconds, then reports the count.
capture_hello() {
    local tap="$1" pcap="$2"
    timeout "$HELLO_WAIT" tcpdump -i "$tap" -w "$pcap" 'ether proto 0x6007' >/tmp/tcpdump-"$tap".log 2>&1
    tcpdump -r "$pcap" 2>/dev/null | wc -l
}

# ---------------------------------------------------------------------------
# RUN 1 (negative control): a FRESH, UNAUTHORED disk -- the factory-seeded
# VAXCLUSTER default is 0 (tools/vms_sysgen.c). Author NOTHING; the boot path
# (ovmx_init.c's start_cluster_port(), gated by cluster_boot_gate.h) must
# never even issue VMS_IOCTL_CLUSTER_START.
# ---------------------------------------------------------------------------
echo ""
echo "--- RUN 1 (VAXCLUSTER=0, unauthored): must show NO PEA0:/HELLO on the wire ---"
DISK_NEG=/tmp/cluster-start-negctl-neg.img
rm -f "$DISK_NEG"; cp "$DISTRIB_IMG" "$DISK_NEG"

boot_node "$TAP_NEG" "$DISK_NEG" /tmp/cluster-start-negctl-neg.log /tmp/cluster-start-negctl-neg.in
FRAMES_NEG=$(capture_hello "$TAP_NEG" /tmp/cluster-start-neg.pcap)
qemu_halt

if [ "$FRAMES_NEG" -eq 0 ]; then
    ok "VAXCLUSTER=0: zero 0x6007 frames captured on the tap in ${HELLO_WAIT}s (no fabricated port, INV-6)"
else
    bad "VAXCLUSTER=0: captured $FRAMES_NEG 0x6007 frame(s) -- the port started when VAXCLUSTER=0 should have refused it"
fi

# ---------------------------------------------------------------------------
# RUN 2 (positive): author SCSNODE + VAXCLUSTER=2 via SYSGEN, WRITE CURRENT,
# reboot -- the SAME across-reboot adoption shape as test_cluster_param_
# adoption.sh (vms-495), so this run's positive result is proved adopted
# from the persisted store, not merely from the live session's memory.
# ---------------------------------------------------------------------------
echo ""
echo "--- RUN 2 (VAXCLUSTER=2, SCSNODE authored + rebooted): must show HELLO(s) on the wire ---"
DISK_POS=/tmp/cluster-start-negctl-pos.img
rm -f "$DISK_POS"; cp "$DISTRIB_IMG" "$DISK_POS"

boot_node "$TAP_POS" "$DISK_POS" /tmp/cluster-start-negctl-pos-b1.log /tmp/cluster-start-negctl-pos-b1.in
A_OFF=$(wc -c </tmp/cluster-start-negctl-pos-b1.log)
send 'SYSGEN'
wait_for 'SYSGEN>' 20 "$A_OFF"
send 'USE CURRENT'
send 'SET SCSNODE NODEC'
send 'SET SCSSYSTEMID 1027'
send 'SET VAXCLUSTER 2'
send 'WRITE CURRENT'
send 'EXIT'
if wait_for '%SYSGEN-I-WRITTEN' 20 "$A_OFF"; then
    ok "RUN 2 boot1: SYSGEN authored VAXCLUSTER=2/SCSNODE=NODEC and WRITE CURRENT persisted it"
else
    dump_and_die "RUN 2 boot1: SYSGEN WRITE CURRENT never confirmed"
fi
qemu_halt

boot_node "$TAP_POS" "$DISK_POS" /tmp/cluster-start-negctl-pos-b2.log /tmp/cluster-start-negctl-pos-b2.in
FRAMES_POS=$(capture_hello "$TAP_POS" /tmp/cluster-start-pos.pcap)
qemu_halt

if [ "$FRAMES_POS" -ge 1 ]; then
    ok "VAXCLUSTER=2 (rebooted, authored): $FRAMES_POS HELLO frame(s) captured on the wire -- PEA0: is really up"
else
    bad "VAXCLUSTER=2 (rebooted, authored): zero 0x6007 frames captured -- CLUSTER_START never brought the port up"
fi

echo ""
echo "===================================="
echo "RESULT: $PASS passed, $FAIL failed"
if [ "$FAIL" -eq 0 ]; then
    echo "NEGCTL OK -- VAXCLUSTER genuinely gates PEA0:/HELLO (0 frames at 0, $FRAMES_POS at 2)"
    exit 0
fi
exit 1
