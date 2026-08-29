#!/bin/bash
# run_dlm_harness_h10.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h10): boot THREE OVMX QEMU nodes A/B/C, EACH
# with a real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev --
# NO host bridge, NO privilege -- complete the full VMS$VAXcluster join on all
# three, then have node C DEPART GRACEFULLY (shorter duration) and verdict that
# the survivors' executives shrank the DLM directory membership and RES_C's
# directory deterministically remastered to a survivor (rd vms-2bf, DLM epic
# vms-7fa rung H10a, the graceful-departure directory INGRESS).
#
# H10a PASS iff, on top of the join precondition (ALL THREE nodes VAXCLMEMBER):
#   1. node A latched RES_C's directory over the full membership BEFORE departure
#      (SCSD-I-DLMDIRBEFORE) and it was C's CSID (dir_before == 1032).
#   2. after C departs, node A AND node B each emitted SCSD-I-DLMDEPART with
#      csid=1032, found=1 and live=2 (the DLM directory membership shrank 3->2).
#   3. node A emitted SCSD-I-DLMREMASTER with dir_before == 1032 (C) and
#      dir_after in {1030,1031} (a SURVIVOR) -- RES_C's directory DETERMINISTICALLY
#      re-resolved off the departed node onto a survivor.
#
# INV-6 / Rule 9: the verdict READS the values the nodes' own SCSD emitted (the
# REAL executive-returned found/live/dir), never fabricates them. The membership
# shrink + directory re-resolution happen in each node's EXECUTIVE on real
# /dev/vms; the departure is a real SCS class-0x04 self-departure. This is the
# INGRESS foundation only -- the cross-node lock-STATE rebuild is H10b (vms-dca9).

set -uo pipefail

DURATION="${H10_DURATION:-120}"       # node A/B lifetime
C_DURATION="${H10_C_DURATION:-60}"    # node C lifetime (SHORTER -> departs first)
NETDEV="${H10_NETDEV:-mcast}"         # mcast (default) | sockpair(unsupported for 3)
WALL="${H10_WALL_TIMEOUT:-900}"
OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

# The 3-member DLM CSID vector (== each node's SCSSYSTEMID). Node C departs.
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

MAC_A=52:54:00:00:00:0a
MAC_B=52:54:00:00:00:0b
MAC_C=52:54:00:00:00:0c
GROUP=230.0.0.13     # DISTINCT from h6/h8 (230.0.0.12) and h9 (230.0.0.12)
PORT=16013           # DISTINCT from h9 (16012)

echo "=== OVMX DLM Harness H10 Runner (vms-2bf) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV"
echo "durations: A/B=${DURATION}s  C=${C_DURATION}s (C departs first)  wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; all nodes OVMX_DLM_H10=1; node A OVMX_DLM_H10_RES=RES_C"
echo "milestone: C departs gracefully -> A & B shrink DLM membership (SCSD-I-DLMDEPART live=2 found=1);"
echo "           node A re-resolves RES_C's directory off C (1032) onto a survivor (SCSD-I-DLMREMASTER)"
echo ""

if [ "$NETDEV" != "mcast" ]; then
    echo "FATAL: H10 needs a shared-L2 mcast fabric for 3 nodes (H10_NETDEV=mcast); got '$NETDEV'"
    exit 2
fi

LAUNCH_PID=0
launch_node() {
    local node="$1" mac="$2" dur="$3"
    # ttyS0=console(file), ttyS1=node verdict log(file), ttyS2=pcap-b64(file).
    $QEMU $MACHINE \
        -kernel "$KERNEL" -initrd "$INITRD" \
        -append "$CONSOLE net.ifnames=0 biosdevname=0 panic=-1 loglevel=4 ovmx.node=${node} ovmx.duration=${dur}" \
        -m 512M -smp 1 -nographic -no-reboot -nodefaults \
        -netdev "socket,id=net0,mcast=${GROUP}:${PORT},localaddr=127.0.0.1" \
        -device "virtio-net-pci,netdev=net0,mac=${mac},romfile=" \
        -serial "file:$OUT/node${node}.console.log" \
        -serial "file:$OUT/node${node}.ttyS1.log" \
        -serial "file:$OUT/node${node}.pcap.b64" \
        >/dev/null 2>&1 &
    LAUNCH_PID=$!
}

echo "--- booting node A (SCSNODE OVMXA csid=$CSID_A, mac=$MAC_A, readback survivor) ---"
launch_node A "$MAC_A" "$DURATION"; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (SCSNODE OVMXB csid=$CSID_B, mac=$MAC_B, survivor) ---"
launch_node B "$MAC_B" "$DURATION"; PB=$LAUNCH_PID
sleep 2
echo "--- booting node C (SCSNODE OVMXC csid=$CSID_C, mac=$MAC_C, DEPARTS first, duration=$C_DURATION) ---"
launch_node C "$MAC_C" "$C_DURATION"; PC=$LAUNCH_PID

( sleep "$WALL"; kill -9 "$PA" "$PB" "$PC" 2>/dev/null ) &
GUARD=$!
wait "$PA" 2>/dev/null
wait "$PB" 2>/dev/null
wait "$PC" 2>/dev/null
kill "$GUARD" 2>/dev/null

echo ""
for N in A B C; do
    echo "=== node $N log (ttyS1) ==="; cat "$OUT/node${N}.ttyS1.log" 2>/dev/null || echo "(none)"
    echo ""
done

# Reconstruct the pcap artifacts.
for N in A B C; do
    B64="$OUT/node${N}.pcap.b64"
    if [ -s "$B64" ]; then
        sed -n '/===PCAP-'"$N"'-B64-BEGIN===/,/===PCAP-'"$N"'-B64-END===/p' "$B64" \
            | grep -v '===PCAP-' | tr -d '\r' | base64 -d > "$OUT/node${N}.pcap" 2>/dev/null || true
        [ -s "$OUT/node${N}.pcap" ] && \
            echo "reconstructed pcap: $OUT/node${N}.pcap ($(wc -c < "$OUT/node${N}.pcap") bytes)"
    fi
done

# --- verdict -----------------------------------------------------------------
LA="$OUT/nodeA.ttyS1.log"; LB="$OUT/nodeB.ttyS1.log"; LC="$OUT/nodeC.ttyS1.log"

member_reached() { grep -qa 'REACHED SCSD-I-VAXCLMEMBER\|SCSD-I-VAXCLMEMBER,' "$1" 2>/dev/null; }
A_MEM=0; B_MEM=0; C_MEM=0
member_reached "$LA" && A_MEM=1
member_reached "$LB" && B_MEM=1
member_reached "$LC" && C_MEM=1

# Field extractors: pull the REAL numeric values SCSD emitted (never fabricated here).
# SCSD-I-DLMDEPART csid=<n> live=<n> found=<n> ...
depart_field() {  # $1 log, $2 field name -> value from the csid=1032 DLMDEPART line
    grep -a 'SCSD-I-DLMDEPART' "$1" 2>/dev/null | grep -a 'csid=1032' | head -1 \
        | sed -n "s/.*[[:space:](]$2=\([0-9]\{1,\}\).*/\1/p"
}
# SCSD-I-DLMREMASTER name=RES_C dir_before=<n> dir_after=<n> ...
remaster_field() {  # $1 log, $2 field name
    grep -a 'SCSD-I-DLMREMASTER' "$1" 2>/dev/null | grep -a 'name=RES_C' | head -1 \
        | sed -n "s/.*[[:space:]]$2=\([0-9]\{1,\}\).*/\1/p"
}
# SCSD-I-DLMDIRBEFORE name=RES_C dir_before=<n> ...
dirbefore_field() {
    grep -a 'SCSD-I-DLMDIRBEFORE' "$1" 2>/dev/null | grep -a 'name=RES_C' | head -1 \
        | sed -n "s/.*[[:space:]]dir_before=\([0-9]\{1,\}\).*/\1/p"
}

A_DEPART_FOUND=$(depart_field "$LA" found)
A_DEPART_LIVE=$(depart_field "$LA" live)
B_DEPART_FOUND=$(depart_field "$LB" found)
B_DEPART_LIVE=$(depart_field "$LB" live)

A_DIRBEFORE=$(dirbefore_field "$LA")
A_REM_BEFORE=$(remaster_field "$LA" dir_before)
A_REM_AFTER=$(remaster_field "$LA" dir_after)

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
echo "highest join rung C : $(highest "$LC")"
echo ""
echo "verdict inputs:"
echo "  VAXCLMEMBER: A=$A_MEM B=$B_MEM C=$C_MEM   (3-way join precondition)"
echo "  node A  DLMDIRBEFORE dir_before : ${A_DIRBEFORE:-<none>}   (expect ${CSID_C})"
echo "  node A  DLMDEPART    found=${A_DEPART_FOUND:-<none>} live=${A_DEPART_LIVE:-<none>}"
echo "  node B  DLMDEPART    found=${B_DEPART_FOUND:-<none>} live=${B_DEPART_LIVE:-<none>}"
echo "  node A  DLMREMASTER  dir_before=${A_REM_BEFORE:-<none>} dir_after=${A_REM_AFTER:-<none>}"
echo "                       (expect dir_before=${CSID_C}, dir_after in {${CSID_A},${CSID_B}})"
echo ""

FAIL=0

# (1) 3-way join precondition -- a core part of the done-condition, NOT vacuous.
[ "$A_MEM" = 1 ] || { echo "  FAIL: node A never reached VAXCLMEMBER (3-way join precondition)"; FAIL=1; }
[ "$B_MEM" = 1 ] || { echo "  FAIL: node B never reached VAXCLMEMBER (3-way join precondition)"; FAIL=1; }
[ "$C_MEM" = 1 ] || { echo "  FAIL: node C never reached VAXCLMEMBER (3-way join precondition)"; FAIL=1; }

# (2) node A's before-read: RES_C's directory was C over the full membership.
if [ -z "$A_DIRBEFORE" ]; then
    echo "  FAIL: node A emitted no SCSD-I-DLMDIRBEFORE (the pre-departure readback baseline)"
    FAIL=1
elif [ "$A_DIRBEFORE" != "$CSID_C" ]; then
    echo "  FAIL: RES_C's directory before departure was $A_DIRBEFORE, expected C ($CSID_C) --"
    echo "        the test resource must hash to the departing node for a remaster to be observable."
    FAIL=1
fi

# (3) after C departs, A AND B shrank their DLM membership (found=1, live=2).
for pair in "A:$A_DEPART_FOUND:$A_DEPART_LIVE" "B:$B_DEPART_FOUND:$B_DEPART_LIVE"; do
    n=${pair%%:*}; rest=${pair#*:}; f=${rest%%:*}; l=${rest#*:}
    if [ -z "$f" ] || [ -z "$l" ]; then
        echo "  FAIL: node $n emitted no SCSD-I-DLMDEPART for csid=$CSID_C -- it never saw C depart."
        FAIL=1
    else
        [ "$f" = 1 ] || { echo "  FAIL: node $n DLMDEPART found=$f, expected 1 (C was a configured member)"; FAIL=1; }
        [ "$l" = 2 ] || { echo "  FAIL: node $n DLMDEPART live=$l, expected 2 (membership must shrink 3->2)"; FAIL=1; }
    fi
done

# (4) node A's remaster proof: dir_before=C, dir_after=a survivor.
if [ -z "$A_REM_BEFORE" ] || [ -z "$A_REM_AFTER" ]; then
    echo "  FAIL: node A emitted no complete SCSD-I-DLMREMASTER (the before/after directory proof)"
    FAIL=1
else
    [ "$A_REM_BEFORE" = "$CSID_C" ] || { echo "  FAIL: DLMREMASTER dir_before=$A_REM_BEFORE, expected C ($CSID_C)"; FAIL=1; }
    if [ "$A_REM_AFTER" = "$CSID_C" ]; then
        echo "  FAIL: DLMREMASTER dir_after=$A_REM_AFTER still points at the DEPARTED node ($CSID_C) --"
        echo "        the directory did NOT re-resolve off C."
        FAIL=1
    elif [ "$A_REM_AFTER" != "$CSID_A" ] && [ "$A_REM_AFTER" != "$CSID_B" ]; then
        echo "  FAIL: DLMREMASTER dir_after=$A_REM_AFTER is not a survivor ({$CSID_A,$CSID_B})"
        FAIL=1
    fi
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H10 PASSED: the GRACEFUL-DEPARTURE DIRECTORY INGRESS proven. All three"
    echo "  nodes A/B/C joined one VMScluster; node C departed gracefully (class-0x04 self-departure)."
    echo "  Nodes A and B each observed C's departure and called the LOCAL depart ioctl -- their"
    echo "  executives dropped C's CSID from the LIVE DLM directory membership (SCSD-I-DLMDEPART"
    echo "  csid=$CSID_C found=1 live=2, the membership shrank 3->2). Node A re-resolved RES_C's"
    echo "  directory, which hashed to C over the full set (dir_before=$CSID_C), onto a survivor"
    echo "  (dir_after=$A_REM_AFTER) -- SCSD-I-DLMREMASTER. INV-6: every value is the REAL executive"
    echo "  read, never fabricated. This is the H10a FOUNDATION (ingress + directory re-resolution);"
    echo "  the cross-node lock-STATE rebuild (COLLECT) is the H10b rung (vms-dca9)."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H10 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    echo "--- node C console tail ---"; tail -n 40 "$OUT/nodeC.console.log" 2>/dev/null || true
    exit 1
fi
