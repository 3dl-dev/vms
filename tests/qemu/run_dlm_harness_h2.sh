#!/bin/bash
# run_dlm_harness_h2.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h2): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- with the join sequencer ON, and verdict the FULL
# VMS$VAXcluster JOIN (rd vms-4bd0).
#
# H2 PASS iff BOTH nodes reached VMS$VAXcluster SYSAP connection OPEN, i.e. each
# node's own SCSD log carries SCSD-I-VAXCLMEMBER. This mirrors the success oracle
# of tests/cluster/two-ovmx/verdict.sh (both nodes reach VMS$VAXcluster OPEN),
# but here each node runs on a REAL executive inside QEMU rather than as a Docker
# userspace daemon. H2 is JOIN/membership ONLY -- it does NOT drive a cross-node
# $ENQ into the executive (H3) or assert any DLM grant (H4).
#
# A per-node pcap of the full 0x6007 join exchange is reconstructed from each
# guest's base64 UART dump as the artifact.

set -uo pipefail

# The full join (multicast HELLO -> directed HELLO -> 0x41 START/VC OPEN -> the
# 8-step SCS$DIRECTORY/MSCP$DISK/VMS$VAXcluster sequencer -> 0x5b accept) needs
# the daemons to run long enough to complete the stop-and-wait choreography with
# retransmits. The Docker two-ovmx harness completes it at DURATION=90; match it.
DURATION="${H2_DURATION:-90}"
NETDEV="${H2_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H2_WALL_TIMEOUT:-600}"
OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

KERNEL=/boot/vmlinuz
INITRD=/initramfs.cpio.gz
ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    MACHINE="-machine virt -cpu cortex-a57"
    CONSOLE="console=ttyAMA0"
else
    QEMU=qemu-system-x86_64
    if [ -w /dev/kvm ]; then MACHINE="-accel kvm -cpu host"; else MACHINE="-accel tcg"; fi
    CONSOLE="console=ttyS0"
fi

# Distinct HW MACs are load-bearing: they are how each node tells the peer's
# frames from its own looped-back beacon, and the join sequencer keys per-peer
# state off the source MAC.
MAC_A=52:54:00:00:00:0a
MAC_B=52:54:00:00:00:0b
GROUP=230.0.0.7
PORT=16007

echo "=== OVMX DLM Harness H2 Runner (vms-4bd0) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1 (membership only; no DLM \$ENQ)"
echo ""

netdev_arg() {
    case "$NETDEV" in
        mcast)    echo "socket,id=net0,mcast=${GROUP}:${PORT},localaddr=127.0.0.1" ;;
        sockpair) if [ "$1" = "A" ]; then echo "socket,id=net0,listen=127.0.0.1:${PORT}";
                  else echo "socket,id=net0,connect=127.0.0.1:${PORT}"; fi ;;
        *)        echo "UNKNOWN" ;;
    esac
}

# Launch a guest as a REAL background child of THIS shell (never inside $() --
# that would reparent it and make `wait` a no-op). PID -> global LAUNCH_PID.
LAUNCH_PID=0
launch_node() {
    local role="$1" mac="$2" node="$3"
    local nd; nd=$(netdev_arg "$role")
    if [ "$nd" = "UNKNOWN" ]; then
        echo "FATAL: unknown H2_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
    fi
    # ttyS0=console(file), ttyS1=node verdict log(file), ttyS2=pcap-b64(file).
    # -m 512M matches the real-executive image (tests/qemu/Dockerfile).
    $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "$CONSOLE net.ifnames=0 biosdevname=0 panic=-1 loglevel=4 ovmx.node=${node} ovmx.duration=${DURATION}" \
        -m 512M -smp 1 -nographic -no-reboot -nodefaults \
        -netdev "$nd" \
        -device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
        -serial "file:$OUT/node${node}.console.log" \
        -serial "file:$OUT/node${node}.ttyS1.log" \
        -serial "file:$OUT/node${node}.pcap.b64" \
        >/dev/null 2>&1 &
    LAUNCH_PID=$!
}

echo "--- booting node A (SCSNODE OVMXA, mac=$MAC_A) ---"
launch_node A "$MAC_A" A; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (SCSNODE OVMXB, mac=$MAC_B) ---"
launch_node B "$MAC_B" B; PB=$LAUNCH_PID

( sleep "$WALL"; kill -9 "$PA" "$PB" 2>/dev/null ) &
GUARD=$!
wait "$PA" 2>/dev/null
wait "$PB" 2>/dev/null
kill "$GUARD" 2>/dev/null

echo ""
echo "=== node A log (ttyS1) ==="; cat "$OUT/nodeA.ttyS1.log" 2>/dev/null || echo "(none)"
echo ""
echo "=== node B log (ttyS1) ==="; cat "$OUT/nodeB.ttyS1.log" 2>/dev/null || echo "(none)"

# Reconstruct the pcap artifacts.
for N in A B; do
    B64="$OUT/node${N}.pcap.b64"
    if [ -s "$B64" ]; then
        sed -n '/===PCAP-'"$N"'-B64-BEGIN===/,/===PCAP-'"$N"'-B64-END===/p' "$B64" \
            | grep -v '===PCAP-' | tr -d '\r' | base64 -d > "$OUT/node${N}.pcap" 2>/dev/null || true
        [ -s "$OUT/node${N}.pcap" ] && \
            echo "reconstructed pcap: $OUT/node${N}.pcap ($(wc -c < "$OUT/node${N}.pcap") bytes)"
    fi
done

# --- verdict -----------------------------------------------------------------
# Success oracle (mirrors tests/cluster/two-ovmx/verdict.sh): BOTH nodes reached
# VMS$VAXcluster SYSAP connection OPEN -- SCSD-I-VAXCLMEMBER in each node's log.
LA="$OUT/nodeA.ttyS1.log"; LB="$OUT/nodeB.ttyS1.log"
A_VAXCLMEMBER=0; B_VAXCLMEMBER=0
grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$LA" 2>/dev/null && A_VAXCLMEMBER=1
grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$LB" 2>/dev/null && B_VAXCLMEMBER=1

# Highest rung each node climbed (for a stall diagnosis in CI output).
ladder="SCSD-I-HELLOSENT SCSD-I-DIRHELLO SCSD-I-STARTTX SCSD-I-STARTDONE SCSD-I-VCOPEN SCSD-I-OWNDIRBOUND SCSD-I-MSCPBOUND SCSD-I-CONNRESP SCSD-I-VAXCLMEMBER SCSD-I-CMCONFIG"
highest() {
    local log="$1" top="<none - no HELLO even sent>"
    for k in $ladder; do
        grep -qa "REACHED $k" "$log" 2>/dev/null && top="$k"
    done
    echo "$top"
}
echo ""
echo "highest rung A : $(highest "$LA")"
echo "highest rung B : $(highest "$LB")"
echo ""
echo "verdict inputs: A_VAXCLMEMBER=$A_VAXCLMEMBER  B_VAXCLMEMBER=$B_VAXCLMEMBER"
echo ""
echo "=========================================="
if [ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ]; then
    echo "  DLM HARNESS H2 PASSED"
    echo "  A_VAXCLMEMBER=1 B_VAXCLMEMBER=1"
    echo "  Two real-executive OVMX nodes drove the SAME join sequencer the Docker"
    echo "  two-ovmx harness uses (OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1) to full"
    echo "  VMS\$VAXcluster membership over a QEMU socket netdev (no host bridge, no"
    echo "  privilege), each on a real /dev/vms. The join sequence completes inside"
    echo "  QEMU; the harness is ready to carry H3's cross-node \$ENQ."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H2 FAILED"
    echo "  A_VAXCLMEMBER=$A_VAXCLMEMBER B_VAXCLMEMBER=$B_VAXCLMEMBER"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 30 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 30 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
