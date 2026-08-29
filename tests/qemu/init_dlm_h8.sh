#!/bin/busybox sh
# init_dlm_h8.sh - PID 1 inside a DLM harness H8 OVMX node (rd vms-d81).
#
# H8 (DLM epic vms-7fa, harness rung H8 -- THE LVB WIRE) proves cross-node
# lock-value-block replication: a remote HOLDER on node A writes the 16-byte LVB
# and releases with LCK_M_VALBLK; the MASTER (node B) replicates that wire value
# into its resource, and a LOCAL $ENQ on node B reads back exactly the bytes A
# wrote. It rides the SAME two-node join as H2-H6 (OVMX_MCAST_SOLICIT=1
# OVMX_JOIN_SEQ=1) for a resource (RLVB) mastered on node B:
#
#   1. node A: cross-node $ENQ RLVB EX (req_lkid 1), GRANTED by B's executive
#      (SCSD-I-DLMENQ + the GRANT round-trip).
#   2. node A: WRITE a known 16-byte value block ("OVMXLVB-RUNG6" + 3 NULs) and
#      release the holder with a cross-node $DEQ carrying LCK_M_VALBLK + that
#      block (SCSD-I-DLMLVBWR name=RLVB val=<32-hex>). scsd marshals the wire
#      valblk into req->valblk; the master's $DEQ handler replicates it into
#      res->valblk (vms_lock_dlm_xnode_deq, rd vms-d81).
#   3. node B (master): after dispatching A's $DEQ, does a LOCAL $ENQ RLVB EX with
#      LCK_M_VALBLK and reads args.valblk back (SCSD-I-DLMLVBRD name=RLVB
#      val=<32-hex>) -- the value A wrote, lifted from B's own executive.
#
# WHAT H8 PROVES (the LVB WIRE): a value block genuinely CROSSED the wire A->B and
# landed in the MASTER resource -- B's read == A's write == the known pattern. The
# READ is a real LOCAL $ENQ into B's executive, never a log echo.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; SCSD prints verbatim the bytes its own executive returned; the
# host verdict reads A's DLMLVBWR and B's DLMLVBRD from the nodes' own SCSD logs and
# asserts A_val == B_val == the expected pattern. A missing wire write would leave B
# reading zeros -> mismatch -> FAIL, never a vacuous pass.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H8: the LVB (lock value block) wire (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H8-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H8-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2-H6 complete the join with) ---------
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H8 DELTA: arm the LVB wire ---------------------------------------
# Node A is the remote HOLDER + LVB WRITER: OVMX_DLM_ENQ=RLVB arms the first $ENQ;
# OVMX_DLM_H8=1 arms the value-block write ($DEQ with LCK_M_VALBLK carrying the
# known pattern). Node B is the DLM SERVER/MASTER of RLVB; it ALSO carries
# OVMX_DLM_H8=1 so its server leg, after dispatching A's $DEQ, does the LOCAL $ENQ
# read-back. Only node A gets OVMX_DLM_ENQ (a single writer, one $ENQ on the wire).
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_ENQ=RLVB
    export OVMX_DLM_H8=1
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RLVB OVMX_DLM_H8=1 (LVB writer)"
else
    export OVMX_DLM_H8=1
    echo "join sequencer ON; node $NODE is the DLM SERVER/MASTER for RLVB (reads the LVB back with a LOCAL \$ENQ)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H8-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the H8 sequence ------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers PLUS the H8 LVB-wire markers the host verdict reads:
#   node A: SCSD-I-DLMENQ     (A sent the cross-node $ENQ for RLVB)
#           SCSD-I-DLMDONE    (the GRANT round-trip completed)
#           SCSD-I-DLMLVBWR   (A wrote the LVB + released with LCK_M_VALBLK)
#   node B: SCSD-I-DLMRX      (B received + dispatched the cross-node requests)
#           SCSD-I-DLMLVBRD   (B read the replicated LVB back with a LOCAL $ENQ)
# Lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H8-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    if [ "$NODE" = "A" ]; then
        echo "role=holder+lvb_writer dlm_enq=RLVB dlm_h8=1"
    else
        echo "role=dlm_server+lvb_reader res=RLVB dlm_h8=1"
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
    echo "--- DLM H8 LVB-wire markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMENQ' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDONE' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMLVBWR' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMLVBRD' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H8-NODE-$NODE-END==="
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
