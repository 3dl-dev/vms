#!/bin/bash
# run_dlm_h1_derisk.sh - host side (runs INSIDE the container built from
# Dockerfile.dlm-harness-h1-derisk): boot TWO minimal QEMU guests wired ONLY by
# a QEMU `socket` netdev (no host bridge, no privilege) and prove the 0x6007
# group multicast (AB-00-04-01-01-01) floods between them (rd vms-534).
#
# NETDEV modes (H1_NETDEV env):
#   mcast    (DEFAULT) - both guests join a host UDP multicast group on loopback
#                        (-netdev socket,mcast=GROUP:PORT,localaddr=127.0.0.1).
#                        This is the make-or-break form H1 wants: it models a
#                        shared L2 segment for N guests with zero host setup.
#   sockpair          - point-to-point L2-over-TCP on loopback
#                        (listen/connect). A robust 2-node fallback that is
#                        STILL a socket netdev (still CI-runnable, still no
#                        privilege) -- NOT the tap+bridge pivot.
#
# VERDICT: PASS iff BOTH guests logged SCA-L2PROBE-PEER-OK (each received the
# other's 0x6007 group-multicast frame). Reconstructs each guest's pcap from the
# base64 it emitted on ttyS2.

set -uo pipefail

WINDOW="${H1_WINDOW:-12}"
NETDEV="${H1_NETDEV:-mcast}"
WALL="${H1_WALL_TIMEOUT:-240}"
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

MAC_A=52:54:00:00:00:0a
MAC_B=52:54:00:00:00:0b
GROUP=230.0.0.7
PORT=16007

echo "=== OVMX DLM Harness H1 NETDEV DE-RISK Runner (vms-534) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV window=${WINDOW}s"
echo ""

netdev_arg() {
    # $1 = node role (A|B)
    case "$NETDEV" in
        mcast)
            echo "socket,id=net0,mcast=${GROUP}:${PORT},localaddr=127.0.0.1"
            ;;
        sockpair)
            if [ "$1" = "A" ]; then
                echo "socket,id=net0,listen=127.0.0.1:${PORT}"
            else
                echo "socket,id=net0,connect=127.0.0.1:${PORT}"
            fi
            ;;
        *)
            echo "UNKNOWN" ;;
    esac
}

boot_node() {
    # $1=role $2=mac $3=node-letter-for-cmdline
    local role="$1" mac="$2" node="$3"
    local nd; nd=$(netdev_arg "$role")
    if [ "$nd" = "UNKNOWN" ]; then
        echo "FATAL: unknown H1_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
    fi
    # ttyS0=console(file), ttyS1=result(file), ttyS2=pcap-b64(file).
    $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "$CONSOLE net.ifnames=0 biosdevname=0 panic=-1 loglevel=4 ovmx.node=${node} ovmx.window=${WINDOW}" \
        -m 256M -smp 1 -nographic -no-reboot -nodefaults \
        -netdev "$nd" \
        -device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
        -serial "file:$OUT/node${node}.console.log" \
        -serial "file:$OUT/node${node}.ttyS1.log" \
        -serial "file:$OUT/node${node}.pcap.b64" \
        >/dev/null 2>&1 &
    echo $!
}

# In sockpair mode the listener (A) must be up before the connector (B); in
# mcast mode order is irrelevant. Start A, then B.
echo "--- booting node A (mac=$MAC_A) ---"
PA=$(boot_node A "$MAC_A" A)
sleep 2
echo "--- booting node B (mac=$MAC_B) ---"
PB=$(boot_node B "$MAC_B" B)

# Hard wall: reap if a guest wedges (poweroff -f should end each guest itself).
( sleep "$WALL"; kill -9 "$PA" "$PB" 2>/dev/null ) &
GUARD=$!
wait "$PA" 2>/dev/null; RA=$?
wait "$PB" 2>/dev/null; RB=$?
kill "$GUARD" 2>/dev/null

echo ""
echo "=== node A result (ttyS1) ==="; cat "$OUT/nodeA.ttyS1.log" 2>/dev/null || echo "(none)"
echo ""
echo "=== node B result (ttyS1) ==="; cat "$OUT/nodeB.ttyS1.log" 2>/dev/null || echo "(none)"

# Reconstruct pcaps from the base64 UART dumps.
for N in A B; do
    B64="$OUT/node${N}.pcap.b64"
    if [ -s "$B64" ]; then
        sed -n '/===PCAP-'"$N"'-B64-BEGIN===/,/===PCAP-'"$N"'-B64-END===/p' "$B64" \
            | grep -v '===PCAP-' | tr -d '\r' | base64 -d > "$OUT/node${N}.pcap" 2>/dev/null || true
        if [ -s "$OUT/node${N}.pcap" ]; then
            echo "reconstructed pcap: $OUT/node${N}.pcap ($(wc -c < "$OUT/node${N}.pcap") bytes)"
        fi
    fi
done

echo ""
PEER_A=0; PEER_B=0
grep -qa 'SCA-L2PROBE-PEER-OK' "$OUT/nodeA.ttyS1.log" 2>/dev/null && PEER_A=1
grep -qa 'SCA-L2PROBE-PEER-OK' "$OUT/nodeB.ttyS1.log" 2>/dev/null && PEER_B=1

echo "=========================================="
if [ "$PEER_A" = 1 ] && [ "$PEER_B" = 1 ]; then
    echo "  H1 NETDEV DE-RISK PASSED (mode=$NETDEV)"
    echo "  Both guests received the other's 0x6007 group multicast over a QEMU"
    echo "  socket netdev -- NO host bridge, NO privilege. Full H1 can build on it."
    echo "=========================================="
    exit 0
else
    echo "  H1 NETDEV DE-RISK FAILED (mode=$NETDEV): peer_A=$PEER_A peer_B=$PEER_B"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 25 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 25 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
