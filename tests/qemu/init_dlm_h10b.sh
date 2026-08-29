#!/bin/busybox sh
# init_dlm_h10b.sh - PID 1 inside a DLM harness H10b OVMX node (rd vms-dca9).
#
# H10b (DLM epic vms-7fa, harness rung 10b -- THE REMASTER LOCK REBUILD) proves
# that a CROSS-NODE LOCK SURVIVES the departure of its master node. It is a
# copy-extend of init_dlm_h10.sh (H10a, the graceful-departure directory
# INGRESS): same THREE nodes A/B/C, same 1030/1031/1032 static DLM membership
# vector, same class-0x04 graceful departure of node C -- but layers a
# genuine HELD LOCK and its cross-node REBUILD on top of the ingress:
#
#   1. All three nodes join exactly as H10a does (SCSD-I-VAXCLMEMBER on all
#      three, the same static 3-member DLM vector, SCSSYSTEMID == DLM CSID).
#   2. Node A does a cross-node $ENQ EX on RES_C, TARGETED at node C (RES_C's
#      directory over the full 3-member set hashes to C -- the same fact H10a's
#      readback baseline uses). C grants it; A dispatches the GRANT into its own
#      executive, so A's origin record GENUINELY holds RES_C EX
#      (SCSD-I-DLMHOLDOK). THIS is the lock that must survive C's departure.
#   3. Node C departs GRACEFULLY (shorter duration + OVMX_LASTGASP=1, exactly
#      as H10a). Nodes A and B each run the H10a departure ingress (local
#      VMS_IOCTL_DLM_MEMBER_DEPART, membership 3->2).
#   4. Node A, having held RES_C via the now-departed C, re-reads its own
#      granted mode fresh (GETLKI) and RES_C's new directory master (the SAME
#      re-resolution H10a's readback proves -> a survivor, B) and sends a
#      TARGETED SCS_DLM_OP_REBUILD to B carrying its REAL req_lkid/mode/req_csid
#      (SCSD-I-DLMREBUILDSENT).
#   5. Node B dispatches the REBUILD into its own executive
#      (vms_lock_dlm_xnode_rebuild reconstructs the lock DIRECTLY into
#      res->granted from A's real state) and reads it back via
#      VMS_IOCTL_DLM_GET_GRANTED (SCSD-I-DLMREBUILT) -- the VALUE-VERIFY proof
#      the runner checks against what A sent.
#
# WHAT H10b PROVES: the lock A held on RES_C via its departed master C is NOT
# lost -- it is genuinely reconstructed on the resource's new master (B) from
# A's real origin state, never a fabricated or defaulted grant (INV-6). This is
# the FULL cross-node lock-state remaster; H10a is the departure-ingress
# foundation it builds on.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms
# the node FAILS loudly; every marker prints exactly what THIS node's executive
# returned; the host verdict reads them from the nodes' own SCSD logs.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

# Map NODE -> this node's CSID. The 3-member vector is the SAME on all nodes; each
# node's SCSSYSTEMID (in its SYSGEN store, baked by the Dockerfile) equals its CSID.
case "$NODE" in
    A) MYCSID=1030 ;;
    B) MYCSID=1031 ;;
    C) MYCSID=1032 ;;
    *) MYCSID=0 ;;
esac

echo ""
echo "=== OVMX DLM Harness H10b: remaster lock rebuild (node=$NODE csid=$MYCSID) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

if [ "$MYCSID" = "0" ]; then
    echo "  FAIL: unknown node label '$NODE' (want ovmx.node=A|B|C)"
    echo "H10B-NODE-$NODE: FAIL (unknown node label, no CSID mapping)" > /dev/ttyS1
    sync
    poweroff -f
fi

# --- 1. the real executive, loaded WITH the static 3-member vector -----------
echo "--- Loading vms.ko vms_local_csid=$MYCSID dlm_member_csids=1030,1031,1032 ---"
insmod /lib/modules/vms.ko vms_local_csid="$MYCSID" dlm_member_csids=1030,1031,1032
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H10B-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
    poweroff -f
fi

# --- 2. the shared L2 NIC ----------------------------------------------------
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
sleep 2
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC"

# --- 3. this node's cluster identity (SCSSYSTEMID == its DLM CSID) ------------
STORE=/etc/ovmx/sysgen-$NODE.dat
if [ ! -f "$STORE" ]; then
    echo "  FAIL: identity store $STORE missing"
    echo "H10B-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2-H10a complete the join with) ------
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H10a DELTA: arm the graceful-departure ingress (same as h10) ----
export OVMX_DLM_H10=1

# --- 3d. THE H10b DELTA: the pre-departure hold + remaster rebuild -----------
# Node A: hold RES_C via a TARGETED cross-node $ENQ at C (OVMX_DLM_ENQ_CSID
# pins the send to C's CSID -- in a 3-node cluster an untargeted send would
# also reach B, which would unconditionally (and wrongly) self-master a second
# copy of RES_C -- see scsd_dlm_send_enq's H10b comment). OVMX_DLM_H10B arms
# the hold-establish + rebuild-send drive. OVMX_DLM_H10_RES keeps emitting the
# H10a readback proof too (regression check: the foundation this rung builds
# on still works).
# Node B: OVMX_DLM_H10B arms the REBUILD receive + VMS_IOCTL_DLM_GET_GRANTED
# readback (SCSD-I-DLMREBUILT).
# Node C: departs first (shorter duration, class-0x04 self-departure).
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_H10_RES=RES_C
    export OVMX_DLM_ENQ=RES_C
    export OVMX_DLM_ENQ_CSID=1032
    export OVMX_DLM_H10B=1
    echo "join sequencer ON; node A ARMED: OVMX_DLM_H10=1 OVMX_DLM_H10_RES=RES_C" \
         "OVMX_DLM_ENQ=RES_C OVMX_DLM_ENQ_CSID=1032 OVMX_DLM_H10B=1" \
         "(holder: \$ENQ RES_C EX targeted at C, then rebuild onto the survivor)"
elif [ "$NODE" = "B" ]; then
    export OVMX_DLM_H10B=1
    echo "join sequencer ON; node B ARMED: OVMX_DLM_H10=1 OVMX_DLM_H10B=1" \
         "(survivor + RES_C's post-departure master: receives the REBUILD)"
else
    export OVMX_LASTGASP=1
    echo "join sequencer ON; node C ARMED: OVMX_DLM_H10=1 OVMX_LASTGASP=1" \
         "(emits class-0x04 self-departure; DEPARTS first, duration=$DURATION)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H10B-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the H10b sequence ---
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers PLUS the H10a + H10b markers the host verdict reads:
#   node A: SCSD-I-DLMDIRBEFORE / SCSD-I-DLMDEPART / SCSD-I-DLMREMASTER (H10a)
#           SCSD-I-DLMHOLDOK    (our pre-departure hold on RES_C is GRANTED)
#           SCSD-I-DLMREBUILDSENT (the targeted rebuild send to the new master)
#   node B: SCSD-I-DLMDEPART    (B saw C depart -> shrank its DLM membership)
#           SCSD-I-DLMRX / SCSD-I-DLMREBUILT (received + rebuilt + read back)
# Lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H10B-NODE-$NODE-BEGIN==="
    echo "node=$NODE csid=$MYCSID mac=$MAC store=$STORE scsd_rc=$scsd_rc member_vector=1030,1031,1032"
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
    echo "--- DLM H10a graceful-departure ingress markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMDIRBEFORE' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDEPART' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMREMASTER' /tmp/scsd-$NODE.log
    echo "--- DLM H10b remaster lock rebuild markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMHOLDOK' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMREBUILDSENT' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX, cross-node REBUILD' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMREBUILT' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H10B-NODE-$NODE-END==="
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
