#!/bin/busybox sh
# init_dlm_h3.sh - PID 1 inside a DLM harness H3 OVMX node (rd vms-209).
#
# H3 (DLM epic vms-7fa, harness rung 3 -- THE MILESTONE) takes H2's two joined
# real-/dev/vms QEMU nodes and, AFTER both reach SCSD-I-VAXCLMEMBER, drives node
# A to issue ONE cross-node $ENQ (OVMX_DLM_ENQ=RESONE) for a resource mastered
# on node B. Node B receives it over SCS, DISPATCHES it to its REAL executive
# (VMS_IOCTL_DLM_XNODE -> vms_lock_dlm_xnode_dispatch, the rung-1 stub), and
# returns the executive's status back to A on the same VC.
#
# WHAT H3 PROVES (the $ENQ REACHES B's real executive -- NOT a grant): node B's
# DLM dispatch returns SS$_UNSUPPORTED (2296 / 0x000008F8) -- the honest rung-1
# result of a LIVE /dev/vms lock manager that does not yet grant cross-node -- and
# specifically NOT SS$_NOSUCHDEV (2680 / 0x00000A78), which is what the OLD Docker
# harness returned because it had no executive at all (fail-honest). That 2296
# (reached-but-unsupported), never 2680, is the machine-checkable proof the
# cross-node $ENQ genuinely reached the mastering node's real executive. The
# ACTUAL grant (-> SS$_NORMAL) is H4/rung-2 (vms-e8f1) -- explicitly NOT here.
#
# H3 does NOT modify the executive: the stub still returns SS$_UNSUPPORTED for
# the ENQ case, which is the EXPECTED honest rung-1 behaviour. This init only
# arms node A's requester leg (OVMX_DLM_ENQ) and lifts the DLM round-trip markers
# out of each SCSD log so the host verdict can read them.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; SCSD prints verbatim what the executive returned; the host
# verdict reads B's dispatch status from B's own SCSD log -- never fabricates it.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H3: cross-node \$ENQ reaches B's REAL executive (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H3-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H3-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2/two-ovmx complete the join with) --
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H3 DELTA: arm node A's cross-node $ENQ requester leg -------------
# Only node A issues the $ENQ; the resource (RESONE) is mastered on node B, which
# runs a pure DLM server (no OVMX_DLM_ENQ) and answers with its executive's
# status. scsd_dlm_send_enq() is one-shot per peer and fires ONLY after the join
# to that peer is complete (JOINBOUND) and only with the member-role flag set --
# so it rides the SAME LIVE VMS$VAXcluster VC the join established. B's receive
# path (scsd_dlm_srv_msg_input) dispatches to /dev/vms unconditionally as part of
# the join, so B needs no extra env.
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_ENQ=RESONE
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RESONE (cross-node \$ENQ, resource mastered on B)"
else
    echo "join sequencer ON; node $NODE is the DLM SERVER for RESONE (dispatches \$ENQ to real executive)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H3-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
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
# Join ladder markers (so a join stall is visible), PLUS the DLM round-trip
# markers the host H3 verdict reads:
#   node A: SCSD-I-DLMENQ  (A SENT the cross-node $ENQ)
#           SCSD-I-DLMDONE (A got B's GRANT back -> round-trip complete, status)
#   node B: SCSD-I-DLMRX   (B RECEIVED the $ENQ + dispatched to its executive,
#                           carrying the executive status verbatim)
#           SCSD-I-DLMGRANT(B sent the status back on the VC)
# These lines are lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H3-NODE-$NODE-BEGIN==="
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
    echo "--- DLM cross-node \$ENQ round-trip markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMENQ' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMGRANT' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDONE' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H3-NODE-$NODE-END==="
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
