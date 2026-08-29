#!/bin/busybox sh
# init_dlm_h6.sh - PID 1 inside a DLM harness H6 OVMX node (rd vms-76d).
#
# H6 (DLM epic vms-7fa, harness rung 6 -- THE BLKAST WIRE) is the SYMMETRIC MIRROR
# of H5's requester-side GRANT receive: a remote lock HOLDER genuinely RECEIVES a
# blocking-AST over SCS and FIRES it on its own executive. It layers on H5's
# two-node join + block-then-grant, driven from node A (OVMX_DLM_ENQ=RESONE
# OVMX_DLM_H6=1) for a resource mastered on node B:
#
#   1. node A holds RESONE EX (#1), granted by B's executive. Under H6, node A
#      ESTABLISHES its holder origin record WITH a blocking-AST routine
#      (SCSD-I-DLMHOLDARM) so it can receive a BLKAST for real.
#   2. node A sends a SECOND, incompatible $ENQ (#2); B QUEUES it on its real
#      waiting queue, WIREs the queued-reply (A's origin #2 -> PENDING,
#      SCSD-I-DLMPEND) AND -- because #2 blocks the remote holder #1 -- WIREs a
#      real BLKAST to node A (SCSD-I-DLMBLKSENT on B).
#   3. node A RECEIVES the BLKAST (SCSD-I-DLMBLKAST), dispatches it into ITS OWN
#      executive, which FIRES a genuine user-mode blocking AST on the holder's
#      process; A DRAINS it via DELIVERAST -- proof the AST really landed
#      (SCSD-I-DLMBLKFIRE on A). A then releases the holder (#1) with a real
#      cross-node $DEQ IN RESPONSE to the BLKAST (SCSD-I-DLMDEQ1).
#   4. B releases #1, GRANTS #2, and WIREs the deferred GRANT to A (SCSD-I-DLMDEFER
#      on B) -> A's origin #2 FLIPS NL->EX (SCSD-I-DLMH5FLIP on A).
#
# WHAT H6 PROVES (the BLKAST WIRE, on top of H5): the holder RELEASES because it
# RECEIVED a real master->holder BLKAST over the live SCS wire and the blocking AST
# genuinely FIRED on its executive -- NOT the H5 shortcut where the holder releases
# on its own. The AST delivery is read back with DELIVERAST (the exact registered
# astadr), never a log line.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; SCSD prints verbatim what its own executive returned; the host
# verdict reads B's SCSD-I-DLMBLKSENT and A's SCSD-I-DLMBLKFIRE from the nodes' own
# SCSD logs -- never fabricates them. The AST fires in A's EXECUTIVE on a real
# /dev/vms.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H6: BLKAST over the SCS wire (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H6-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
    poweroff -f
fi

# --- 2. the shared L2 NIC ----------------------------------------------------
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
sleep 2
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC"

# --- 3. this node's cluster identity (distinct SCSNODE/SCSSYSTEMID) -----------
STORE=/etc/ovmx/sysgen-$NODE.dat
if [ ! -f "$STORE" ]; then
    echo "  FAIL: identity store $STORE missing"
    echo "H6-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2/H3/H4/H5 complete the join with) ---
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H6 DELTA: arm node A's BLKAST-wire sequence ----------------------
# Only node A drives the sequence; RESONE is mastered on node B. OVMX_DLM_ENQ arms
# the first $ENQ; OVMX_DLM_H6 arms the holder-with-blkast + the block-then-grant
# sequence whose holder release is driven by a RECEIVED BLKAST. Node B runs a pure
# DLM server that GRANTS/QUEUES, WIREs the BLKAST to the holder, and WIREs the
# deferred GRANT -- it needs no extra env.
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_ENQ=RESONE
    export OVMX_DLM_H6=1
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RESONE OVMX_DLM_H6=1 (BLKAST wire)"
else
    echo "join sequencer ON; node $NODE is the DLM SERVER for RESONE (WIREs the BLKAST + deferred GRANT)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H6-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the H6 sequence ------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers PLUS the H6 wire markers the host verdict reads:
#   node A: SCSD-I-DLMENQ      (A sent the first $ENQ #1)
#           SCSD-I-DLMHOLDARM  (A established the holder origin WITH a blkast routine)
#           SCSD-I-DLMENQ2     (A sent the second, contending $ENQ #2)
#           SCSD-I-DLMPEND     (A's origin record for #2 is PENDING -- genuine block)
#           SCSD-I-DLMBLKAST   (A RECEIVED the BLKAST)
#           SCSD-I-DLMBLKFIRE  (A FIRED a real blocking AST, drained via DELIVERAST)
#           SCSD-I-DLMDEQ1     (A released the holder #1 in response to the BLKAST)
#           SCSD-I-DLMH5FLIP   (A's origin record for #2 FLIPPED NL->EX)
#   node B: SCSD-I-DLMRX       (B received + dispatched the cross-node requests)
#           SCSD-I-DLMBLKSENT  (B WIRED the BLKAST to the holder)
#           SCSD-I-DLMDEFER    (B WIRED the deferred GRANT off a real $DEQ)
# Lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H6-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    if [ "$NODE" = "A" ]; then
        echo "role=requester+holder dlm_enq=RESONE dlm_h6=1"
    else
        echo "role=dlm_server res=RESONE"
    fi
    echo "--- join ladder markers (chronological NEW->MEMBER) ---"
    for KEY in SCSD-I-HELLOSENT SCSD-I-DIRHELLO SCSD-I-STARTTX SCSD-I-STARTDONE \
               SCSD-I-VCOPEN SCSD-I-OWNDIRBOUND SCSD-I-MSCPBOUND \
               SCSD-I-CONNRESP SCSD-I-VAXCLMEMBER SCSD-I-CMCONFIG; do
        if grep -qa "$KEY" /tmp/scsd-$NODE.log; then
            echo "  REACHED $KEY"
        else
            echo "  ------- $KEY"
        fi
    done
    echo "--- VAXCLMEMBER (membership complete on this node) ---"
    grep -a 'SCSD-I-VAXCLMEMBER' /tmp/scsd-$NODE.log
    echo "--- DLM H6 BLKAST-wire markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMENQ' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMHOLDARM' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMENQ2' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMPEND' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMBLKSENT' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMBLKAST' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMBLKFIRE' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDEQ1' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDEFER' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMH5FLIP' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H6-NODE-$NODE-END==="
} > /dev/ttyS1 2>&1

# --- 7. pcap artifact, base64 on a dedicated UART ----------------------------
if [ -f /tmp/$NODE.pcap ]; then
    {
        echo "===PCAP-$NODE-B64-BEGIN==="
        base64 /tmp/$NODE.pcap
        echo "===PCAP-$NODE-B64-END==="
    } > /dev/ttyS2 2>&1
fi

sync
poweroff -f
