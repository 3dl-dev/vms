#!/bin/bash
# run_dlm_harness_h1.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h1): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- and verdict the H1 two-node SCS HELLO proof
# (rd vms-534).
#
# H1 PASS iff BOTH nodes:
#   (a) emitted their own HELLO      -> SCSD-I-HELLOSENT in that node's log, AND
#   (b) received the PEER's HELLO    -> SCSD-I-FRAME with src = the OTHER node's
#                                       distinct HW MAC in that node's log.
# That is the cross-node 0x6007 LAVC/SCA datalink the DLM (H2+) rides on. H1 is
# transport/HELLO ONLY -- it deliberately does NOT assert the full VMS$VAXcluster
# join (that is the OVMX<->OVMX join gap, a later rung) or any DLM grant (H3/H4).
#
# A per-node pcap of the 0x6007 traffic is reconstructed from each guest's
# base64 UART dump as the artifact.

set -uo pipefail

DURATION="${H1_DURATION:-15}"
NETDEV="${H1_NETDEV:-mcast}"     # mcast (default) | sockpair (see de-risk runner)
WALL="${H1_WALL_TIMEOUT:-420}"
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
# HELLO from its own looped-back beacon (AF_PACKET delivers outgoing frames too).
MAC_A=52:54:00:00:00:0a
MAC_B=52:54:00:00:00:0b
GROUP=230.0.0.7
PORT=16007

echo "=== OVMX DLM Harness H1 Runner (vms-534) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s"
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
        echo "FATAL: unknown H1_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
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
LA="$OUT/nodeA.ttyS1.log"; LB="$OUT/nodeB.ttyS1.log"
A_SENT=0; B_SENT=0; A_RCVD_B=0; B_RCVD_A=0
grep -qa 'SCSD-I-HELLOSENT' "$LA" 2>/dev/null && A_SENT=1
grep -qa 'SCSD-I-HELLOSENT' "$LB" 2>/dev/null && B_SENT=1
# Each node must have logged a received 0x6007 FRAME whose src is the PEER's MAC.
grep -qa "SCSD-I-FRAME.*src=$MAC_B" "$LA" 2>/dev/null && A_RCVD_B=1
grep -qa "SCSD-I-FRAME.*src=$MAC_A" "$LB" 2>/dev/null && B_RCVD_A=1

echo ""
echo "verdict inputs: A_HELLOSENT=$A_SENT  B_HELLOSENT=$B_SENT"
echo "                A_received_B_HELLO=$A_RCVD_B (src=$MAC_B)"
echo "                B_received_A_HELLO=$B_RCVD_A (src=$MAC_A)"
echo ""
echo "=========================================="
if [ "$A_SENT" = 1 ] && [ "$B_SENT" = 1 ] && [ "$A_RCVD_B" = 1 ] && [ "$B_RCVD_A" = 1 ]; then
    echo "  DLM HARNESS H1 PASSED"
    echo "  Two real-executive OVMX nodes exchanged 0x6007 LAVC/SCA HELLOs over a"
    echo "  QEMU socket netdev (no host bridge, no privilege). Each SENT a HELLO and"
    echo "  RECEIVED the peer's. The cross-node datalink for the DLM (H2+) is proven."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H1 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 30 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 30 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
