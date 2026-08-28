#!/bin/bash
# run_dlm_harness_h5.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h5): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- complete the full VMS$VAXcluster join, then drive
# node A's BLOCK-THEN-GRANT-over-the-wire sequence (OVMX_DLM_ENQ=RESONE
# OVMX_DLM_H5=1) and verdict that the QUEUED cross-node request GRANTED on the
# REQUESTER node A, across the live SCS wire, driven by a real remote $DEQ
# (rd vms-6ca, DLM epic vms-7fa rung H5).
#
# H5 PASS iff, on top of the H2 join precondition (both nodes VAXCLMEMBER):
#   1. node A held RESONE (SCSD-I-DLMENQ) and CONTENDED (SCSD-I-DLMENQ2).
#   2. ⭐ node A's requester origin record for #2 was genuinely PENDING after B's
#      queued-reply (SCSD-I-DLMPEND) -- a real block read from A's OWN executive,
#      not a userspace flag.
#   3. node A released the holder with a real cross-node $DEQ (SCSD-I-DLMDEQ1).
#   4. ⭐ node B WIRED the deferred GRANT off that real $DEQ (SCSD-I-DLMDEFER).
#   5. ⭐⭐ node A's origin record for #2 FLIPPED NL->EX (SCSD-I-DLMH5FLIP with
#      granted_mode=EX) -- the block-then-grant status flip observed on the
#      REQUESTER node, across the wire, driven ONLY by what the master sent. THIS
#      is the milestone.
#
# INV-6 / Rule 9: the verdict READS A's flip + B's deferred-grant from the nodes'
# own SCSD logs; it never fabricates them. The flip happens in node A's EXECUTIVE
# on a real /dev/vms (GETLKI on the origin record); the wire delivery is real SCS.
# The BLKAST wire is deferred honestly (the holder releases on its own).

set -uo pipefail

DURATION="${H5_DURATION:-90}"
NETDEV="${H5_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H5_WALL_TIMEOUT:-600}"
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
GROUP=230.0.0.8
PORT=16008

echo "=== OVMX DLM Harness H5 Runner (vms-6ca) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; node A armed OVMX_DLM_ENQ=RESONE OVMX_DLM_H5=1"
echo "milestone: node A's QUEUED cross-node request GRANTS on the REQUESTER (SCSD-I-DLMH5FLIP granted_mode=EX)"
echo "           after node B WIREs the deferred GRANT off a real \$DEQ (SCSD-I-DLMDEFER)"
echo ""

netdev_arg() {
    case "$NETDEV" in
        mcast)    echo "socket,id=net0,mcast=${GROUP}:${PORT},localaddr=127.0.0.1" ;;
        sockpair) if [ "$1" = "A" ]; then echo "socket,id=net0,listen=127.0.0.1:${PORT}";
                  else echo "socket,id=net0,connect=127.0.0.1:${PORT}"; fi ;;
        *)        echo "UNKNOWN" ;;
    esac
}

LAUNCH_PID=0
launch_node() {
    local role="$1" mac="$2" node="$3"
    local nd; nd=$(netdev_arg "$role")
    if [ "$nd" = "UNKNOWN" ]; then
        echo "FATAL: unknown H5_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
    fi
    # ttyS0=console(file), ttyS1=node verdict log(file), ttyS2=pcap-b64(file).
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

echo "--- booting node A (SCSNODE OVMXA, mac=$MAC_A, requester / block-then-grant driver) ---"
launch_node A "$MAC_A" A; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (SCSNODE OVMXB, mac=$MAC_B, DLM server / master of RESONE) ---"
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

# Precondition: the H2 join must have completed on both nodes.
A_VAXCLMEMBER=0; B_VAXCLMEMBER=0
grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$LA" 2>/dev/null && A_VAXCLMEMBER=1
grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$LB" 2>/dev/null && B_VAXCLMEMBER=1

# 1. node A held (#1) and contended (#2).
A_ENQ=0;  grep -qa 'SCSD-I-DLMENQ,'  "$LA" 2>/dev/null && A_ENQ=1
A_ENQ2=0; grep -qa 'SCSD-I-DLMENQ2'  "$LA" 2>/dev/null && A_ENQ2=1
# 2. node A saw its origin record PENDING (genuine block on the requester).
A_PEND=0; grep -qa 'SCSD-I-DLMPEND'  "$LA" 2>/dev/null && A_PEND=1
# 3. node A released the holder.
A_DEQ1=0; grep -qa 'SCSD-I-DLMDEQ1'  "$LA" 2>/dev/null && A_DEQ1=1
# 4. node B WIRED the deferred grant.
B_DEFER=0; grep -qa 'SCSD-I-DLMDEFER' "$LB" 2>/dev/null && B_DEFER=1
# 5. ⭐ node A's origin record FLIPPED NL->EX. THE MILESTONE.
A_FLIP=0; grep -qa 'SCSD-I-DLMH5FLIP' "$LA" 2>/dev/null && A_FLIP=1
A_FLIP_MODE=$(grep -a 'SCSD-I-DLMH5FLIP' "$LA" 2>/dev/null \
    | sed -n 's/.*granted_mode=\([A-Za-z0-9]*\).*/\1/p' | tail -1)

# Highest join rung each node climbed (for a stall diagnosis in CI output).
ladder="SCSD-I-HELLOSENT SCSD-I-DIRHELLO SCSD-I-STARTTX SCSD-I-STARTDONE SCSD-I-VCOPEN SCSD-I-OWNDIRBOUND SCSD-I-MSCPBOUND SCSD-I-CONNRESP SCSD-I-VAXCLMEMBER SCSD-I-CMCONFIG"
highest() {
    local log="$1" top="<none - no HELLO even sent>"
    for k in $ladder; do
        grep -qa "REACHED $k" "$log" 2>/dev/null && top="$k"
    done
    echo "$top"
}

echo ""
echo "highest join rung A : $(highest "$LA")"
echo "highest join rung B : $(highest "$LB")"
echo ""
echo "verdict inputs:"
echo "  A_VAXCLMEMBER=$A_VAXCLMEMBER  B_VAXCLMEMBER=$B_VAXCLMEMBER   (H2 join precondition)"
echo "  A_ENQ=$A_ENQ  A_ENQ2=$A_ENQ2   (node A held RESONE, then contended with a second \$ENQ)"
echo "  A_PEND=$A_PEND   (node A's origin record for #2 was PENDING -- genuine block on the requester)"
echo "  A_DEQ1=$A_DEQ1   (node A released the holder with a real cross-node \$DEQ)"
echo "  B_DEFER=$B_DEFER   (node B WIRED the deferred GRANT off the real \$DEQ)"
echo "  A_FLIP=$A_FLIP  granted_mode=${A_FLIP_MODE:-<none>}   (node A's origin record FLIPPED NL->EX -- want EX)"
echo ""

FAIL=0
[ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ] || { echo "  MISS: H2 join precondition not met on both nodes"; FAIL=1; }
[ "$A_ENQ" = 1 ]   || { echo "  MISS: node A did not send the first \$ENQ (no SCSD-I-DLMENQ)"; FAIL=1; }
[ "$A_ENQ2" = 1 ]  || { echo "  MISS: node A did not contend with a second \$ENQ (no SCSD-I-DLMENQ2)"; FAIL=1; }
[ "$A_PEND" = 1 ]  || { echo "  MISS: node A's origin record was never PENDING (no SCSD-I-DLMPEND) -- the queued-reply wire did not land"; FAIL=1; }
[ "$A_DEQ1" = 1 ]  || { echo "  MISS: node A did not release the holder (no SCSD-I-DLMDEQ1)"; FAIL=1; }
[ "$B_DEFER" = 1 ] || { echo "  MISS: node B did not WIRE the deferred GRANT (no SCSD-I-DLMDEFER) -- the deferred-grant wire did not fire"; FAIL=1; }

# THE MILESTONE ASSERTION: node A's origin record flipped NL->EX across the wire.
if [ "$A_FLIP" != 1 ]; then
    echo "  FAIL: node A's origin record did NOT flip (no SCSD-I-DLMH5FLIP) --"
    echo "        the QUEUED cross-node request never GRANTED on the requester. The"
    echo "        requester-side GRANT RECEIVE (vms_lock_dlm_xnode_dispatch) or the"
    echo "        deferred-grant wire did not complete the round-trip."
    FAIL=1
elif [ "${A_FLIP_MODE:-}" != "EX" ]; then
    echo "  FAIL: node A's origin record flipped to '${A_FLIP_MODE:-<none>}', expected EX (the granted mode)."
    FAIL=1
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H5 PASSED: block-then-grant proven on the REQUESTER over the SCS wire"
    echo "  Node A held RESONE EX, then a SECOND incompatible \$ENQ QUEUED on node B; B WIRED"
    echo "  a queued-reply and node A's requester ORIGIN record went genuinely PENDING"
    echo "  (SCSD-I-DLMPEND, read from A's OWN executive). Node A released the holder with a"
    echo "  real cross-node \$DEQ; node B released it, GRANTED the queued request, and WIRED"
    echo "  the deferred GRANT (SCSD-I-DLMDEFER) off that real \$DEQ. Node A dispatched it"
    echo "  into its executive and the origin record FLIPPED NL->EX (SCSD-I-DLMH5FLIP,"
    echo "  granted_mode=EX) -- the status flip observed on the REQUESTER node, across the"
    echo "  live SCS wire, driven ONLY by what the master sent. The BLKAST wire is deferred"
    echo "  honestly. INV-6: no fabricated wire reply or grant."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H5 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
