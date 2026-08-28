#!/bin/bash
# run_dlm_harness_h3.sh - host side (runs INSIDE the container built from
# tests/qemu/Dockerfile.dlm-harness-h3): boot TWO OVMX QEMU nodes, EACH with a
# real /dev/vms, wired on ONE shared L2 by a QEMU `socket` (mcast) netdev -- NO
# host bridge, NO privilege -- complete the full VMS$VAXcluster join, then drive
# node A's cross-node $ENQ (OVMX_DLM_ENQ=RESONE) and verdict that node B's DLM
# dispatch REACHED B's REAL executive (rd vms-209, DLM epic vms-7fa rung H3).
#
# H3 PASS iff, on top of the H2 join precondition (both nodes VAXCLMEMBER):
#   1. node A SENT the cross-node $ENQ            (SCSD-I-DLMENQ  in A's log)
#   2. node B RECEIVED it + dispatched to /dev/vms(SCSD-I-DLMRX   in B's log)
#   3. B's dispatch status == 2296 / 0x000008F8   (SS$_UNSUPPORTED, reached the
#      real rung-1 executive handler vms_lock_dlm_xnode_dispatch) and NOT
#      2680 / 0x00000A78 (SS$_NOSUCHDEV, which is what a MISSING executive / the
#      old Docker harness returned). THIS STATUS FLIP is the milestone.
#   4. B sent the status back + A completed the round-trip (SCSD-I-DLMGRANT in B,
#      SCSD-I-DLMDONE in A) -- proves the LIVE A->B->A SCS transport.
#
# INV-6 / Rule 9: the verdict READS B's status from B's own SCSD log; it never
# fabricates it, and 2680 is NOT accepted as a pass. H3 proves the $ENQ REACHES
# the executive; it does NOT make it grant (that is H4/vms-e8f1). A real GRANT
# (SS$_NORMAL) is out of scope here -- 2296 is the honest, expected win.

set -uo pipefail

DURATION="${H3_DURATION:-90}"
NETDEV="${H3_NETDEV:-mcast}"     # mcast (default) | sockpair
WALL="${H3_WALL_TIMEOUT:-600}"
OUT="${OUT_DIR:-/out}"
mkdir -p "$OUT"

# The honest cross-node statuses. Since the DLM rung-2 FOUNDATION GRANT landed
# (vms-e8f1), a compatible cross-node $ENQ is GRANTED: B's dispatch now returns
# 0x00000001 = 1 = SS$_NORMAL. H3 asserts that grant (it proves the $ENQ REACHED
# B's real executive AND was granted -- the reach H3 originally proved is now
# manifest as the grant). The two statuses that must NEVER pass: 0x000008F8 =
# 2296 = SS$_UNSUPPORTED (the pre-rung-2 decline) and 0x00000A78 = 2680 =
# SS$_NOSUCHDEV (fail-honest: no executive reached). H4 (run_dlm_harness_h4.sh)
# adds the held-lock-for-A's-CSID proof on top of this grant.
STATUS_NORMAL="0x00000001"        # 1  -- SS$_NORMAL (rung-2 grant)
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

echo "=== OVMX DLM Harness H3 Runner (vms-209) ==="
echo "arch=$ARCH qemu=$QEMU accel=${MACHINE#-accel } netdev=$NETDEV duration=${DURATION}s wall=${WALL}s"
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1; node A armed OVMX_DLM_ENQ=RESONE"
echo "milestone: node B's cross-node \$ENQ dispatch REACHES the real executive -> status ${STATUS_UNSUPPORTED} (2296), NOT ${STATUS_NOSUCHDEV} (2680)"
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
        echo "FATAL: unknown H3_NETDEV='$NETDEV' (want mcast|sockpair)"; exit 2
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
#    dispatch status VERBATIM from B's SCSD-I-DLMRX line (".. -> executive
#    status=0x........"). This is THE machine-checkable value.
B_RECEIVED_ENQ=0
grep -qa 'SCSD-I-DLMRX' "$LB" 2>/dev/null && B_RECEIVED_ENQ=1
B_DISPATCH_STATUS=$(grep -a 'SCSD-I-DLMRX' "$LB" 2>/dev/null \
    | sed -n 's/.*executive status=\(0x[0-9A-Fa-f]\{8\}\).*/\1/p' | head -1)
# Normalise to upper-case for the compare.
B_DISPATCH_STATUS=$(printf '%s' "$B_DISPATCH_STATUS" | tr 'a-f' 'A-F')

# 3. node B sent the status back; node A completed the round-trip.
B_SENT_GRANT=0; A_ROUNDTRIP=0
grep -qa 'SCSD-I-DLMGRANT' "$LB" 2>/dev/null && B_SENT_GRANT=1
grep -qa 'SCSD-I-DLMDONE' "$LA" 2>/dev/null && A_ROUNDTRIP=1

# The status the round-trip carried back to A (must equal B's dispatch status).
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
echo "  B_RECEIVED_ENQ=$B_RECEIVED_ENQ   (node B received it over SCS + dispatched to /dev/vms)"
echo "  B_DISPATCH_STATUS=${B_DISPATCH_STATUS:-<none>}   (want ${STATUS_NORMAL}=1 SS\$_NORMAL/GRANTED; NOT ${STATUS_UNSUPPORTED}=2296, NOT ${STATUS_NOSUCHDEV}=2680)"
echo "  B_SENT_GRANT=$B_SENT_GRANT  A_ROUNDTRIP=$A_ROUNDTRIP  A_ROUNDTRIP_STATUS=${A_ROUNDTRIP_STATUS:-<none>}"
echo ""

FAIL=0
[ "$A_VAXCLMEMBER" = 1 ] && [ "$B_VAXCLMEMBER" = 1 ] || { echo "  MISS: H2 join precondition not met on both nodes"; FAIL=1; }
[ "$A_SENT_ENQ" = 1 ]     || { echo "  MISS: node A did not send the cross-node \$ENQ (no SCSD-I-DLMENQ)"; FAIL=1; }
[ "$B_RECEIVED_ENQ" = 1 ] || { echo "  MISS: node B did not receive/dispatch the \$ENQ (no SCSD-I-DLMRX)"; FAIL=1; }
[ "$B_SENT_GRANT" = 1 ]   || { echo "  MISS: node B did not send its status back (no SCSD-I-DLMGRANT)"; FAIL=1; }
[ "$A_ROUNDTRIP" = 1 ]    || { echo "  MISS: node A did not complete the round-trip (no SCSD-I-DLMDONE)"; FAIL=1; }

# THE MILESTONE ASSERTION: B's real executive GRANTED the cross-node $ENQ
# (SS$_NORMAL), NOT the missing-executive fail-honest status (2680) and NOT the
# pre-rung-2 decline (2296).
if [ "$B_DISPATCH_STATUS" = "$STATUS_NOSUCHDEV" ]; then
    echo "  FAIL: node B dispatch returned ${STATUS_NOSUCHDEV} (2680 SS\$_NOSUCHDEV) --"
    echo "        the \$ENQ did NOT reach a real executive (device missing or REGISTER refused)."
    echo "        This is the exact H0 regression class: diagnose at the SCSD/transport layer."
    FAIL=1
elif [ "$B_DISPATCH_STATUS" = "$STATUS_UNSUPPORTED" ]; then
    echo "  FAIL: node B dispatch returned ${STATUS_UNSUPPORTED} (2296 SS\$_UNSUPPORTED) -- the \$ENQ"
    echo "        REACHED the executive but was NOT granted. The rung-2 grant path in"
    echo "        vms_lock_dlm_xnode_dispatch (src/kernel-core/vms_lock.c) did not fire."
    FAIL=1
elif [ "$B_DISPATCH_STATUS" != "$STATUS_NORMAL" ]; then
    echo "  FAIL: node B dispatch returned '${B_DISPATCH_STATUS:-<none>}' -- expected ${STATUS_NORMAL} (1 SS\$_NORMAL)."
    FAIL=1
fi

echo ""
echo "=========================================="
if [ "$FAIL" = 0 ]; then
    echo "  DLM HARNESS H3 PASSED: B dispatched cross-node \$ENQ to real executive rc=1 SS\$_NORMAL (GRANTED)"
    echo "  A_VAXCLMEMBER=1 B_VAXCLMEMBER=1  A_SENT_ENQ=1  B_RECEIVED_ENQ=1  B_DISPATCH_STATUS=${STATUS_NORMAL}"
    echo "  Node A issued a cross-node \$ENQ (RESONE) over the LIVE VMS\$VAXcluster VC the"
    echo "  join established; node B received it over SCS and DISPATCHED it to its REAL"
    echo "  /dev/vms executive (vms_lock_dlm_xnode_dispatch), which GRANTED it. B's status"
    echo "  is 1 (SS\$_NORMAL) -- the \$ENQ REACHED the mastering node's real executive and"
    echo "  was GRANTED (rung-2, vms-e8f1), NOT 2680 (SS\$_NOSUCHDEV, missing executive) and"
    echo "  NOT 2296 (SS\$_UNSUPPORTED, the pre-rung-2 decline). The A->B->A round-trip"
    echo "  completed. H4 (run_dlm_harness_h4.sh) adds the held-lock-for-A's-CSID proof."
    echo "=========================================="
    exit 0
else
    echo "  DLM HARNESS H3 FAILED"
    echo "=========================================="
    echo "--- node A console tail ---"; tail -n 40 "$OUT/nodeA.console.log" 2>/dev/null || true
    echo "--- node B console tail ---"; tail -n 40 "$OUT/nodeB.console.log" 2>/dev/null || true
    exit 1
fi
