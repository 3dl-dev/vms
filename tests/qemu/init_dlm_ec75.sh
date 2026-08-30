#!/bin/busybox sh
# init_dlm_ec75.sh - PID 1 inside a DLM harness ec75 OVMX node (rd vms-ec75).
#
# ec75 proves DISTRIBUTED DEADLOCK DETECTION (DLM rung H11): a genuine cross-node
# wait-for CYCLE is detected by an edge-chasing search over REAL executive state,
# and EXACTLY ONE deterministic victim's queued $ENQ is aborted with SS$_DEADLOCK,
# breaking the cycle. It is a copy-extend of init_dlm_e84.sh: the SAME three nodes
# A/B/C, the SAME static 1030/1031/1032 DLM membership vector, the SAME join
# sequencer -- but instead of one targeted $ENQ, the two CONTENDERS (A, B) each
# establish one end of a cross-node cycle, and node C (which masters BOTH contended
# resources) initiates + drives the deadlock search.
#
# THE TOPOLOGY (2 nodes in the cycle; C is the neutral resource master, so both
# holds AND both waits are cross-node -- a genuine cross-node deadlock needs the
# blocker to be a REMOTE holder, which a 2-node cluster cannot arrange for both
# edges; RES_D and RES_E both hash to C=1032, the same hash-target fact e84 uses):
#
#   A HOLDS RES_D (EX, cross-node at master C), then $ENQs RES_E (EX) -> QUEUES
#     behind B's hold of RES_E.        (A waits-for B)
#   B HOLDS RES_E (EX, cross-node at master C), then $ENQs RES_D (EX) -> QUEUES
#     behind A's hold of RES_D.        (B waits-for A)
#   Edges: A ->(waits RES_E, held by B)-> B ->(waits RES_D, held by A)-> A. CYCLE.
#
# When A's WAIT queues, master C initiates DLKSRCH(SEARCH-HOLDER, chase B). B reads
# its pending wait (VMS_IOCTL_DLM_ENUM_WAITS, the HOME authority) -> forwards
# DLKSRCH(SEARCH-RESOURCE, RES_D) to C. C reads who holds RES_D
# (VMS_IOCTL_DLM_GET_GRANTED, the MASTER authority) -> A == the initiator -> CYCLE.
# The GLOBAL-min victim is A's request (CSID 1030 < 1031, deterministic); C aborts
# it (VMS_DLM_OP_DLKSRCH VICTIM) and wires GRANT(SS$_DEADLOCK) back to A's $ENQ. B's
# request stays QUEUED (not aborted). Exactly one SS$_DEADLOCK.
#
# WHAT ec75 PROVES: SCSD-I-DLKCYCLE (on C, the real cycle read off res->granted +
# pending origins) + SCSD-I-DLKVICTIM (on C, the abort; and on A, "our $ENQ returned
# SS$_DEADLOCK") -- and NO DLKVICTIM on B. INV-6 / Rule 9: nothing here fabricates a
# deadlock; every marker value is a REAL executive read, and with no /dev/vms the
# node FAILS loudly. A dropped/ttl-expired probe reports no deadlock, never a fake.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=110

case "$NODE" in
    A) MYCSID=1030 ;;
    B) MYCSID=1031 ;;
    C) MYCSID=1032 ;;
    *) MYCSID=0 ;;
esac

echo ""
echo "=== OVMX DLM Harness ec75: DISTRIBUTED DEADLOCK (node=$NODE csid=$MYCSID) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

if [ "$MYCSID" = "0" ]; then
    echo "  FAIL: unknown node label '$NODE' (want ovmx.node=A|B|C)"
    echo "EC75-NODE-$NODE: FAIL (unknown node label, no CSID mapping)" > /dev/ttyS1
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
    echo "EC75-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
    poweroff -f
fi

# --- 2. the shared L2 NIC (promiscuous: the SCS solicit is flooded to a group
# address the guest MAC never explicitly joins -- see init_dlm_e84.sh) --------
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
ip link set eth0 promisc on 2>/dev/null || ifconfig eth0 promisc 2>/dev/null
sleep 2
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC (promisc on)"

# --- 3. this node's cluster identity (SCSSYSTEMID == its DLM CSID) ------------
STORE=/etc/ovmx/sysgen-$NODE.dat
if [ ! -f "$STORE" ]; then
    echo "  FAIL: identity store $STORE missing"
    echo "EC75-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags every h5..e84 harness completes with) -
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1

# --- 3c. THE ec75 DELTA: arm each node's H11 role. RES_D and RES_E both master
# on C (1032). Contenders drive a timed hold-then-wait to C; C initiates the
# search. The initiator (A) waits LONGER than B before sending its WAIT so that,
# whatever the per-node join skew, B's PENDING wait-for edge exists before the
# search reaches it (else a SEARCH-HOLDER probe could dead-end on a false
# negative). Every value below is a lab switch; the detection logic is executive. -
export OVMX_DLM_EC75=1
if [ "$NODE" = "A" ]; then
    export OVMX_DLM_EC75_HOLD=RES_D          # A holds RES_D
    export OVMX_DLM_EC75_WAIT=RES_E          # A waits RES_E (held by B) -> A waits-for B
    export OVMX_DLM_EC75_MASTER=1032         # both resources are mastered on C
    export OVMX_DLM_EC75_WAIT_MS=20000       # initiator: send WAIT last (skew-dominating stagger)
    echo "join sequencer ON; node A ARMED (initiator): HOLD=RES_D WAIT=RES_E master=C"
elif [ "$NODE" = "B" ]; then
    export OVMX_DLM_EC75_HOLD=RES_E          # B holds RES_E
    export OVMX_DLM_EC75_WAIT=RES_D          # B waits RES_D (held by A) -> B waits-for A
    export OVMX_DLM_EC75_MASTER=1032
    export OVMX_DLM_EC75_WAIT_MS=6000        # non-initiator: establish its wait-for edge first
    echo "join sequencer ON; node B ARMED (contender): HOLD=RES_E WAIT=RES_D master=C"
else
    export OVMX_DLM_EC75_INIT=1030           # C designates A as the single search initiator
    echo "join sequencer ON; node C is the MASTER of RES_D + RES_E; INIT=A (1030) --"
    echo "  C initiates + drives the distributed deadlock search over its two authorities"
fi

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "EC75-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. the real SCS datalink daemon, driving the join + the ec75 sequence ----
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

# --- 5. emit the machine-checkable node log on ttyS1 -------------------------
{
    echo "===EC75-NODE-$NODE-BEGIN==="
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
    echo "--- DLM H11 markers (verbatim from SCSD, INV-6) ---"
    grep -a 'SCSD-I-DLKENQ,'    /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLKPEND,'   /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLKSRCH,'   /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLKCYCLE,'  /tmp/scsd-$NODE.log
    grep -a 'SCSD-I-DLKVICTIM,' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "===EC75-NODE-$NODE-END==="
} > /dev/ttyS1 2>&1

sync
poweroff -f
