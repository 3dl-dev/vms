#!/bin/bash
# run_dlm_harness_h8.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h8): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- complete the full VMS$VAXcluster join, then drive
# the LVB WIRE (OVMX_DLM_ENQ=RLVB OVMX_DLM_H8=1 on node A) and verdict that a
# remote holder's value block genuinely CROSSED the wire A->B and landed in the
# master resource (rd vms-d81, DLM epic vms-7fa rung H8).
#
# THE PROOF: node A holds RLVB EX, WRITES a known 16-byte value block, and
# releases with a cross-node $DEQ carrying LCK_M_VALBLK (SCSD-I-DLMLVBWR val=HEX).
# The master (node B) replicates that wire value into res->valblk; a LOCAL $ENQ on
# node B reads it back (SCSD-I-DLMLVBRD val=HEX). H8 PASS iff, on top of the H2
# join precondition (both nodes VAXCLMEMBER):
#   1. both markers are present (A wrote, B read),
#   2. A_val == B_val  (the LVB replicated across the wire),
#   3. A_val == B_val == the EXPECTED known 16-byte pattern.
# A missing wire write leaves B reading zeros -> mismatch -> FAIL (never vacuous).
#
# INV-6 / Rule 9: the verdict READS A's DLMLVBWR + B's DLMLVBRD from the nodes' own
# SCSD logs; the values are the REAL executive bytes, hex-encoded verbatim. The
# read is a real LOCAL $ENQ into node B's EXECUTIVE on a live /dev/vms.

set -uo pipefail

DURATION="${H8_DURATION:-90}"
NETDEV="${H8_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H8_WALL_TIMEOUT:-600}"
OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

# The known 16-byte LVB node A writes ("OVMXLVB-RUNG6" + 3 NULs). MUST track the
# scsd_h8_lvb[] array in src/vmsscs/scsd.c (there is no shared C/shell header).
EXPECT_HEX="${H8_EXPECT_HEX:-4f564d584c56422d52554e4736000000}"

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
GROUP=230.0.0.11
PORT=16011

echo "=== OVMX DLM Harness H8 Runner (vms-d81) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; node A armed OVMX_DLM_ENQ=RLVB OVMX_DLM_H8=1"
echo "milestone: node A WRITEs a 16-byte LVB over SCS (SCSD-I-DLMLVBWR) and node B"
echo "           READs it back from the master resource (SCSD-I-DLMLVBRD)"
echo "expected LVB (hex): $EXPECT_HEX"
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
        echo "FATAL: unknown H8_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
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

echo "--- booting node A (SCSNODE OVMXA, mac=$MAC_A, holder / LVB writer) ---"
launch_node A "$MAC_A" A; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (SCSNODE OVMXB, mac=$MAC_B, DLM server / master of RLVB / LVB reader) ---"
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

A_ENQ=0; grep -qa 'SCSD-I-DLMENQ,' "$LA" 2>/dev/null && A_ENQ=1

# Extract the hex value blocks the two nodes printed (verbatim). 32 lowercase hex.
A_VAL=$(grep -a 'SCSD-I-DLMLVBWR' "$LA" 2>/dev/null | sed -n 's/.*val=\([0-9a-f]\{1,\}\).*/\1/p' | head -1)
B_VAL=$(grep -a 'SCSD-I-DLMLVBRD' "$LB" 2>/dev/null | sed -n 's/.*val=\([0-9a-f]\{1,\}\).*/\1/p' | head -1)
A_WROTE=0; [ -n "$A_VAL" ] && A_WROTE=1
B_READ=0;  [ -n "$B_VAL" ] && B_READ=1

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
echo "  A_ENQ=$A_ENQ   (node A sent the cross-node \$ENQ for RLVB)"
echo "  A_WROTE=$A_WROTE  A_VAL=${A_VAL:-<none>}   (node A wrote the LVB over SCS)"
echo "  B_READ=$B_READ  B_VAL=${B_VAL:-<none>}   (node B read the master's LVB back)"
echo "  EXPECT_VAL=$EXPECT_HEX"
echo ""

FAIL=0
[ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ] || { echo "  MISS: H2 join precondition not met on both nodes"; FAIL=1; }
[ "$A_ENQ" = 1 ] || { echo "  MISS: node A did not send the cross-node \$ENQ (no SCSD-I-DLMENQ)"; FAIL=1; }

# THE MILESTONE ASSERTIONS: the LVB wire.
if [ "$A_WROTE" != 1 ]; then
    echo "  FAIL: node A did NOT write an LVB (no SCSD-I-DLMLVBWR) --"
    echo "        the holder never issued a value-block \$DEQ over the wire."
    FAIL=1
fi
if [ "$B_READ" != 1 ]; then
    echo "  FAIL: node B did NOT read the LVB back (no SCSD-I-DLMLVBRD) --"
    echo "        the master's LOCAL \$ENQ read did not run or reported no value."
    FAIL=1
fi
if [ "$A_WROTE" = 1 ] && [ "$B_READ" = 1 ]; then
    if [ "$A_VAL" != "$B_VAL" ]; then
        echo "  FAIL: LVB MISMATCH -- node B read '$B_VAL' but node A wrote '$A_VAL'."
        echo "        The value block did NOT replicate across the wire (B likely read"
        echo "        zeros -- the master resource never received A's write)."
        FAIL=1
    fi
    if [ "$B_VAL" != "$EXPECT_HEX" ]; then
        echo "  FAIL: node B's LVB '$B_VAL' != the expected known pattern '$EXPECT_HEX'."
        echo "        The replicated value is not the 16 bytes A was supposed to write."
        FAIL=1
    fi
    if [ "$A_VAL" != "$EXPECT_HEX" ]; then
        echo "  FAIL: node A's LVB '$A_VAL' != the expected known pattern '$EXPECT_HEX'."
        FAIL=1
    fi
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H8 PASSED: the LVB wire proven -- a remote holder's value block"
    echo "  genuinely CROSSED the SCS wire A->B and landed in the master resource. Node A"
    echo "  held RLVB EX, WROTE a known 16-byte LVB, and released it with a cross-node"
    echo "  \$DEQ carrying LCK_M_VALBLK (SCSD-I-DLMLVBWR val=$A_VAL). The master (node B)"
    echo "  replicated that wire value into res->valblk; a LOCAL \$ENQ on node B read it"
    echo "  back (SCSD-I-DLMLVBRD val=$B_VAL). All three equal ($EXPECT_HEX): A's write =="
    echo "  B's read == the expected pattern. INV-6: both markers are the REAL executive"
    echo "  bytes, hex-encoded verbatim -- never a fabricated echo."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H8 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
