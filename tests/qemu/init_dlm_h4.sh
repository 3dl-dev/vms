#!/bin/busybox sh
# init_dlm_h4.sh - PID 1 inside a DLM harness H4 OVMX node (rd vms-e8f1 / vms-17c).
#
# H4 (DLM epic vms-7fa, harness rung 4 -- THE CROWN: the real cross-node GRANT)
# takes H3's two joined real-/dev/vms QEMU nodes and, AFTER both reach
# SCSD-I-VAXCLMEMBER, drives node A to issue ONE cross-node $ENQ
# (OVMX_DLM_ENQ=RESONE) for a resource mastered on node B. Node B receives it over
# SCS, DISPATCHES it to its REAL executive (VMS_IOCTL_DLM_XNODE ->
# vms_lock_dlm_xnode_dispatch, the rung-2 FOUNDATION GRANT), which GRANTS it, and
# returns SS$_NORMAL back to A on the same VC.
#
# WHAT H4 PROVES (the GRANT, not just the reach -- H3's job):
#   (a) node A's response is SS$_NORMAL (0x00000001) -- a REAL cross-node grant,
#       the status flip 2296 (H3, reached-not-granted) -> 1 (H4, GRANTED); and
#   (b) node B's executive GENUINELY HOLDS the lock for A's cluster identity:
#       B reads its OWN resource DB back (GET_RESMASTER) and prints
#       SCSD-I-DLMHELD with found=1, is_local_master=1, n_granted=1, and
#       held_for_csid=<A's CSID> -- a REAL held-lock proof, not just the return
#       status. This is the "B holds a real lock for A's CSID" the H4 verdict reads.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; SCSD prints verbatim what the executive returned AND what its
# lock DB reports; the host verdict reads B's grant status + held-lock state from
# B's own SCSD log -- never fabricates it. The grant happens in the EXECUTIVE on a
# real /dev/vms; the SCSD-I-DLMHELD line is a READ of that real state.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H4: cross-node \$ENQ GRANTED by B's REAL executive (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H4-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H4-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2/H3 complete the join with) --------
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H4 DELTA: arm node A's cross-node $ENQ requester leg -------------
# Identical arming to H3 -- only node A issues the $ENQ; RESONE is mastered on
# node B, which runs a pure DLM server and now GRANTS the request through its real
# executive (rung 2). B's receive path (scsd_dlm_srv_msg_input) dispatches to
# /dev/vms unconditionally as part of the join, then -- on a grant -- reads its own
# lock DB back and prints SCSD-I-DLMHELD, so B needs no extra env.
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_ENQ=RESONE
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RESONE (cross-node \$ENQ, resource mastered on B)"
else
    echo "join sequencer ON; node $NODE is the DLM SERVER for RESONE (GRANTS \$ENQ in the real executive)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H4-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the $ENQ round-trip -
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers (so a join stall is visible), PLUS the DLM round-trip +
# GRANT markers the host H4 verdict reads:
#   node A: SCSD-I-DLMENQ  (A SENT the cross-node $ENQ)
#           SCSD-I-DLMDONE (A got B's GRANT back -> round-trip complete, status=1)
#   node B: SCSD-I-DLMRX   (B RECEIVED the $ENQ + dispatched to its executive ->
#                           status=0x00000001 GRANTED)
#           SCSD-I-DLMHELD (B's OWN lock DB: held_for_csid=<A> -- REAL held lock)
#           SCSD-I-DLMGRANT(B sent SS$_NORMAL back on the VC)
# These lines are lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H4-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    if [ "$NODE" = "A" ]; then
        echo "role=requester dlm_enq=RESONE"
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
    echo "--- DLM cross-node \$ENQ GRANT round-trip markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMENQ' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMHELD' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMGRANT' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDONE' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H4-NODE-$NODE-END==="
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
