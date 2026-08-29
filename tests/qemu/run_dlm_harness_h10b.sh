#!/bin/bash
# run_dlm_harness_h10b.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h10b): boot THREE OVMX QEMU nodes A/B/C, EACH
# with a real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev
# -- NO host bridge, NO privilege -- complete the full VMS$VAXcluster join on all
# three, have node A hold a cross-node lock on RES_C (mastered by C), have node C
# DEPART GRACEFULLY (shorter duration), and verdict that node A's lock SURVIVES:
# it re-registers on RES_C's new directory master (node B), which reconstructs it
# from A's REAL origin state and VALUE-VERIFIES against what A sent (rd vms-dca9,
# DLM epic vms-7fa rung H10b, the REMASTER LOCK REBUILD).
#
# H10b PASS iff, on top of the H10a join + departure-ingress foundation:
#   1. ALL THREE nodes reach VAXCLMEMBER.
#   2. node A's cross-node $ENQ EX on RES_C (targeted at C) was GRANTED and
#      dispatched into A's own executive (SCSD-I-DLMHOLDOK, mode=EX) -- the
#      PRE-DEPARTURE HOLD that must survive.
#   3. node C departs gracefully; A and B each shrink their DLM membership
#      (SCSD-I-DLMDEPART csid=1032 found=1 live=2), same as H10a.
#   4. node A re-registers its RES_C lock on the new directory master
#      (SCSD-I-DLMREBUILDSENT csid=1031 lkid=0x00000001 mode=EX sent=1).
#   5. node B (the new master) dispatches the REBUILD into its own executive and
#      reads the rebuilt lock back (SCSD-I-DLMREBUILT found=1 n_granted>=1
#      holder_csid=1030 lkid=0x00000001 mode=EX).
#   6. THREE-WAY VALUE EQUALITY: A's sent lkid == B's read-back lkid, A's sent
#      mode == B's read-back mode, B's read-back holder_csid == A's own CSID,
#      and the mode is EX (never NL -- INV-6 refuses a zero/NL rebuild).
#
# INV-6 / Rule 9: the verdict READS the values the nodes' own SCSD emitted (the
# REAL executive-returned found/n_granted/holder_csid/lkid/mode), never
# fabricates them. The rebuild happens in node B's EXECUTIVE on real /dev/vms,
# reconstructed DIRECTLY from node A's real origin state transported over SCS.

set -uo pipefail

DURATION="${H10B_DURATION:-120}"       # node A/B lifetime
C_DURATION="${H10B_C_DURATION:-60}"    # node C lifetime (SHORTER -> departs first)
NETDEV="${H10B_NETDEV:-mcast}"         # mcast (default) | sockpair(unsupported for 3)
WALL="${H10B_WALL_TIMEOUT:-900}"
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

MAC_A=52:54:00:00:01:0a
MAC_B=52:54:00:00:01:0b
MAC_C=52:54:00:00:01:0c
GROUP=230.0.0.14     # DISTINCT from h6/h8 (230.0.0.12) and h9/h10 (230.0.0.13)
PORT=16014           # DISTINCT from h9 (16012) and h10 (16013)

echo "=== OVMX DLM Harness H10b Runner (rd vms-dca9) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV"
echo "durations: A/B=${DURATION}s  C=${C_DURATION}s (C departs first)  wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; all nodes OVMX_DLM_H10=1;"
echo "  node A OVMX_DLM_ENQ=RES_C OVMX_DLM_ENQ_CSID=1032 OVMX_DLM_H10B=1 OVMX_DLM_H10_RES=RES_C"
echo "  node B OVMX_DLM_H10B=1"
echo "milestone: A holds RES_C EX via C (SCSD-I-DLMHOLDOK); C departs gracefully;"
echo "           A re-registers the SAME lock on the new master B (SCSD-I-DLMREBUILDSENT);"
echo "           B reconstructs + reads it back (SCSD-I-DLMREBUILT), three-way value-verified"
echo ""

if [ "$NETDEV" != "mcast" ]; then
    echo "FATAL: H10b needs a shared-L2 mcast fabric for 3 nodes (H10B_NETDEV=mcast); got '$NETDEV'"
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

echo "--- booting node A (SCSNODE OVMXA csid=$CSID_A, mac=$MAC_A, holder) ---"
launch_node A "$MAC_A" "$DURATION"; PA=$LAUNCH_PID
sleep 2
echo "--- booting node B (SCSNODE OVMXB csid=$CSID_B, mac=$MAC_B, new master) ---"
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

# Generic "key=value" extractor: pulls the value up to the next whitespace after
# "<field>=" on the FIRST line matching <marker>. Hex fields (lkid) are
# uppercased to match every other DLM runner's convention (H9's lesson: EXPECT
# constants and extracted hex must compare on the SAME case).
extract_field() {  # $1 log, $2 marker (grep -F), $3 field name
    # Value runs of alnum only (never [^ ]*): several markers put the LAST
    # field directly against a closing paren with no space (H10b's own
    # "sent=%d)" -- measured, [^ ]* swallowed the ")" into the value and
    # broke the sent==1 comparison on the first live run).
    grep -a "$2" "$1" 2>/dev/null | head -1 \
        | sed -n "s/.*[[:space:]]$3=\([0-9A-Za-z]*\).*/\1/p" | tr 'a-f' 'A-F'
}

# node A: the pre-departure hold.
A_HOLDOK=0; grep -qa 'SCSD-I-DLMHOLDOK' "$LA" 2>/dev/null && A_HOLDOK=1
A_HOLD_MASTER=$(extract_field "$LA" 'SCSD-I-DLMHOLDOK' master_csid)
A_HOLD_MODE=$(extract_field "$LA" 'SCSD-I-DLMHOLDOK' mode)

# node A/B: the H10a departure-ingress foundation (regression check).
depart_field() {  # $1 log, $2 field name -> value from the csid=1032 DLMDEPART line
    grep -a 'SCSD-I-DLMDEPART' "$1" 2>/dev/null | grep -a 'csid=1032' | head -1 \
        | sed -n "s/.*[[:space:](]$2=\([0-9]\{1,\}\).*/\1/p"
}
A_DEPART_FOUND=$(depart_field "$LA" found); A_DEPART_LIVE=$(depart_field "$LA" live)
B_DEPART_FOUND=$(depart_field "$LB" found); B_DEPART_LIVE=$(depart_field "$LB" live)

# node A: the rebuild send.
A_REBUILDSENT=0; grep -qa 'SCSD-I-DLMREBUILDSENT' "$LA" 2>/dev/null && A_REBUILDSENT=1
A_SENT_CSID=$(extract_field "$LA" 'SCSD-I-DLMREBUILDSENT' csid)
A_SENT_LKID=$(extract_field "$LA" 'SCSD-I-DLMREBUILDSENT' lkid)
A_SENT_MODE=$(extract_field "$LA" 'SCSD-I-DLMREBUILDSENT' mode)
A_SENT_FLAG=$(extract_field "$LA" 'SCSD-I-DLMREBUILDSENT' sent)

# node B: the rebuild receive + value-verify readback.
B_REBUILT=0; grep -qa 'SCSD-I-DLMREBUILT' "$LB" 2>/dev/null && B_REBUILT=1
B_FOUND=$(extract_field "$LB" 'SCSD-I-DLMREBUILT' found)
B_NGRANTED=$(extract_field "$LB" 'SCSD-I-DLMREBUILT' n_granted)
B_HOLDER_CSID=$(extract_field "$LB" 'SCSD-I-DLMREBUILT' holder_csid)
B_HOLDER_LKID=$(extract_field "$LB" 'SCSD-I-DLMREBUILT' lkid)
B_HOLDER_MODE=$(extract_field "$LB" 'SCSD-I-DLMREBUILT' mode)

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
echo "  node A  DLMHOLDOK      present=$A_HOLDOK master_csid=${A_HOLD_MASTER:-<none>} mode=${A_HOLD_MODE:-<none>}"
echo "  node A  DLMDEPART      found=${A_DEPART_FOUND:-<none>} live=${A_DEPART_LIVE:-<none>}"
echo "  node B  DLMDEPART      found=${B_DEPART_FOUND:-<none>} live=${B_DEPART_LIVE:-<none>}"
echo "  node A  DLMREBUILDSENT present=$A_REBUILDSENT csid=${A_SENT_CSID:-<none>} lkid=${A_SENT_LKID:-<none>}" \
     "mode=${A_SENT_MODE:-<none>} sent=${A_SENT_FLAG:-<none>}"
echo "  node B  DLMREBUILT     present=$B_REBUILT found=${B_FOUND:-<none>} n_granted=${B_NGRANTED:-<none>}" \
     "holder_csid=${B_HOLDER_CSID:-<none>} lkid=${B_HOLDER_LKID:-<none>} mode=${B_HOLDER_MODE:-<none>}"
echo ""

FAIL=0

# (1) 3-way join precondition.
[ "$A_MEM" = 1 ] || { echo "  FAIL: node A never reached VAXCLMEMBER (3-way join precondition)"; FAIL=1; }
[ "$B_MEM" = 1 ] || { echo "  FAIL: node B never reached VAXCLMEMBER (3-way join precondition)"; FAIL=1; }
[ "$C_MEM" = 1 ] || { echo "  FAIL: node C never reached VAXCLMEMBER (3-way join precondition)"; FAIL=1; }

# (2) node A's pre-departure hold: RES_C EX, mastered by C, genuinely GRANTED.
if [ "$A_HOLDOK" != 1 ]; then
    echo "  FAIL: node A never emitted SCSD-I-DLMHOLDOK -- the cross-node \$ENQ on RES_C"
    echo "        (targeted at C) was never GRANTED and dispatched into A's executive."
    FAIL=1
else
    [ "$A_HOLD_MASTER" = "$CSID_C" ] || { echo "  FAIL: DLMHOLDOK master_csid=$A_HOLD_MASTER, expected C ($CSID_C)"; FAIL=1; }
    [ "$A_HOLD_MODE" = "EX" ] || { echo "  FAIL: DLMHOLDOK mode=$A_HOLD_MODE, expected EX"; FAIL=1; }
fi

# (3) after C departs, A AND B shrank their DLM membership (H10a foundation).
for pair in "A:$A_DEPART_FOUND:$A_DEPART_LIVE" "B:$B_DEPART_FOUND:$B_DEPART_LIVE"; do
    n=${pair%%:*}; rest=${pair#*:}; f=${rest%%:*}; l=${rest#*:}
    if [ -z "$f" ] || [ -z "$l" ]; then
        echo "  FAIL: node $n emitted no SCSD-I-DLMDEPART for csid=$CSID_C -- it never saw C depart."
        FAIL=1
    else
        [ "$f" = 1 ] || { echo "  FAIL: node $n DLMDEPART found=$f, expected 1"; FAIL=1; }
        [ "$l" = 2 ] || { echo "  FAIL: node $n DLMDEPART live=$l, expected 2"; FAIL=1; }
    fi
done

# (4) node A sent the targeted rebuild to the new master (must be B, and must
# NOT be C -- a send back to the departed master proves nothing).
if [ "$A_REBUILDSENT" != 1 ]; then
    echo "  FAIL: node A never emitted SCSD-I-DLMREBUILDSENT."
    FAIL=1
else
    if [ "$A_SENT_CSID" = "$CSID_C" ]; then
        echo "  FAIL: DLMREBUILDSENT csid=$A_SENT_CSID still targets the DEPARTED node ($CSID_C)"
        FAIL=1
    elif [ "$A_SENT_CSID" != "$CSID_B" ]; then
        echo "  FAIL: DLMREBUILDSENT csid=$A_SENT_CSID is not the survivor ($CSID_B)"
        FAIL=1
    fi
    [ "$A_SENT_FLAG" = 1 ] || { echo "  FAIL: DLMREBUILDSENT sent=$A_SENT_FLAG -- the frame was not actually sent"; FAIL=1; }
    [ "$A_SENT_MODE" = "EX" ] || { echo "  FAIL: DLMREBUILDSENT mode=$A_SENT_MODE, expected EX"; FAIL=1; }
fi

# (5) node B received + rebuilt + read the lock back.
if [ "$B_REBUILT" != 1 ]; then
    echo "  FAIL: node B never emitted SCSD-I-DLMREBUILT -- the REBUILD frame was never"
    echo "        received, dispatched, or read back."
    FAIL=1
else
    [ "$B_FOUND" = 1 ] || { echo "  FAIL: DLMREBUILT found=$B_FOUND, expected 1 (a remote-held granted lock)"; FAIL=1; }
    if [ -z "$B_NGRANTED" ] || [ "$B_NGRANTED" -lt 1 ] 2>/dev/null; then
        echo "  FAIL: DLMREBUILT n_granted=${B_NGRANTED:-<none>}, expected >= 1"
        FAIL=1
    fi
fi

# (6) THE MILESTONE: three-way VALUE EQUALITY between what A sent and what B
# read back, never a vacuous "both markers appeared" pass.
if [ "$A_REBUILDSENT" = 1 ] && [ "$B_REBUILT" = 1 ]; then
    if [ -z "$A_SENT_LKID" ] || [ -z "$B_HOLDER_LKID" ]; then
        echo "  FAIL: could not extract both lkid values (A_SENT_LKID='${A_SENT_LKID:-}'"
        echo "        B_HOLDER_LKID='${B_HOLDER_LKID:-}')"
        FAIL=1
    elif [ "$A_SENT_LKID" != "$B_HOLDER_LKID" ]; then
        echo "  FAIL: A's sent lkid ($A_SENT_LKID) != B's read-back lkid ($B_HOLDER_LKID)"
        FAIL=1
    fi
    if [ -z "$A_SENT_MODE" ] || [ -z "$B_HOLDER_MODE" ]; then
        echo "  FAIL: could not extract both mode values"
        FAIL=1
    elif [ "$A_SENT_MODE" != "$B_HOLDER_MODE" ]; then
        echo "  FAIL: A's sent mode ($A_SENT_MODE) != B's read-back mode ($B_HOLDER_MODE)"
        FAIL=1
    fi
    if [ "$B_HOLDER_MODE" != "EX" ]; then
        echo "  FAIL: B's read-back mode is $B_HOLDER_MODE, expected EX (a zero/NL rebuild"
        echo "        is refused by the executive -- INV-6 -- so EX proves a real rebuild)"
        FAIL=1
    fi
    if [ -z "$B_HOLDER_CSID" ]; then
        echo "  FAIL: could not extract B's read-back holder_csid"
        FAIL=1
    elif [ "$B_HOLDER_CSID" != "$CSID_A" ]; then
        echo "  FAIL: B's read-back holder_csid ($B_HOLDER_CSID) != A's own CSID ($CSID_A) --"
        echo "        the rebuilt lock is not held for the node that actually holds it."
        FAIL=1
    fi
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H10b PASSED: the REMASTER LOCK REBUILD proven. Node A held RES_C EX"
    echo "  via its master C (SCSD-I-DLMHOLDOK); C departed gracefully; A and B shrank their"
    echo "  DLM membership 3->2 (SCSD-I-DLMDEPART, the H10a foundation). A re-read its OWN"
    echo "  granted mode fresh via GETLKI and re-resolved RES_C's directory to the survivor"
    echo "  B, then sent a TARGETED SCS_DLM_OP_REBUILD carrying its REAL req_lkid/mode/req_csid"
    echo "  (SCSD-I-DLMREBUILDSENT csid=$A_SENT_CSID). B dispatched it into its own executive --"
    echo "  which reconstructed the lock DIRECTLY into res->granted from A's real origin state"
    echo "  (vms_lock_dlm_xnode_rebuild) -- and read it back via VMS_IOCTL_DLM_GET_GRANTED"
    echo "  (SCSD-I-DLMREBUILT). THREE-WAY equality held: A_sent_lkid == B_readback_lkid =="
    echo "  $A_SENT_LKID, A_sent_mode == B_readback_mode == $A_SENT_MODE, and B's readback"
    echo "  holder_csid == A's own CSID ($CSID_A). INV-6: no fabricated lock -- the cross-node"
    echo "  lock genuinely SURVIVED the departure of its master."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H10b FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    echo "--- node C console tail ---"; tail -n 40 "$OUT/nodeC.console.log" 2>/dev/null || true
    exit 1
fi
