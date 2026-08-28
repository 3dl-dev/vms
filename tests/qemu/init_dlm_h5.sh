#!/bin/busybox sh
# init_dlm_h5.sh - PID 1 inside a DLM harness H5 OVMX node (rd vms-6ca).
#
# H5 (DLM epic vms-7fa, harness rung 5 -- THE ASYNC-REPLY SCS WIRE) takes H4's two
# joined real-/dev/vms QEMU nodes and, AFTER both reach SCSD-I-VAXCLMEMBER, drives
# node A's block-then-grant-over-the-wire sequence (OVMX_DLM_ENQ=RESONE
# OVMX_DLM_H5=1) for a resource mastered on node B:
#
#   1. node A holds RESONE EX (#1), granted by B's executive (the H4 grant).
#   2. node A sends a SECOND, incompatible $ENQ (#2); B QUEUES it on its real
#      waiting queue and WIREs a queued-reply -> A dispatches it into ITS OWN
#      executive, so #2's requester ORIGIN record is genuinely PENDING (GETLKI->NL,
#      SCSD-I-DLMPEND).
#   3. node A releases the holder (#1) with a real cross-node $DEQ (SCSD-I-DLMDEQ1).
#   4. B releases #1, GRANTS #2, and WIREs the deferred GRANT to A (SCSD-I-DLMDEFER
#      on B) -> A dispatches it into its executive and #2's origin record FLIPS
#      NL->EX (GETLKI->EX, SCSD-I-DLMH5FLIP on A).
#
# WHAT H5 PROVES (the WIRE, on top of H4's single cross-node grant): the status
# flip is observed on the REQUESTER node A, across the LIVE SCS wire, driven ONLY
# by what the master genuinely sent -- a real queued-reply and a real deferred
# GRANT off a real remote $DEQ. The BLKAST wire is deferred honestly (the holder
# releases on its own).
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; SCSD prints verbatim what its own executive returned via
# GETLKI on the origin record; the host verdict reads A's SCSD-I-DLMH5FLIP and B's
# SCSD-I-DLMDEFER from the nodes' own SCSD logs -- never fabricates them. The flip
# happens in A's EXECUTIVE on a real /dev/vms.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H5: block-then-grant over the SCS wire (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H5-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H5-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2/H3/H4 complete the join with) ------
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H5 DELTA: arm node A's block-then-grant-over-the-wire sequence ----
# Only node A drives the sequence; RESONE is mastered on node B. OVMX_DLM_ENQ arms
# the first $ENQ (identical to H4); OVMX_DLM_H5 arms the requester-side sequence
# (second $ENQ that queues, the $DEQ, and the origin-record flip). Node B runs a
# pure DLM server that GRANTS/QUEUES in its real executive and WIREs both the
# queued-reply and the deferred GRANT -- it needs no extra env.
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_ENQ=RESONE
    export OVMX_DLM_H5=1
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RESONE OVMX_DLM_H5=1 (block-then-grant over the wire)"
else
    echo "join sequencer ON; node $NODE is the DLM SERVER for RESONE (QUEUES + WIREs the deferred GRANT)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H5-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the H5 sequence ------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers (so a join stall is visible), PLUS the H5 wire markers the
# host verdict reads:
#   node A: SCSD-I-DLMENQ    (A sent the first $ENQ #1)
#           SCSD-I-DLMENQ2   (A sent the second, contending $ENQ #2)
#           SCSD-I-DLMPEND   (A's origin record for #2 is PENDING -- genuine block)
#           SCSD-I-DLMDEQ1   (A released the holder #1)
#           SCSD-I-DLMH5FLIP (A's origin record for #2 FLIPPED NL->EX -- THE PROOF)
#   node B: SCSD-I-DLMRX     (B received + dispatched the cross-node requests)
#           SCSD-I-DLMDEFER  (B WIRED the deferred GRANT off a real $DEQ)
# These lines are lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H5-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    if [ "$NODE" = "A" ]; then
        echo "role=requester dlm_enq=RESONE dlm_h5=1"
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
    echo "--- DLM H5 block-then-grant-over-the-wire markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMENQ' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMENQ2' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMPEND' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDEQ1' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDEFER' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMH5FLIP' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H5-NODE-$NODE-END==="
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
