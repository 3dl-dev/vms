#!/bin/bash
# run_dlm_harness_h4.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h4): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- complete the full VMS$VAXcluster join, then drive
# node A's cross-node $ENQ (OVMX_DLM_ENQ=RESONE) and verdict that node B's
# executive GRANTED it and GENUINELY HOLDS the lock for A (rd vms-e8f1 / vms-17c,
# DLM epic vms-7fa rung H4 -- THE CROWN).
#
# H4 PASS iff, on top of the H2 join precondition (both nodes VAXCLMEMBER):
#   1. node A SENT the cross-node $ENQ            (SCSD-I-DLMENQ  in A's log)
#   2. node B RECEIVED it + dispatched to /dev/vms(SCSD-I-DLMRX   in B's log)
#   3. B's dispatch status == 0x00000001          (SS$_NORMAL -- the executive
#      GRANTED the cross-node $ENQ; the H3->H4 status flip 2296 -> 1). NOT 2680
#      (SS$_NOSUCHDEV, missing executive) and NOT 2296 (SS$_UNSUPPORTED, the
#      rung-1 decline). THIS GRANT is the milestone.
#   4. ⭐ node B GENUINELY HOLDS the lock for A's cluster identity: B read its OWN
#      resource DB back and printed SCSD-I-DLMHELD with found=1, is_local_master=1,
#      n_granted>=1, and held_for_csid == A's CSID (the CSID B's DLMRX line shows
#      the $ENQ came from). A REAL held-lock proof, not just the return status.
#   5. B sent SS$_NORMAL back + A completed the round-trip (SCSD-I-DLMGRANT in B,
#      SCSD-I-DLMDONE status=1 in A) -- proves the LIVE A->B->A SCS transport.
#
# INV-6 / Rule 9: the verdict READS B's status + held-lock state from B's own SCSD
# log; it never fabricates them, and neither 2680 nor 2296 is accepted as a pass.
# The grant happens in the EXECUTIVE on a real /dev/vms; SCSD-I-DLMHELD is a READ
# of that real lock state (GET_RESMASTER), not a synthesised line.

set -uo pipefail

DURATION="${H4_DURATION:-90}"
NETDEV="${H4_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H4_WALL_TIMEOUT:-600}"
OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

# The GRANT status. 0x00000001 = 1 = SS$_NORMAL (the executive granted the
# cross-node $ENQ). The two honest NON-grant statuses that must NEVER pass H4:
# 0x000008F8 = 2296 = SS$_UNSUPPORTED (rung-1 reached-but-declined) and
# 0x00000A78 = 2680 = SS$_NOSUCHDEV (fail-honest: no executive reached).
STATUS_NORMAL="0x00000001"        # 1
STATUS_UNSUPPORTED="0x000008F8"   # 2296
STATUS_NOSUCHDEV="0x00000A78"     # 2680

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

echo "=== OVMX DLM Harness H4 Runner (vms-e8f1) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; node A armed OVMX_DLM_ENQ=RESONE"
echo "milestone: node B's executive GRANTS the cross-node \$ENQ -> status ${STATUS_NORMAL} (1 SS\$_NORMAL)"
echo "           and B GENUINELY HOLDS the lock for A's CSID (SCSD-I-DLMHELD held_for_csid)"
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
        echo "FATAL: unknown H4_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
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

echo "--- booting node A (SCSNODE OVMXA, mac=$MAC_A, requester) ---"
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

# 1. node A SENT the cross-node $ENQ.
A_SENT_ENQ=0
grep -qa 'SCSD-I-DLMENQ' "$LA" 2>/dev/null && A_SENT_ENQ=1

# 2. node B RECEIVED the $ENQ and dispatched it to the executive. Lift B's
#    dispatch status VERBATIM from B's SCSD-I-DLMRX line, and the CSID the $ENQ
#    came from (A's CSID) -- both are THE machine-checkable values.
B_RECEIVED_ENQ=0
grep -qa 'SCSD-I-DLMRX' "$LB" 2>/dev/null && B_RECEIVED_ENQ=1
B_DISPATCH_STATUS=$(grep -a 'SCSD-I-DLMRX' "$LB" 2>/dev/null \
    | sed -n 's/.*executive status=\(0x[0-9A-Fa-f]\{8\}\).*/\1/p' | head -1)
B_DISPATCH_STATUS=$(printf '%s' "$B_DISPATCH_STATUS" | tr 'a-f' 'A-F')
A_CSID=$(grep -a 'SCSD-I-DLMRX' "$LB" 2>/dev/null \
    | sed -n 's/.*from CSID=\([0-9]\{1,\}\).*/\1/p' | head -1)

# 3. ⭐ node B's HELD-LOCK PROOF: B read its own lock DB back (SCSD-I-DLMHELD).
B_HELD=0
grep -qa 'SCSD-I-DLMHELD' "$LB" 2>/dev/null && B_HELD=1
B_HELD_FOR=$(grep -a 'SCSD-I-DLMHELD' "$LB" 2>/dev/null \
    | sed -n 's/.*held_for_csid=\([0-9]\{1,\}\).*/\1/p' | head -1)
B_NGRANTED=$(grep -a 'SCSD-I-DLMHELD' "$LB" 2>/dev/null \
    | sed -n 's/.*n_granted=\([0-9]\{1,\}\).*/\1/p' | head -1)
B_ISLOCALMASTER=$(grep -a 'SCSD-I-DLMHELD' "$LB" 2>/dev/null \
    | sed -n 's/.*is_local_master=\([0-9]\{1,\}\).*/\1/p' | head -1)
B_FOUND=$(grep -a 'SCSD-I-DLMHELD' "$LB" 2>/dev/null \
    | sed -n 's/.*found=\([0-9]\{1,\}\).*/\1/p' | head -1)

# 4. node B sent the GRANT back; node A completed the round-trip.
B_SENT_GRANT=0; A_ROUNDTRIP=0
grep -qa 'SCSD-I-DLMGRANT' "$LB" 2>/dev/null && B_SENT_GRANT=1
grep -qa 'SCSD-I-DLMDONE' "$LA" 2>/dev/null && A_ROUNDTRIP=1
A_ROUNDTRIP_STATUS=$(grep -a 'SCSD-I-DLMDONE' "$LA" 2>/dev/null \
    | sed -n 's/.*GRANT status=\(0x[0-9A-Fa-f]\{8\}\).*/\1/p' | head -1)
A_ROUNDTRIP_STATUS=$(printf '%s' "$A_ROUNDTRIP_STATUS" | tr 'a-f' 'A-F')

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
echo "  A_SENT_ENQ=$A_SENT_ENQ   (node A issued the cross-node \$ENQ)"
echo "  B_RECEIVED_ENQ=$B_RECEIVED_ENQ  A_CSID=${A_CSID:-<none>}   (node B received it over SCS + dispatched to /dev/vms)"
echo "  B_DISPATCH_STATUS=${B_DISPATCH_STATUS:-<none>}   (want ${STATUS_NORMAL}=1 SS\$_NORMAL/GRANTED; NOT 2296, NOT 2680)"
echo "  B_HELD=$B_HELD  found=${B_FOUND:-<none>} is_local_master=${B_ISLOCALMASTER:-<none>} n_granted=${B_NGRANTED:-<none>} held_for_csid=${B_HELD_FOR:-<none>}"
echo "  B_SENT_GRANT=$B_SENT_GRANT  A_ROUNDTRIP=$A_ROUNDTRIP  A_ROUNDTRIP_STATUS=${A_ROUNDTRIP_STATUS:-<none>}"
echo ""

FAIL=0
[ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ] || { echo "  MISS: H2 join precondition not met on both nodes"; FAIL=1; }
[ "$A_SENT_ENQ" = 1 ]     || { echo "  MISS: node A did not send the cross-node \$ENQ (no SCSD-I-DLMENQ)"; FAIL=1; }
[ "$B_RECEIVED_ENQ" = 1 ] || { echo "  MISS: node B did not receive/dispatch the \$ENQ (no SCSD-I-DLMRX)"; FAIL=1; }
[ "$B_SENT_GRANT" = 1 ]   || { echo "  MISS: node B did not send its status back (no SCSD-I-DLMGRANT)"; FAIL=1; }
[ "$A_ROUNDTRIP" = 1 ]    || { echo "  MISS: node A did not complete the round-trip (no SCSD-I-DLMDONE)"; FAIL=1; }

# THE MILESTONE ASSERTION (a): B's executive GRANTED the cross-node $ENQ.
if [ "$B_DISPATCH_STATUS" = "$STATUS_NOSUCHDEV" ]; then
    echo "  FAIL: node B dispatch returned ${STATUS_NOSUCHDEV} (2680 SS\$_NOSUCHDEV) --"
    echo "        the \$ENQ did NOT reach a real executive (device missing or REGISTER refused)."
    FAIL=1
elif [ "$B_DISPATCH_STATUS" = "$STATUS_UNSUPPORTED" ]; then
    echo "  FAIL: node B dispatch returned ${STATUS_UNSUPPORTED} (2296 SS\$_UNSUPPORTED) --"
    echo "        the \$ENQ REACHED the executive but was NOT granted. That is H3's rung-1"
    echo "        result; H4 requires the rung-2 GRANT (SS\$_NORMAL). The grant path in"
    echo "        vms_lock_dlm_xnode_dispatch (src/kernel-core/vms_lock.c) did not fire."
    FAIL=1
elif [ "$B_DISPATCH_STATUS" != "$STATUS_NORMAL" ]; then
    echo "  FAIL: node B dispatch returned '${B_DISPATCH_STATUS:-<none>}' -- expected ${STATUS_NORMAL} (1 SS\$_NORMAL)."
    FAIL=1
fi

# THE MILESTONE ASSERTION (b): B GENUINELY HOLDS the lock for A's CSID.
[ "$B_HELD" = 1 ] || { echo "  MISS: node B did not print its held-lock proof (no SCSD-I-DLMHELD)"; FAIL=1; }
[ "${B_FOUND:-0}" = 1 ] || { echo "  MISS: B's DB does not show the resource mastered (found!=1)"; FAIL=1; }
[ "${B_ISLOCALMASTER:-0}" = 1 ] || { echo "  MISS: B is not the local master of the resource (is_local_master!=1)"; FAIL=1; }
[ "${B_NGRANTED:-0}" -ge 1 ] 2>/dev/null || { echo "  MISS: B's DB shows no granted lock (n_granted<1)"; FAIL=1; }
if [ -z "${B_HELD_FOR:-}" ] || [ "${B_HELD_FOR:-0}" = 0 ]; then
    echo "  FAIL: B's held lock is not attributed to any remote CSID (held_for_csid=${B_HELD_FOR:-<none>})"
    FAIL=1
elif [ -n "${A_CSID:-}" ] && [ "${B_HELD_FOR}" != "${A_CSID}" ]; then
    echo "  FAIL: B holds the lock for CSID ${B_HELD_FOR}, but node A's \$ENQ came from CSID ${A_CSID} -- mismatch"
    FAIL=1
fi

# The round-trip must carry the SAME grant status back to A.
if [ -n "${A_ROUNDTRIP_STATUS:-}" ] && [ "${A_ROUNDTRIP_STATUS}" != "$STATUS_NORMAL" ]; then
    echo "  FAIL: node A's round-trip status is ${A_ROUNDTRIP_STATUS}, expected ${STATUS_NORMAL} (SS\$_NORMAL)"
    FAIL=1
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H4 PASSED: cross-node \$ENQ GRANTED rc=SS\$_NORMAL, B holds lock for A's CSID"
    echo "  B_DISPATCH_STATUS=${STATUS_NORMAL}  held_for_csid=${B_HELD_FOR} (A_CSID=${A_CSID})  n_granted=${B_NGRANTED}  is_local_master=1"
    echo "  Node A issued a cross-node \$ENQ (RESONE) over the LIVE VMS\$VAXcluster VC the join"
    echo "  established; node B received it over SCS and DISPATCHED it to its REAL /dev/vms"
    echo "  executive (vms_lock_dlm_xnode_dispatch), which GRANTED it -- the status flip"
    echo "  2296 (H3, reached-not-granted) -> 1 (SS\$_NORMAL, GRANTED). B then read its OWN"
    echo "  resource DB back (GET_RESMASTER) and reported the lock GENUINELY HELD for A's"
    echo "  cluster identity (held_for_csid=A_CSID), not a fabricated status. The A->B->A"
    echo "  round-trip carried SS\$_NORMAL back to A. A now holds a REAL cross-node lock."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H4 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
