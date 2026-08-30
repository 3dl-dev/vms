#!/bin/bash
# run_dlm_harness_ec75.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-ec75): boot THREE OVMX QEMU nodes A/B/C,
# EACH with a real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast)
# netdev -- NO host bridge, NO privilege -- complete the VMS$VAXcluster join on
# all three, and verdict DISTRIBUTED DEADLOCK DETECTION (rd vms-ec75, DLM rung
# H11): a genuine cross-node wait-for cycle (A waits-for B, B waits-for A, both
# blocked behind the other's cross-node hold on resources mastered by C) is
# DETECTED by an edge-chasing search over REAL executive state, and EXACTLY ONE
# deterministic victim's queued $ENQ is aborted with SS$_DEADLOCK.
#
# ec75 PASS iff:
#   1. ALL THREE nodes reach VAXCLMEMBER.
#   2. Node C (the master of both contended resources) emits SCSD-I-DLKCYCLE with
#      initiator CSID=1030 -- the search closed a REAL cross-node cycle read off
#      res->granted + the pending origins.
#   3. Node C emits SCSD-I-DLKVICTIM -- it aborted the global-min victim (A's
#      request) with SS$_DEADLOCK.
#   4. Node A (the deterministic global-min victim, CSID 1030 < 1031) emits
#      SCSD-I-DLKVICTIM showing its WAIT $ENQ returned status=0x00000E0A
#      (SS$_DEADLOCK).
#   5. Node B's request is NOT aborted: node B emits NO SCSD-I-DLKVICTIM (its
#      queued $ENQ stays queued) -- exactly ONE victim breaks the cycle.
#
# INV-6 / Rule 9: the verdict READS the values the nodes' own SCSD emitted (the
# REAL executive-returned cycle + abort), never fabricates them. A single boot.

set -uo pipefail

DURATION="${EC75_DURATION:-110}"
WALL="${EC75_WALL_TIMEOUT:-420}"
OUT_BASE="${OUT_DIR:-/out}"
mkdir -p "$OUT_BASE"

CSID_A=1030
CSID_B=1031
CSID_C=1032
SS_DEADLOCK="0x00000E0A"   # SS$_DEADLOCK = 3594

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

MAC_A=52:54:00:00:01:0a
MAC_B=52:54:00:00:01:0b
MAC_C=52:54:00:00:01:0c
GROUP=230.0.0.17
PORT=16017

echo "=== OVMX DLM Harness ec75 Runner (rd vms-ec75, DLM rung H11) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } duration=${DURATION}s wall=${WALL}s"
echo "milestone: a genuine cross-node deadlock cycle (A<->B, resources mastered by C)"
echo "           is DETECTED by the executive's edge-chase and EXACTLY ONE victim's"
echo "           \$ENQ is aborted with SS\$_DEADLOCK (the other stays queued)."
echo ""

LAUNCH_PID=0
launch_node() {
    # Sets the GLOBAL LAUNCH_PID (a $(...) subshell PID is not a child of this
    # shell, so a later wait on it no-ops -- see run_dlm_harness_e84.sh).
    local node="$1" mac="$2" dur="$3" out="$4"
    local append="$CONSOLE net.ifnames=0 biosdevname=0 panic=-1 loglevel=4 ovmx.node=${node} ovmx.duration=${dur}"
    $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "$append" \
        -m 512M -smp 1 -nographic -no-reboot -nodefaults \
        -netdev "socket,id=net0,mcast=${GROUP}:${PORT},localaddr=127.0.0.1" \
        -device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
        -serial "file:$out/node${node}.console.log" \
        -serial "file:$out/node${node}.ttyS1.log" \
        >/dev/null 2>&1 &
    LAUNCH_PID=$!
}

OUT="$OUT_BASE/run"
mkdir -p "$OUT"

echo "--- booting node A (csid=$CSID_A, the initiator + deterministic victim) ---"
launch_node A "$MAC_A" "$DURATION" "$OUT"; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (csid=$CSID_B, the other contender) ---"
launch_node B "$MAC_B" "$DURATION" "$OUT"; PB=$LAUNCH_PID
sleep 2
echo "--- booting node C (csid=$CSID_C, master of RES_D + RES_E; search driver) ---"
launch_node C "$MAC_C" "$DURATION" "$OUT"; PC=$LAUNCH_PID

( sleep "$WALL"; kill -9 "$PA" "$PB" "$PC" 2>/dev/null ) &
GUARD=$!
wait "$PA" 2>/dev/null
wait "$PB" 2>/dev/null
wait "$PC" 2>/dev/null
kill "$GUARD" 2>/dev/null

echo ""
for N in A B C; do
    echo "=== node $N log (ttyS1) ==="
    cat "$OUT/node${N}.ttyS1.log" 2>/dev/null || echo "(none)"
    echo ""
done

# --- verdict -----------------------------------------------------------------
LA="$OUT/nodeA.ttyS1.log"
LB="$OUT/nodeB.ttyS1.log"
LC="$OUT/nodeC.ttyS1.log"

member_reached() { grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$1" 2>/dev/null; }

FAIL=0

echo "--- (1) 3-way join precondition ---"
for pair in "A:$LA" "B:$LB" "C:$LC"; do
    N=${pair%%:*}; f=${pair#*:}
    if member_reached "$f"; then
        echo "  node $N: VAXCLMEMBER reached"
    else
        echo "  FAIL: node $N never reached VAXCLMEMBER"
        FAIL=1
    fi
done

echo ""
echo "--- (2) node C detected the cross-node deadlock CYCLE ---"
CYCLE_LINE=$(grep -a 'SCSD-I-DLKCYCLE,' "$LC" 2>/dev/null | head -1)
if [ -n "$CYCLE_LINE" ]; then
    echo "  $CYCLE_LINE"
    echo "$CYCLE_LINE" | grep -qa "initiator CSID=$CSID_A" \
        || { echo "  FAIL: DLKCYCLE initiator is not A ($CSID_A)"; FAIL=1; }
else
    echo "  FAIL: node C never emitted SCSD-I-DLKCYCLE -- no cross-node cycle detected"
    FAIL=1
fi

echo ""
echo "--- (3) node C aborted the global-min victim with SS\$_DEADLOCK ---"
CVIC_LINE=$(grep -a 'SCSD-I-DLKVICTIM,' "$LC" 2>/dev/null | head -1)
if [ -n "$CVIC_LINE" ]; then
    echo "  $CVIC_LINE"
else
    echo "  FAIL: node C never emitted SCSD-I-DLKVICTIM -- no victim aborted"
    FAIL=1
fi

echo ""
echo "--- (4) node A (the victim) saw its WAIT \$ENQ return SS\$_DEADLOCK ---"
AVIC_LINE=$(grep -a 'SCSD-I-DLKVICTIM,' "$LA" 2>/dev/null | head -1)
if [ -n "$AVIC_LINE" ]; then
    echo "  $AVIC_LINE"
    # The marker prints the status verbatim as "status=0x00000E0A" (SS$_DEADLOCK
    # = 3594 = 0xE0A, upper-case hex from the %08X). Match it literally -- do NOT
    # tr the whole line, which would also upper-case the word "status".
    echo "$AVIC_LINE" | grep -qa "status=$SS_DEADLOCK" \
        || { echo "  FAIL: node A DLKVICTIM status is not SS\$_DEADLOCK ($SS_DEADLOCK)"; FAIL=1; }
else
    echo "  FAIL: node A never emitted SCSD-I-DLKVICTIM -- its \$ENQ did not return SS\$_DEADLOCK"
    FAIL=1
fi

echo ""
echo "--- (5) node B's request was NOT aborted (exactly one victim) ---"
if grep -qa 'SCSD-I-DLKVICTIM,' "$LB" 2>/dev/null; then
    echo "  FAIL: node B ALSO emitted SCSD-I-DLKVICTIM -- TWO victims aborted, the"
    echo "        double-abort the global-min rule exists to prevent:"
    grep -a 'SCSD-I-DLKVICTIM,' "$LB" | sed 's/^/    /'
    FAIL=1
else
    echo "  node B: NO DLKVICTIM -- its cross-node \$ENQ stays queued (correct)"
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS ec75 PASSED: DISTRIBUTED DEADLOCK DETECTION proven on the rail."
    echo "  A genuine cross-node wait-for cycle (A waits-for B on RES_E, B waits-for A"
    echo "  on RES_D, both mastered by C) was DETECTED by the executive's edge-chasing"
    echo "  search over REAL state (res->granted + pending origins, INV-6): node C"
    echo "  emitted SCSD-I-DLKCYCLE (initiator=$CSID_A) and aborted the deterministic"
    echo "  global-min victim (A) with SS\$_DEADLOCK; node A's WAIT \$ENQ returned"
    echo "  SS\$_DEADLOCK ($SS_DEADLOCK); node B's request stayed queued -- exactly"
    echo "  ONE victim broke the cycle. The LAST DLM rung is complete."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS ec75 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 50 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 50 "$OUT/nodeB.console.log" 2>/dev/null || true
    echo "--- node C console tail ---"; tail -n 50 "$OUT/nodeC.console.log" 2>/dev/null || true
    exit 1
fi
