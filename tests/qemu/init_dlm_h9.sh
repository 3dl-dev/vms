#!/bin/busybox sh
# init_dlm_h9.sh - PID 1 inside a DLM harness H9 OVMX node (rd vms-eeb).
#
# H9 (DLM epic vms-7fa, harness rung 9 -- THE LVB READ CROSSING) is the exact
# MIRROR of H8's write crossing (vms-d81): node B (the master) HOLDS a known value
# block for a resource, and node A READS it back via a cross-node $ENQ over the
# GRANT wire. It layers on the same two-node join H2-H6 complete:
#
#   1. node B (master of RRD): SEEDS a KNOWN 16-byte value into RRD's resource
#      value block, at/after join and BEFORE serving A -- a LOCAL $ENQ RRD (EX,
#      LCK_M_VALBLK) with the known non-zero block as INPUT (the single-node core
#      writes it to res->valblk), then a LOCAL $DEQ RRD (LCK_M_VALBLK) so
#      res->valblk PERSISTS (the core keeps a resource whose LVB is non-zero).
#      Node B emits SCSD-I-DLMLVBSEED name=RRD val=<32-hex>.
#   2. node A (requester): does a cross-node $ENQ RRD (EX, LCK_M_VALBLK) to the
#      master B; B's grant path READS its res->valblk into the reply and the GRANT
#      frame carries the master's value block back. Node A dispatches the GRANT
#      into ITS OWN executive (grant_recv stores the LVB on the origin record) and
#      then GETLKIs its OWN request handle, reading args.valblk. Node A emits
#      SCSD-I-DLMLVBRD9 name=RRD val=<32-hex> -- the value it read back over the wire.
#
# WHAT H9 PROVES (the LVB READ crossing): the value block genuinely CROSSED THE
# WIRE B->A on the GRANT. The host verdict asserts a THREE-WAY equality --
# A_read == B_seed == the expected known pattern. A missing/dropped LVB leaves A
# reading zeros -> mismatch -> FAIL. The markers are the REAL executive bytes
# hex-encoded verbatim (scsd_hex16), never a fabricated echo (INV-6).
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; SCSD prints verbatim the LVB its own executive returned; the
# host verdict reads B's SEED and A's RD9 from the nodes' own SCSD logs -- never
# fabricates them. The value block is written/read in real EXECUTIVE state on a
# real /dev/vms.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H9: LVB READ crossing over the SCS wire (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H9-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H9-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2/H3/H4/H5/H6 complete the join with) --
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H9 DELTA: arm the LVB read crossing ------------------------------
# RRD is mastered on node B. Node B (OVMX_DLM_H9=1, NO OVMX_DLM_ENQ) SEEDS RRD's
# value block once at/after join, then serves A's cross-node $ENQ (the base server
# path carries the master's res->valblk back in the GRANT). Node A
# (OVMX_DLM_ENQ=RRD OVMX_DLM_H9=1) does the cross-node $ENQ (EX, VALBLK) and reads
# the master's LVB back with a LOCAL GETLKI on its own request handle.
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_ENQ=RRD
    export OVMX_DLM_H9=1
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RRD OVMX_DLM_H9=1 (LVB read)"
else
    export OVMX_DLM_H9=1
    echo "join sequencer ON; node $NODE is the DLM SERVER + MASTER of RRD (SEEDs the LVB, serves A)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H9-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the H9 sequence ------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers PLUS the H9 LVB markers the host verdict reads:
#   node A: SCSD-I-DLMENQ      (A sent the cross-node $ENQ, EX, VALBLK)
#           SCSD-I-DLMDONE     (the GRANT round-trip completed)
#           SCSD-I-DLMLVBRD9   (A read the master's LVB back: name=RRD val=<hex>)
#   node B: SCSD-I-DLMLVBSEED  (B seeded RRD's value block: name=RRD val=<hex>)
#           SCSD-I-DLMRX       (B received + dispatched the cross-node request)
#           SCSD-I-DLMGRANT    (B sent the GRANT carrying its LVB back)
# Lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H9-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    if [ "$NODE" = "A" ]; then
        echo "role=requester dlm_enq=RRD dlm_h9=1"
    else
        echo "role=dlm_server+master res=RRD dlm_h9=1 (seed)"
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
    echo "--- DLM H9 LVB-read-crossing markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMLVBSEED' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMENQ' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMGRANT' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDONE' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMLVBRD9' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H9-NODE-$NODE-END==="
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
