#!/bin/bash
# run_dlm_harness_h6.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h6): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- complete the full VMS$VAXcluster join, then drive
# node A's BLKAST-WIRE sequence (OVMX_DLM_ENQ=RESONE OVMX_DLM_H6=1) and verdict
# that a remote HOLDER genuinely RECEIVED a blocking-AST over SCS and FIRED it on
# its own executive (rd vms-76d, DLM epic vms-7fa rung H6).
#
# H6 PASS iff, on top of the H2 join precondition (both nodes VAXCLMEMBER):
#   1. node A held RESONE (SCSD-I-DLMENQ), established the holder WITH a blkast
#      routine (SCSD-I-DLMHOLDARM), and CONTENDED (SCSD-I-DLMENQ2).
#   2. node A's origin record for #2 was genuinely PENDING (SCSD-I-DLMPEND).
#   3. ⭐ node B WIRED a real BLKAST to the holder (SCSD-I-DLMBLKSENT).
#   4. ⭐⭐ node A RECEIVED the BLKAST (SCSD-I-DLMBLKAST) and FIRED a real blocking
#      AST on its executive, drained via DELIVERAST (SCSD-I-DLMBLKFIRE). THIS is
#      the milestone -- the holder releases BECAUSE of the BLKAST, not on its own.
#   5. node A released the holder (SCSD-I-DLMDEQ1), node B WIRED the deferred GRANT
#      (SCSD-I-DLMDEFER), and node A's origin #2 FLIPPED NL->EX (SCSD-I-DLMH5FLIP).
#
# INV-6 / Rule 9: the verdict READS A's BLKFIRE + B's BLKSENT from the nodes' own
# SCSD logs; it never fabricates them. The AST fires in node A's EXECUTIVE on a
# real /dev/vms (dispatched + drained via DELIVERAST); the wire delivery is real SCS.

set -uo pipefail

DURATION="${H6_DURATION:-90}"
NETDEV="${H6_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H6_WALL_TIMEOUT:-600}"
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
GROUP=230.0.0.9
PORT=16009

echo "=== OVMX DLM Harness H6 Runner (vms-76d) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; node A armed OVMX_DLM_ENQ=RESONE OVMX_DLM_H6=1"
echo "milestone: node B WIREs a BLKAST to the holder (SCSD-I-DLMBLKSENT) and node A"
echo "           FIREs a real blocking AST on its executive (SCSD-I-DLMBLKFIRE)"
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
        echo "FATAL: unknown H6_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
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

echo "--- booting node A (SCSNODE OVMXA, mac=$MAC_A, requester+holder / BLKAST-wire driver) ---"
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

A_ENQ=0;     grep -qa 'SCSD-I-DLMENQ,'     "$LA" 2>/dev/null && A_ENQ=1
A_HOLDARM=0; grep -qa 'SCSD-I-DLMHOLDARM'  "$LA" 2>/dev/null && A_HOLDARM=1
A_ENQ2=0;    grep -qa 'SCSD-I-DLMENQ2'     "$LA" 2>/dev/null && A_ENQ2=1
A_PEND=0;    grep -qa 'SCSD-I-DLMPEND'     "$LA" 2>/dev/null && A_PEND=1
B_BLKSENT=0; grep -qa 'SCSD-I-DLMBLKSENT'  "$LB" 2>/dev/null && B_BLKSENT=1
A_BLKAST=0;  grep -qa 'SCSD-I-DLMBLKAST,'  "$LA" 2>/dev/null && A_BLKAST=1
A_BLKFIRE=0; grep -qa 'SCSD-I-DLMBLKFIRE'  "$LA" 2>/dev/null && A_BLKFIRE=1
A_DEQ1=0;    grep -qa 'SCSD-I-DLMDEQ1'     "$LA" 2>/dev/null && A_DEQ1=1
B_DEFER=0;   grep -qa 'SCSD-I-DLMDEFER'    "$LB" 2>/dev/null && B_DEFER=1
A_FLIP=0;    grep -qa 'SCSD-I-DLMH5FLIP'   "$LA" 2>/dev/null && A_FLIP=1

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
echo "  A_ENQ=$A_ENQ  A_HOLDARM=$A_HOLDARM  A_ENQ2=$A_ENQ2   (held RESONE with a blkast routine, then contended)"
echo "  A_PEND=$A_PEND   (node A's origin record for #2 was PENDING)"
echo "  B_BLKSENT=$B_BLKSENT   (node B WIRED a BLKAST to the holder)"
echo "  A_BLKAST=$A_BLKAST  A_BLKFIRE=$A_BLKFIRE   (node A RECEIVED the BLKAST and FIRED a real AST)"
echo "  A_DEQ1=$A_DEQ1   (node A released the holder in response to the BLKAST)"
echo "  B_DEFER=$B_DEFER  A_FLIP=$A_FLIP   (deferred GRANT wired; origin #2 flipped NL->EX)"
echo ""

FAIL=0
[ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ] || { echo "  MISS: H2 join precondition not met on both nodes"; FAIL=1; }
[ "$A_ENQ" = 1 ]     || { echo "  MISS: node A did not send the first \$ENQ (no SCSD-I-DLMENQ)"; FAIL=1; }
[ "$A_HOLDARM" = 1 ] || { echo "  MISS: node A did not establish the holder with a blkast routine (no SCSD-I-DLMHOLDARM)"; FAIL=1; }
[ "$A_ENQ2" = 1 ]    || { echo "  MISS: node A did not contend with a second \$ENQ (no SCSD-I-DLMENQ2)"; FAIL=1; }
[ "$A_PEND" = 1 ]    || { echo "  MISS: node A's origin record was never PENDING (no SCSD-I-DLMPEND)"; FAIL=1; }
[ "$A_DEQ1" = 1 ]    || { echo "  MISS: node A did not release the holder (no SCSD-I-DLMDEQ1)"; FAIL=1; }
[ "$B_DEFER" = 1 ]   || { echo "  MISS: node B did not WIRE the deferred GRANT (no SCSD-I-DLMDEFER)"; FAIL=1; }
[ "$A_FLIP" = 1 ]    || { echo "  MISS: node A's origin record for #2 did not flip NL->EX (no SCSD-I-DLMH5FLIP)"; FAIL=1; }

# THE MILESTONE ASSERTIONS: the BLKAST wire.
if [ "$B_BLKSENT" != 1 ]; then
    echo "  FAIL: node B did NOT WIRE a BLKAST to the holder (no SCSD-I-DLMBLKSENT) --"
    echo "        the executive did not name a cross-node holder to notify, or the"
    echo "        server leg did not send the BLKAST frame."
    FAIL=1
fi
if [ "$A_BLKAST" != 1 ] || [ "$A_BLKFIRE" != 1 ]; then
    echo "  FAIL: node A did NOT receive+fire the BLKAST (BLKAST=$A_BLKAST FIRE=$A_BLKFIRE) --"
    echo "        the holder-side blocking AST did not genuinely fire on A's executive"
    echo "        (SCSD-I-DLMBLKFIRE reads the AST drained back via DELIVERAST)."
    FAIL=1
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H6 PASSED: the BLKAST wire proven -- a remote holder RECEIVED a"
    echo "  blocking-AST over SCS and FIRED it on its own executive. Node A held RESONE EX"
    echo "  with a registered blocking-AST routine; a SECOND incompatible \$ENQ QUEUED on"
    echo "  node B, which WIRED a real BLKAST to the holder (SCSD-I-DLMBLKSENT). Node A"
    echo "  RECEIVED it (SCSD-I-DLMBLKAST), dispatched it into its executive, and a genuine"
    echo "  user-mode blocking AST FIRED -- drained back via DELIVERAST (SCSD-I-DLMBLKFIRE)."
    echo "  The holder then released #1 IN RESPONSE to the BLKAST; node B WIRED the deferred"
    echo "  GRANT and node A's origin #2 FLIPPED NL->EX. INV-6: no fabricated AST or grant --"
    echo "  the holder released BECAUSE of the BLKAST, not on its own."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H6 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
