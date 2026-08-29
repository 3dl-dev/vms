#!/bin/bash
# run_dlm_harness_h7.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h7): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, EACH insmod'ing vms.ko with the SAME static membership vector
# (dlm_member_csids=1030,1031) but a distinct vms_local_csid (A=1030, B=1031),
# then verdict that both nodes INDEPENDENTLY resolve the SAME directory + master
# for every resource name (rd vms-1bba, DLM harness rung H7 "DB").
#
# The nodes need NO shared netdev: the directory is a pure function of (resource
# name, membership vector), so the nodes do not communicate -- no SCS join, no
# scsd, no shared L2. That the two independent nodes still AGREE is the core proof.
#
# H7 PASS iff, from both nodes' own H7DIR/H7ENQ lines:
#   (a) AGREEMENT (the core proof): for every name, node A's dir_csid == node B's
#       dir_csid AND node A's master_csid == node B's master_csid.
#   (b) SPLIT IS REAL: across the name set at least one name resolves dir=1030 and
#       at least one resolves dir=1031 -- the directory genuinely distributes
#       mastering, it is not all-local.
#   (c) HONEST NO-REGRESSION (INV-6): on each node, a LOCAL $ENQ for a name whose
#       directory is the OTHER node returns SS$_UNSUPPORTED (2296) -- forwarding
#       is deferred (DC, 0.4), it fails honestly rather than fabricating a grant;
#       a LOCAL $ENQ for a name whose directory IS this node succeeds (odd status,
#       SS$_NORMAL region).
#
# NEGATIVE CONTROL (no vacuous pass): both nodes must have produced non-empty
# ttyS1 logs and both must have reached H7-DRIVER-DONE, and each must have emitted
# the full name set. An empty log or a parse failure is a FAIL, never a pass.

set -uo pipefail

WALL="${H7_WALL_TIMEOUT:-600}"
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

echo "=== OVMX DLM Harness H7 Runner (vms-1bba) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } wall=${WALL}s"
echo "each node: insmod vms.ko vms_local_csid=<A=1030|B=1031> dlm_member_csids=1030,1031"
echo "core proof: both nodes independently resolve the SAME dir + master for every name"
echo "            (no SCS join, no shared netdev -- the directory is a pure function"
echo "             of name + the shared membership vector)"
echo ""

LAUNCH_PID=0
launch_node() {
    local node="$1"
    # ttyS0=console(file), ttyS1=node verdict log(file). NO netdev: the nodes do
    # not communicate; agreement between two independent nodes IS the proof.
    $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "$CONSOLE net.ifnames=0 biosdevname=0 panic=-1 loglevel=4 ovmx.node=${node}" \
        -m 512M -smp 1 -nographic -no-reboot -nodefaults \
        -serial "file:$OUT/node${node}.console.log" \
        -serial "file:$OUT/node${node}.ttyS1.log" \
        >/dev/null 2>&1 &
    LAUNCH_PID=$!
}

echo "--- booting node A (vms_local_csid=1030) ---"
launch_node A; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (vms_local_csid=1031) ---"
launch_node B; PB=$LAUNCH_PID

( sleep "$WALL"; kill -9 "$PA" "$PB" 2>/dev/null ) &
GUARD=$!
wait "$PA" 2>/dev/null
wait "$PB" 2>/dev/null
kill "$GUARD" 2>/dev/null

echo ""
echo "=== node A log (ttyS1) ==="; cat "$OUT/nodeA.ttyS1.log" 2>/dev/null || echo "(none)"
echo ""
echo "=== node B log (ttyS1) ==="; cat "$OUT/nodeB.ttyS1.log" 2>/dev/null || echo "(none)"

# --- verdict -----------------------------------------------------------------
LA="$OUT/nodeA.ttyS1.log"; LB="$OUT/nodeB.ttyS1.log"

FAIL=0

# field <log> <marker> <name> <key>  -> prints the numeric value of key= on the
# matching "<marker> name=<name> ..." line, or empty if absent.
field() {
    grep -a "$2 name=$3 " "$1" 2>/dev/null \
        | sed -n "s/.*[[:space:]]$4=\([0-9]*\).*/\1/p" | head -1
}

# --- negative control: both nodes produced real output and finished -----------
if [ ! -s "$LA" ] || [ ! -s "$LB" ]; then
    echo "  FAIL: a node produced no ttyS1 output (A=$( [ -s "$LA" ] && echo nonempty || echo EMPTY ), B=$( [ -s "$LB" ] && echo nonempty || echo EMPTY )) -- cannot be a pass"
    FAIL=1
fi
A_DONE=0; grep -qa 'H7-DRIVER-DONE' "$LA" 2>/dev/null && A_DONE=1
B_DONE=0; grep -qa 'H7-DRIVER-DONE' "$LB" 2>/dev/null && B_DONE=1
[ "$A_DONE" = 1 ] || { echo "  FAIL: node A did not reach H7-DRIVER-DONE (driver never completed)"; FAIL=1; }
[ "$B_DONE" = 1 ] || { echo "  FAIL: node B did not reach H7-DRIVER-DONE (driver never completed)"; FAIL=1; }

# --- each node booted with the expected local CSID ---------------------------
A_LOCAL=$(field "$LA" H7DIR "$(grep -am1 'H7DIR ' "$LA" | sed -n 's/.*name=\([A-Za-z0-9]*\).*/\1/p')" local)
B_LOCAL=$(field "$LB" H7DIR "$(grep -am1 'H7DIR ' "$LB" | sed -n 's/.*name=\([A-Za-z0-9]*\).*/\1/p')" local)
echo ""
echo "node A local_csid=${A_LOCAL:-<none>}   node B local_csid=${B_LOCAL:-<none>}"
[ "$A_LOCAL" = "1030" ] || { echo "  FAIL: node A local_csid is '${A_LOCAL:-<none>}', expected 1030"; FAIL=1; }
[ "$B_LOCAL" = "1031" ] || { echo "  FAIL: node B local_csid is '${B_LOCAL:-<none>}', expected 1031"; FAIL=1; }

# --- names emitted by node A (the driver's fixed set) ------------------------
NAMES=$(grep -a 'H7DIR ' "$LA" 2>/dev/null | sed -n 's/.*name=\([A-Za-z0-9]*\).*/\1/p')
NCOUNT=$(printf '%s\n' "$NAMES" | grep -c . )
NB=$(grep -ac 'H7DIR ' "$LB" 2>/dev/null); NB=${NB:-0}
echo "node A emitted $NCOUNT H7DIR lines; node B emitted $NB"
[ "$NCOUNT" -ge 6 ] || { echo "  FAIL: node A emitted only $NCOUNT names (< 6) -- too few to prove a split"; FAIL=1; }
[ "$NCOUNT" = "$NB" ] || { echo "  FAIL: node A/B emitted different name counts ($NCOUNT vs $NB) -- nodes did not run the same driver"; FAIL=1; }

# --- per-name assertions -----------------------------------------------------
SEEN_1030=0
SEEN_1031=0
AGREE_CHECKED=0
echo ""
echo "per-name resolution (A vs B):"
for n in $NAMES; do
    ad=$(field "$LA" H7DIR "$n" dir);    am=$(field "$LA" H7DIR "$n" master)
    bd=$(field "$LB" H7DIR "$n" dir);    bm=$(field "$LB" H7DIR "$n" master)
    ae=$(field "$LA" H7ENQ "$n" enq_status)
    be=$(field "$LB" H7ENQ "$n" enq_status)

    printf "  %-8s A(dir=%s master=%s enq=%s)  B(dir=%s master=%s enq=%s)\n" \
        "$n" "${ad:-?}" "${am:-?}" "${ae:-?}" "${bd:-?}" "${bm:-?}" "${be:-?}"

    # (a) AGREEMENT -- the core proof.
    if [ -z "$ad" ] || [ -z "$bd" ] || [ -z "$am" ] || [ -z "$bm" ]; then
        echo "      MISS: $n missing dir/master on one node (A dir=$ad master=$am / B dir=$bd master=$bm)"
        FAIL=1
    else
        AGREE_CHECKED=$((AGREE_CHECKED + 1))
        if [ "$ad" != "$bd" ]; then
            echo "      FAIL: $n DIRECTORY disagrees (A=$ad B=$bd) -- nodes did not resolve the same directory"
            FAIL=1
        fi
        if [ "$am" != "$bm" ]; then
            echo "      FAIL: $n MASTER disagrees (A=$am B=$bm) -- nodes did not resolve the same master"
            FAIL=1
        fi
        [ "$ad" = "1030" ] && SEEN_1030=1
        [ "$ad" = "1031" ] && SEEN_1031=1
    fi

    # (c) HONEST NO-REGRESSION, node A (local 1030).
    if [ -n "$ad" ] && [ -n "$ae" ]; then
        if [ "$ad" = "1030" ]; then
            if [ $(( ae & 1 )) -ne 1 ]; then
                echo "      FAIL: node A local-mastered $n but \$ENQ status=$ae is not an odd success"
                FAIL=1
            fi
        else
            if [ "$ae" != "2296" ]; then
                echo "      FAIL: node A remote-mastered $n but \$ENQ status=$ae != 2296 (SS\$_UNSUPPORTED)"
                FAIL=1
            fi
        fi
    fi
    # (c) HONEST NO-REGRESSION, node B (local 1031).
    if [ -n "$bd" ] && [ -n "$be" ]; then
        if [ "$bd" = "1031" ]; then
            if [ $(( be & 1 )) -ne 1 ]; then
                echo "      FAIL: node B local-mastered $n but \$ENQ status=$be is not an odd success"
                FAIL=1
            fi
        else
            if [ "$be" != "2296" ]; then
                echo "      FAIL: node B remote-mastered $n but \$ENQ status=$be != 2296 (SS\$_UNSUPPORTED)"
                FAIL=1
            fi
        fi
    fi
done

# (b) SPLIT IS REAL.
echo ""
echo "directory distribution: saw_1030=$SEEN_1030 saw_1031=$SEEN_1031 (agreement checked on $AGREE_CHECKED names)"
if [ "$SEEN_1030" != 1 ] || [ "$SEEN_1031" != 1 ]; then
    echo "  FAIL: the directory did NOT distribute across both members (want at least one name -> 1030 AND one -> 1031); mastering is all-local, not a real directory"
    FAIL=1
fi
[ "$AGREE_CHECKED" -ge 6 ] || { echo "  FAIL: agreement was checkable on only $AGREE_CHECKED names (< 6)"; FAIL=1; }

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H7 PASSED: two nodes, given the SAME static membership vector"
    echo "  (dlm_member_csids=1030,1031) and distinct local CSIDs, INDEPENDENTLY resolved"
    echo "  the SAME directory AND master for every resource name -- with NO communication"
    echo "  between them (no SCS join, no shared netdev). The directory genuinely"
    echo "  distributed mastering across both members (some names -> 1030, some -> 1031),"
    echo "  and each node's LOCAL \$ENQ succeeded for a name it masters and failed honestly"
    echo "  with SS\$_UNSUPPORTED (2296) for a name the OTHER node masters -- forwarding is"
    echo "  deferred (DC, 0.4), not fabricated. INV-6: no faked remote grant."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H7 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
