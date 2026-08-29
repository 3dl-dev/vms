#!/bin/busybox sh
# init_dlm_h10.sh - PID 1 inside a DLM harness H10 OVMX node (rd vms-2bf).
#
# H10a (DLM epic vms-7fa, harness rung 10 -- THE GRACEFUL-DEPARTURE DIRECTORY
# INGRESS) proves that when a cluster member LEAVES GRACEFULLY, the survivors'
# executives drop the departed CSID from the LIVE DLM directory membership and a
# resource whose directory hashed to the departed node DETERMINISTICALLY
# re-resolves to a survivor. It layers THREE nodes A/B/C on one shared-L2 mcast
# cluster (h9 was two, A/B) and combines the SCS join (h9) with a real static DLM
# membership vector (h7):
#
#   1. all THREE nodes insmod vms.ko WITH the SAME ordered 3-member vector
#      dlm_member_csids=1030,1031,1032 and their own vms_local_csid, and each
#      node's SCSSYSTEMID (SYSGEN store) is set EQUAL to its DLM CSID so a peer's
#      SCA src-logical (aa:00:04:00:<LE16(SCSSYSTEMID)>) carries its DLM CSID on
#      the wire. All three run the join sequencer and reach SCSD-I-VAXCLMEMBER.
#   2. node A (the readback survivor) latches, BEFORE any departure, the
#      directory CSID of RES_C over the full 3-member membership: RES_C is chosen
#      so exec_jhash(RES_C) % 3 == 2 -> the 3rd member (CSID 1032 = node C). Node
#      A emits SCSD-I-DLMDIRBEFORE name=RES_C dir_before=1032.
#   3. node C departs GRACEFULLY (a SHORTER ovmx.duration, so its scsd shuts down
#      and emits the class-0x04 self-departure last-gasp first). Nodes A and B
#      observe C's departure and each call the LOCAL depart ioctl
#      (VMS_IOCTL_DLM_MEMBER_DEPART) -> each emits SCSD-I-DLMDEPART csid=1032
#      live=2 found=1.
#   4. node A re-reads RES_C's directory over the now-shrunk 2-member set
#      (exec_jhash(RES_C) % 2 == 1 -> the 2nd survivor, CSID 1031 = node B) and
#      emits SCSD-I-DLMREMASTER name=RES_C dir_before=1032 dir_after=1031.
#
# WHAT H10a PROVES: the departure INGRESS (membership shrink + directory
# re-resolution) is real and DETERMINISTIC. It does NOT rebuild lock STATE across
# nodes -- collecting survivors' origin records and reconstructing res->granted is
# the H10b rung (vms-dca9). The markers are the REAL executive-returned values
# (found/live/dir), never fabricated (INV-6). The depart ingress is a LOCAL ioctl
# to this node's own /dev/vms -- NO new cross-node wire op (that is H10b).
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms the
# node FAILS loudly; the depart/remaster markers print exactly what THIS node's
# executive returned; the host verdict reads them from the nodes' own SCSD logs.

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
echo "=== OVMX DLM Harness H10: graceful-departure directory ingress (node=$NODE csid=$MYCSID) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

if [ "$MYCSID" = "0" ]; then
    echo "  FAIL: unknown node label '$NODE' (want ovmx.node=A|B|C)"
    echo "H10-NODE-$NODE: FAIL (unknown node label, no CSID mapping)" > /dev/ttyS1
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
    echo "H10-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H10-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags H2-H9 complete the join with) --------
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE H10 DELTA: arm the graceful-departure ingress -------------------
# All nodes arm the depart INGRESS (OVMX_DLM_H10): when scsd observes a peer's
# graceful class-0x04 self-departure it calls the LOCAL depart ioctl. Only node A
# additionally arms the READBACK (OVMX_DLM_H10_RES=RES_C): it latches RES_C's
# directory before the departure and re-reads it after, emitting the remaster
# proof. Node C is the one that DEPARTS (shorter duration, set by the runner).
export OVMX_DLM_H10=1
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_H10_RES=RES_C
    echo "join sequencer ON; node A ARMED: OVMX_DLM_H10=1 OVMX_DLM_H10_RES=RES_C (readback survivor)"
elif [ "$NODE" = "C" ]; then
    # C must emit the class-0x04 op-0x0d SELF-DEPARTURE CM open on shutdown (the
    # graceful departure the survivors' ingress hook listens for). That emit
    # (scsd_emit_clean_departure) is DEFAULT-OFF / opt-in via OVMX_LASTGASP=1 --
    # off by default because it crashes a real VAX, but safe + required between
    # OVMX nodes here. Without it C sends only the port-level last-gasp, which
    # does NOT carry the class-0x04 transition, so A/B never see the departure.
    export OVMX_LASTGASP=1
    echo "join sequencer ON; node C ARMED: OVMX_DLM_H10=1 OVMX_LASTGASP=1 (emits class-0x04 self-departure; DEPARTS first, duration=$DURATION)"
else
    echo "join sequencer ON; node $NODE ARMED: OVMX_DLM_H10=1 (survivor, observes C's departure)"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H10-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the join + the H10 sequence -----
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers PLUS the H10 markers the host verdict reads:
#   node A: SCSD-I-DLMDIRBEFORE (RES_C dir over full membership, before departure)
#           SCSD-I-DLMDEPART    (A saw C depart -> shrank its DLM membership)
#           SCSD-I-DLMREMASTER  (RES_C dir_before=1032 dir_after=1031)
#   node B: SCSD-I-DLMDEPART    (B saw C depart -> shrank its DLM membership)
# Lifted VERBATIM from THIS node's SCSD stdout -- never synthesised.
{
    echo "===H10-NODE-$NODE-BEGIN==="
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
    echo "--- DLM H10 graceful-departure ingress markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMDIRBEFORE' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDEPART' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMREMASTER' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H10-NODE-$NODE-END==="
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
