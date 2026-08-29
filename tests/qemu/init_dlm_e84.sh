#!/bin/busybox sh
# init_dlm_e84.sh - PID 1 inside a DLM harness e84 OVMX node (rd vms-e84).
#
# e84 proves the CROSS-NODE DIRECTORY-OWNERSHIP REFUSAL: a node that is NOT a
# resource's directory must REFUSE a cross-node $ENQ for that resource (never
# self-master it), while the SAME resource $ENQ'd on its REAL directory node
# IS granted. It is a copy-extend of init_dlm_h10.sh / init_dlm_h10b.sh: the
# SAME three nodes A/B/C, the SAME static 1030/1031/1032 DLM membership
# vector, the SAME RES_C name (chosen so it hashes to the 3rd member, CSID
# 1032 = node C -- see init_dlm_h10.sh's hash-target comment) -- but instead
# of a departure/remaster sequence, node A sends ONE targeted cross-node $ENQ
# per boot (OVMX_DLM_ENQ_CSID), and this init script is booted TWICE by the
# runner with a DIFFERENT target:
#
#   run "refuse" (ovmx.dlmtarget=1031): A's ENQ for RES_C is targeted at B --
#       a NON-directory node for RES_C. B's receive handler dispatches into
#       ITS OWN executive (vms_lock_dlm_xnode_dispatch -> vms_enq_core_ex ->
#       dlm_resolve_master), which hashes RES_C's directory to C, sees
#       dir_csid(1032) != vms_local_csid(1031), and returns SS$_UNSUPPORTED
#       -- B refuses to master a resource it does not own. B wires that REAL
#       status back to A as a GRANT frame (status=SS$_UNSUPPORTED); A's
#       SCSD-I-DLMDONE prints it verbatim, and this script derives
#       SCSD-I-E84REFUSED from it.
#   run "grant" (ovmx.dlmtarget=1032): the SAME ENQ, targeted at C -- RES_C's
#       REAL directory. C's dlm_resolve_master sees dir_csid==vms_local_csid,
#       masters it (mastered-on-first-use), and grants (SS$_NORMAL). A's
#       SCSD-I-DLMDONE carries status=1 and a real master_lkid; this script
#       derives SCSD-I-E84GRANTED from it.
#
# No new SCS opcode, no new ioctl, no new scsd send site: this reuses the
# EXISTING scsd_dlm_send_enq / OVMX_DLM_ENQ_CSID targeting (added for H10b)
# and the EXISTING SCSD-I-DLMDONE round-trip marker (whose only change for
# vms-e84 is printing the master_lkid field it had already decoded off the
# wire). The E84REFUSED/E84GRANTED lines below are a DERIVATION of that real
# marker -- grep + relabel, never a fabricated value (INV-6).
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
[ -z "$DURATION" ] && DURATION=60
DLMTARGET=$(sed -n 's/.*ovmx.dlmtarget=\([0-9]*\).*/\1/p' /proc/cmdline)

# Map NODE -> this node's CSID. The 3-member vector is the SAME on all nodes; each
# node's SCSSYSTEMID (in its SYSGEN store, baked by the Dockerfile) equals its CSID.
case "$NODE" in
    A) MYCSID=1030 ;;
    B) MYCSID=1031 ;;
    C) MYCSID=1032 ;;
    *) MYCSID=0 ;;
esac

echo ""
echo "=== OVMX DLM Harness e84: directory-ownership refusal (node=$NODE csid=$MYCSID dlmtarget=${DLMTARGET:-<none>}) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

if [ "$MYCSID" = "0" ]; then
    echo "  FAIL: unknown node label '$NODE' (want ovmx.node=A|B|C)"
    echo "E84-NODE-$NODE: FAIL (unknown node label, no CSID mapping)" > /dev/ttyS1
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
    echo "E84-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
    poweroff -f
fi

# --- 2. the shared L2 NIC ----------------------------------------------------
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
# PROMISCUOUS: every other multi-node DLM harness (h5..h10b) runs sca_l2probe
# for pcap capture, whose PACKET_ADD_MEMBERSHIP/PACKET_MR_PROMISC join is a
# REQUIRED side effect, not just an artifact -- the SCS HELLO/DIRHELLO solicit
# is flooded to a group address the guest's own MAC never explicitly joins, so
# virtio-net silently drops it unless the interface is promiscuous. e84 has no
# need for a pcap artifact, so set IFF_PROMISC directly (the same kernel-level
# effect PACKET_MR_PROMISC produces) rather than building/shipping sca_l2probe.
# Measured: without this, HELLOSENT fires (our own outbound send) but
# DIRHELLO/VAXCLMEMBER never do -- every peer's reply is silently dropped.
ip link set eth0 promisc on 2>/dev/null || ifconfig eth0 promisc 2>/dev/null
sleep 2
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC (promisc on)"

# --- 3. this node's cluster identity (SCSSYSTEMID == its DLM CSID) ------------
STORE=/etc/ovmx/sysgen-$NODE.dat
if [ ! -f "$STORE" ]; then
    echo "  FAIL: identity store $STORE missing"
    echo "E84-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags every h5..h10b harness completes the
# join with) -- all three nodes, every run. ------------------------------------
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE e84 DELTA: node A only, targets its ENQ at THIS run's target ----
# RES_C's real directory is C (1032, the H10/H10b hash-target fact). A targeted
# send to B (1031) is a MIS-DIRECTED $ENQ -- the negative proof; a targeted
# send to C (1032) is the positive control. B and C need no special env: the
# receive-side directory check (dlm_resolve_master) is pure executive logic,
# unconditional on any harness flag.
if [ "$NODE" = "A" ]; then
    if [ -z "$DLMTARGET" ]; then
        echo "  FAIL: node A booted with no ovmx.dlmtarget=<csid>"
        echo "E84-NODE-$NODE: FAIL (no dlmtarget)" > /dev/ttyS1
        poweroff -f
    fi
    export OVMX_DLM_ENQ=RES_C
    export OVMX_DLM_ENQ_CSID="$DLMTARGET"
    echo "join sequencer ON; node A ARMED: OVMX_DLM_ENQ=RES_C OVMX_DLM_ENQ_CSID=$DLMTARGET" \
         "(targeted cross-node \$ENQ EX on RES_C at CSID=$DLMTARGET)"
else
    echo "join sequencer ON; node $NODE: no special DLM env -- receive-side directory"
    echo "  check (dlm_resolve_master) is unconditional executive logic"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "E84-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. the real SCS datalink daemon, driving the join + the e84 ENQ ---------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

# --- 5. emit the machine-checkable node log on ttyS1 -------------------------
# Join ladder markers PLUS the e84 markers the host verdict reads:
#   node A: SCSD-I-DLMENQ (the targeted send) / SCSD-I-DLMDONE (the real
#           round-trip status) / a DERIVED SCSD-I-E84REFUSED or
#           SCSD-I-E84GRANTED line (grep+relabel of DLMDONE -- see header).
#   node B/C: SCSD-I-DLMRX (the receive-side executive dispatch status) /
#           SCSD-I-DLMGRANT (the reply it wired back).
# Lifted VERBATIM from THIS node's SCSD stdout -- never synthesised, except
# the E84REFUSED/E84GRANTED derivation, which is a straight field-copy from
# the real DLMDONE line (see below).
{
    echo "===E84-NODE-$NODE-BEGIN==="
    echo "node=$NODE csid=$MYCSID mac=$MAC store=$STORE scsd_rc=$scsd_rc dlmtarget=${DLMTARGET:-<none>} member_vector=1030,1031,1032"
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
    echo "--- DLM e84 markers (verbatim from SCSD) ---"
    grep -a 'SCSD-I-DLMENQ,' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMRX,' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMGRANT,' /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLMDONE,' /tmp/scsd-$NODE.log
    if [ "$NODE" = "A" ]; then
        DONE_LINE=$(grep -a 'SCSD-I-DLMDONE,' /tmp/scsd-$NODE.log | head -1)
        if [ -n "$DONE_LINE" ]; then
            STATUS_HEX=$(echo "$DONE_LINE" | sed -n 's/.*status=\(0x[0-9A-Fa-f]*\).*/\1/p')
            LKID_HEX=$(echo "$DONE_LINE" | sed -n 's/.*master_lkid=\(0x[0-9A-Fa-f]*\).*/\1/p')
            echo "--- e84 derived marker (grep+relabel of the DLMDONE line above; SAME real status) ---"
            case "$STATUS_HEX" in
                0x00000001)
                    echo "SCSD-I-E84GRANTED csid=$DLMTARGET resnam=RES_C status=$STATUS_HEX lkid=$LKID_HEX"
                    ;;
                0x000008F8)
                    echo "SCSD-I-E84REFUSED csid=$DLMTARGET resnam=RES_C status=$STATUS_HEX"
                    ;;
                *)
                    echo "SCSD-W-E84UNEXPECTED csid=$DLMTARGET resnam=RES_C status=${STATUS_HEX:-<none>} -- neither" \
                         "SS\$_NORMAL(0x00000001) nor SS\$_UNSUPPORTED(0x000008F8); honest, not fabricated"
                    ;;
            esac
        else
            echo "--- e84 derived marker: NONE -- node A never saw SCSD-I-DLMDONE (round-trip never completed) ---"
        fi
    fi
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "===E84-NODE-$NODE-END==="
} > /dev/ttyS1 2>&1

sync
poweroff -f
