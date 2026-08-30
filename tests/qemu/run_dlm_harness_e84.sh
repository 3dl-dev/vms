#!/bin/bash
# run_dlm_harness_e84.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-e84): boot THREE OVMX QEMU nodes A/B/C,
# EACH with a real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast)
# netdev -- NO host bridge, NO privilege -- complete the VMS$VAXcluster join
# on all three, TWICE (two separate boots of the SAME image), and verdict the
# CROSS-NODE DIRECTORY-OWNERSHIP REFUSAL (rd vms-e84):
#
#   run "refuse": node A's cross-node $ENQ EX on RES_C is targeted at node B
#       (1031) -- NOT RES_C's directory (RES_C hashes to C, 1032, the same
#       H10/H10b hash-target fact). B's executive (dlm_resolve_master) must
#       REFUSE with SS$_UNSUPPORTED -- it must NOT self-master a resource it
#       does not own.
#   run "grant": the SAME $ENQ, targeted at node C -- RES_C's REAL directory.
#       C's executive masters it on first use and GRANTS (SS$_NORMAL) -- the
#       positive control proving the SAME code path succeeds when the target
#       IS the resource's directory.
#
# e84 PASS iff:
#   1. ALL THREE nodes reach VAXCLMEMBER on BOTH boots.
#   2. run "refuse": node A's SCSD-I-E84REFUSED shows status == SS$_UNSUPPORTED
#      (0x000008F8) from csid=1031.
#   3. run "grant": node A's SCSD-I-E84GRANTED shows status == SS$_NORMAL
#      (0x00000001) from csid=1032, with a nonzero real master_lkid.
#
# INV-6 / Rule 9: the verdict READS the values the nodes' own SCSD emitted (the
# REAL executive-returned status), never fabricates them. Both runs boot the
# EXACT SAME image; only the ovmx.dlmtarget=<csid> kernel cmdline param
# differs, selecting which peer node A's ENQ is targeted at.

set -uo pipefail

DURATION="${E84_DURATION:-60}"
WALL="${E84_WALL_TIMEOUT:-300}"
OUT_BASE="${OUT_DIR:-/out}"
mkdir -p "$OUT_BASE"

# The 3-member DLM CSID vector (== each node's SCSSYSTEMID). RES_C's directory
# hashes to C (1032) -- the SAME fact the H10/H10b harnesses use.
CSID_A=1030
CSID_B=1031
CSID_C=1032

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

echo "=== OVMX DLM Harness e84 Runner (rd vms-e84) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } duration=${DURATION}s wall=${WALL}s"
echo "milestone: A's cross-node \$ENQ on RES_C targeted at B (non-directory) is REFUSED"
echo "           (SS\$_UNSUPPORTED); the SAME ENQ targeted at C (RES_C's real directory)"
echo "           is GRANTED (SS\$_NORMAL). Two separate boots of the SAME image."
echo ""

LAUNCH_PID=0
launch_node() {
    # Sets the GLOBAL LAUNCH_PID rather than echoing $! from a $(...) command
    # substitution -- a command substitution runs in a SUBSHELL, so a PID
    # captured that way is a child of the SUBSHELL, not of this script; a
    # later `wait "$PA"` on it is not a real child of the calling shell and
    # returns immediately (bash: "wait: pid ... is not a child of this
    # shell"), which is exactly the bug an early live run of this harness hit
    # -- both boots "completed" in ~8s total, far short of even one
    # DURATION, because every `wait` was a no-op. Mirrors
    # run_dlm_harness_h10b.sh's launch_node/LAUNCH_PID pattern.
    local node="$1" mac="$2" dur="$3" target="$4" group="$5" port="$6" out="$7"
    local append="$CONSOLE net.ifnames=0 biosdevname=0 panic=-1 loglevel=4 ovmx.node=${node} ovmx.duration=${dur}"
    if [ -n "$target" ]; then
        append="$append ovmx.dlmtarget=${target}"
    fi
    # ttyS0=console(file), ttyS1=node verdict log(file).
    $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "$append" \
        -m 512M -smp 1 -nographic -no-reboot -nodefaults \
        -netdev "socket,id=net0,mcast=${group}:${port},localaddr=127.0.0.1" \
        -device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
        -serial "file:$out/node${node}.console.log" \
        -serial "file:$out/node${node}.ttyS1.log" \
        >/dev/null 2>&1 &
    LAUNCH_PID=$!
}

run_boot() {
    # $1 = run label ("refuse"|"grant"), $2 = node A's dlmtarget csid,
    # $3 = mcast group, $4 = mcast port (DISTINCT per run so a slow-to-exit
    # QEMU from run 1 can never leak traffic into run 2).
    local label="$1" target="$2" group="$3" port="$4"
    local out="$OUT_BASE/$label"
    mkdir -p "$out"

    echo "--- run '$label': node A targets CSID=$target -- booting node A ---"
    launch_node A "$MAC_A" "$DURATION" "$target" "$group" "$port" "$out"; PA=$LAUNCH_PID
    sleep 2
    echo "--- run '$label': booting node B (csid=$CSID_B) ---"
    launch_node B "$MAC_B" "$DURATION" "" "$group" "$port" "$out"; PB=$LAUNCH_PID
    sleep 2
    echo "--- run '$label': booting node C (csid=$CSID_C) ---"
    launch_node C "$MAC_C" "$DURATION" "" "$group" "$port" "$out"; PC=$LAUNCH_PID

    ( sleep "$WALL"; kill -9 "$PA" "$PB" "$PC" 2>/dev/null ) &
    GUARD=$!
    wait "$PA" 2>/dev/null
    wait "$PB" 2>/dev/null
    wait "$PC" 2>/dev/null
    kill "$GUARD" 2>/dev/null

    echo ""
    for N in A B C; do
        echo "=== run '$label' node $N log (ttyS1) ==="
        cat "$out/node${N}.ttyS1.log" 2>/dev/null || echo "(none)"
        echo ""
    done
}

run_boot refuse "$CSID_B" 230.0.0.15 16015
run_boot grant  "$CSID_C" 230.0.0.16 16016

# --- verdict -----------------------------------------------------------------
REFUSE_OUT="$OUT_BASE/refuse"
GRANT_OUT="$OUT_BASE/grant"

member_reached() { grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$1" 2>/dev/null; }

extract_field() {  # $1 log, $2 marker (grep -F), $3 field name
    grep -a "$2" "$1" 2>/dev/null | head -1 \
        | sed -n "s/.*[[:space:]]$3=\([0-9A-Za-z]*\).*/\1/p" | tr 'a-f' 'A-F'
}

FAIL=0

echo ""
echo "--- (1) 3-way join precondition, BOTH runs ---"
for RUN in "refuse:$REFUSE_OUT" "grant:$GRANT_OUT"; do
    label=${RUN%%:*}; out=${RUN#*:}
    for N in A B C; do
        if member_reached "$out/node${N}.ttyS1.log"; then
            echo "  run $label node $N: VAXCLMEMBER reached"
        else
            echo "  FAIL: run $label node $N never reached VAXCLMEMBER"
            FAIL=1
        fi
    done
done

LA_REFUSE="$REFUSE_OUT/nodeA.ttyS1.log"
LA_GRANT="$GRANT_OUT/nodeA.ttyS1.log"

echo ""
echo "--- (2) run 'refuse': node B (non-directory) must REFUSE ---"
REFUSED_PRESENT=0; grep -qa 'SCSD-I-E84REFUSED' "$LA_REFUSE" 2>/dev/null && REFUSED_PRESENT=1
REFUSED_STATUS=$(extract_field "$LA_REFUSE" 'SCSD-I-E84REFUSED' status)
REFUSED_CSID=$(extract_field "$LA_REFUSE" 'SCSD-I-E84REFUSED' csid)
echo "  SCSD-I-E84REFUSED: present=$REFUSED_PRESENT csid=${REFUSED_CSID:-<none>} status=${REFUSED_STATUS:-<none>}"
grep -a 'SCSD-I-DLMDONE,\|SCSD-I-E84REFUSED\|SCSD-W-E84UNEXPECTED' "$LA_REFUSE" 2>/dev/null | sed 's/^/    /'
if [ "$REFUSED_PRESENT" != 1 ]; then
    echo "  FAIL: node A never emitted SCSD-I-E84REFUSED in run 'refuse' -- the round-trip"
    echo "        to B never completed, or B's status was something other than"
    echo "        SS\$_NORMAL / SS\$_UNSUPPORTED. See the DLMDONE/E84UNEXPECTED line above."
    FAIL=1
else
    [ "$REFUSED_CSID" = "$CSID_B" ] || { echo "  FAIL: E84REFUSED csid=$REFUSED_CSID, expected B ($CSID_B)"; FAIL=1; }
    [ "$REFUSED_STATUS" = "0x000008F8" ] || { echo "  FAIL: E84REFUSED status=$REFUSED_STATUS, expected SS\$_UNSUPPORTED (0x000008F8)"; FAIL=1; }
fi

echo ""
echo "--- (3) run 'grant': node C (RES_C's real directory) must GRANT ---"
GRANTED_PRESENT=0; grep -qa 'SCSD-I-E84GRANTED' "$LA_GRANT" 2>/dev/null && GRANTED_PRESENT=1
GRANTED_STATUS=$(extract_field "$LA_GRANT" 'SCSD-I-E84GRANTED' status)
GRANTED_CSID=$(extract_field "$LA_GRANT" 'SCSD-I-E84GRANTED' csid)
GRANTED_LKID=$(extract_field "$LA_GRANT" 'SCSD-I-E84GRANTED' lkid)
echo "  SCSD-I-E84GRANTED: present=$GRANTED_PRESENT csid=${GRANTED_CSID:-<none>} status=${GRANTED_STATUS:-<none>} lkid=${GRANTED_LKID:-<none>}"
grep -a 'SCSD-I-DLMDONE,\|SCSD-I-E84GRANTED\|SCSD-W-E84UNEXPECTED' "$LA_GRANT" 2>/dev/null | sed 's/^/    /'
if [ "$GRANTED_PRESENT" != 1 ]; then
    echo "  FAIL: node A never emitted SCSD-I-E84GRANTED in run 'grant' -- the round-trip"
    echo "        to C never completed, or C's status was something other than"
    echo "        SS\$_NORMAL / SS\$_UNSUPPORTED. See the DLMDONE/E84UNEXPECTED line above."
    FAIL=1
else
    [ "$GRANTED_CSID" = "$CSID_C" ] || { echo "  FAIL: E84GRANTED csid=$GRANTED_CSID, expected C ($CSID_C)"; FAIL=1; }
    [ "$GRANTED_STATUS" = "0x00000001" ] || { echo "  FAIL: E84GRANTED status=$GRANTED_STATUS, expected SS\$_NORMAL (0x00000001)"; FAIL=1; }
    if [ -z "$GRANTED_LKID" ] || [ "$GRANTED_LKID" = "0x00000000" ]; then
        echo "  FAIL: E84GRANTED lkid=${GRANTED_LKID:-<none>} -- expected a nonzero real lock id"
        FAIL=1
    fi
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS e84 PASSED: the CROSS-NODE DIRECTORY-OWNERSHIP REFUSAL proven."
    echo "  Node A's targeted cross-node \$ENQ EX on RES_C, sent to non-directory node B"
    echo "  (csid=$REFUSED_CSID), was REFUSED with the REAL executive status"
    echo "  SS\$_UNSUPPORTED ($REFUSED_STATUS) -- B did NOT self-master a resource it does"
    echo "  not own. The SAME \$ENQ, sent to RES_C's real directory node C"
    echo "  (csid=$GRANTED_CSID), was GRANTED (SS\$_NORMAL, $GRANTED_STATUS) with a real"
    echo "  master lock id (lkid=$GRANTED_LKID). INV-6: both statuses are the executive's"
    echo "  own dlm_resolve_master() decision, read verbatim off two separate boots of"
    echo "  the SAME image -- never fabricated. The H10b harness comment claiming"
    echo "  vms_lock_dlm_xnode_dispatch() 'masters unconditionally' is REFUTED on this rail."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS e84 FAILED"
    echo "=========================================="
    echo "--- run 'refuse' node A console tail ---"; tail -n 40 "$REFUSE_OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- run 'refuse' node B console tail ---"; tail -n 40 "$REFUSE_OUT/nodeB.console.log" 2>/dev/null || true
    echo "--- run 'grant'  node A console tail ---"; tail -n 40 "$GRANT_OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- run 'grant'  node C console tail ---"; tail -n 40 "$GRANT_OUT/nodeC.console.log" 2>/dev/null || true
    exit 1
fi
