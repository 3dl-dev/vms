#!/bin/bash
# run_dlm_harness_h9.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h9): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- complete the full VMS$VAXcluster join, then drive
# the LVB READ crossing and verdict that node A read node B's SEEDED value block
# back over the GRANT wire (rd vms-eeb, DLM epic vms-7fa rung H9, the MIRROR of
# the H8 write crossing vms-d81).
#
# H9 PASS iff, on top of the H2 join precondition (both nodes VAXCLMEMBER):
#   1. node B (master of RRD) SEEDED RRD's value block (SCSD-I-DLMLVBSEED).
#   2. node A did a cross-node $ENQ (SCSD-I-DLMENQ) and it was GRANTED
#      (SCSD-I-DLMDONE).
#   3. ⭐⭐ node A read the master's LVB back (SCSD-I-DLMLVBRD9), AND the value it
#      read == node B's seeded value == the expected known 16-byte pattern
#      (a THREE-WAY equality). THIS is the milestone -- the value block genuinely
#      crossed the wire B->A on the GRANT, not fabricated. A dropped LVB leaves A
#      reading zeros -> mismatch -> FAIL.
#
# INV-6 / Rule 9: the verdict READS the hex the nodes' own SCSD emitted (the REAL
# executive bytes, scsd_hex16) and compares them; it never fabricates a value. The
# LVB is written/read in the nodes' EXECUTIVEs on real /dev/vms; the delivery is
# real SCS.

set -uo pipefail

DURATION="${H9_DURATION:-90}"
NETDEV="${H9_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H9_WALL_TIMEOUT:-600}"
OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

# The expected 16-byte LVB pattern: "OVMXLVB-READ9" NUL-padded to 16 bytes.
# DISTINCT from the H8 write crossing ("OVMXLVB-RUNG6"). MUST match the seed
# constant scsd_h9_known_block in src/vmsscs/scsd.c.
H9_EXPECT_HEX="4F564D584C56422D5245414439000000"

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
GROUP=230.0.0.12
PORT=16012

echo "=== OVMX DLM Harness H9 Runner (vms-eeb) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; node A armed OVMX_DLM_ENQ=RRD OVMX_DLM_H9=1"
echo "milestone: node B SEEDs RRD's LVB (SCSD-I-DLMLVBSEED) and node A reads it back"
echo "           over the GRANT wire (SCSD-I-DLMLVBRD9); three-way equality vs $H9_EXPECT_HEX"
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
        echo "FATAL: unknown H9_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
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

echo "--- booting node A (SCSNODE OVMXA, mac=$MAC_A, requester / LVB-read driver) ---"
launch_node A "$MAC_A" A; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (SCSNODE OVMXB, mac=$MAC_B, DLM server / master + LVB seed of RRD) ---"
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

A_ENQ=0;  grep -qa 'SCSD-I-DLMENQ,'    "$LA" 2>/dev/null && A_ENQ=1
A_DONE=0; grep -qa 'SCSD-I-DLMDONE'    "$LA" 2>/dev/null && A_DONE=1
B_SEED=0; grep -qa 'SCSD-I-DLMLVBSEED' "$LB" 2>/dev/null && B_SEED=1
A_RD9=0;  grep -qa 'SCSD-I-DLMLVBRD9'  "$LA" 2>/dev/null && A_RD9=1

# Extract the hex value blocks (uppercase 32 hex digits after 'val='). These are
# the REAL executive bytes each node's SCSD emitted -- never fabricated here.
extract_val() {
    # $1 = log file, $2 = marker
    grep -a "$2" "$1" 2>/dev/null | head -1 \
        | sed -n 's/.*val=\([0-9A-Fa-f]\{32\}\).*/\1/p' \
        | tr 'a-f' 'A-F'
}
B_SEED_VAL=$(extract_val "$LB" 'SCSD-I-DLMLVBSEED')
A_RD9_VAL=$(extract_val "$LA" 'SCSD-I-DLMLVBRD9')

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
echo "  A_ENQ=$A_ENQ  A_DONE=$A_DONE   (node A cross-node \$ENQ sent + GRANTED)"
echo "  B_SEED=$B_SEED   (node B seeded RRD's value block)"
echo "  A_RD9=$A_RD9   (node A read a value block back over the GRANT)"
echo "  expected LVB : $H9_EXPECT_HEX  (\"OVMXLVB-READ9\" NUL-padded to 16 bytes)"
echo "  B seeded LVB : ${B_SEED_VAL:-<none>}"
echo "  A read   LVB : ${A_RD9_VAL:-<none>}"
echo ""

FAIL=0
[ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ] || { echo "  MISS: H2 join precondition not met on both nodes"; FAIL=1; }
[ "$A_ENQ" = 1 ]  || { echo "  MISS: node A did not send the cross-node \$ENQ (no SCSD-I-DLMENQ)"; FAIL=1; }
[ "$A_DONE" = 1 ] || { echo "  MISS: node A's cross-node \$ENQ round-trip did not complete (no SCSD-I-DLMDONE)"; FAIL=1; }

# THE MILESTONE ASSERTIONS: the LVB read crossing, a three-way equality.
if [ "$B_SEED" != 1 ]; then
    echo "  FAIL: node B did NOT seed RRD's value block (no SCSD-I-DLMLVBSEED) --"
    echo "        the master never wrote a known LVB to res->valblk."
    FAIL=1
fi
if [ "$A_RD9" != 1 ]; then
    echo "  FAIL: node A did NOT read a value block back (no SCSD-I-DLMLVBRD9) --"
    echo "        the cross-node \$ENQ + GETLKI read-back did not run."
    FAIL=1
fi
# Guard against a vacuous pass: both extractions must be present, non-empty, and
# must NOT be all-zeros (an all-zeros LVB means the block was dropped on the wire).
if [ -z "$B_SEED_VAL" ] || [ -z "$A_RD9_VAL" ]; then
    echo "  FAIL: could not extract both LVB hex values (B_SEED_VAL='${B_SEED_VAL:-}'"
    echo "        A_RD9_VAL='${A_RD9_VAL:-}') -- a marker was missing or malformed."
    FAIL=1
else
    if [ "$A_RD9_VAL" = "00000000000000000000000000000000" ]; then
        echo "  FAIL: node A read an ALL-ZEROS value block -- the master's LVB was"
        echo "        dropped on the GRANT wire (or never stored on the origin record)."
        FAIL=1
    fi
    if [ "$A_RD9_VAL" != "$B_SEED_VAL" ]; then
        echo "  FAIL: node A's read LVB ($A_RD9_VAL) != node B's seeded LVB ($B_SEED_VAL) --"
        echo "        the value block that crossed the wire is NOT what the master holds."
        FAIL=1
    fi
    if [ "$A_RD9_VAL" != "$H9_EXPECT_HEX" ]; then
        echo "  FAIL: node A's read LVB ($A_RD9_VAL) != the expected known pattern"
        echo "        ($H9_EXPECT_HEX) -- the transported value is not the seeded pattern."
        FAIL=1
    fi
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H9 PASSED: the LVB READ crossing proven -- node A read node B's"
    echo "  SEEDED value block back over the SCS GRANT wire. Node B (master of RRD) wrote"
    echo "  a known 16-byte LVB to res->valblk via a LOCAL \$ENQ EX+VALBLK then \$DEQ"
    echo "  (SCSD-I-DLMLVBSEED). Node A did a cross-node \$ENQ (EX, VALBLK); the master"
    echo "  READ its res->valblk into the GRANT reply, which carried the LVB back over SCS."
    echo "  Node A dispatched the GRANT into its executive (grant_recv stored the LVB) and"
    echo "  GETLKI'd its own handle, reading the value back (SCSD-I-DLMLVBRD9). THREE-WAY"
    echo "  equality held: A_read == B_seed == $H9_EXPECT_HEX. INV-6: no fabricated block --"
    echo "  the value block genuinely crossed the wire B->A on the GRANT, the mirror of H8."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H9 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
