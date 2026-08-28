#!/bin/busybox sh
# init_dlm_h2.sh - PID 1 inside a DLM harness H2 OVMX node (rd vms-4bd0).
#
# H2 (DLM epic vms-7fa, harness rung 2) takes H1's two real-/dev/vms QEMU nodes
# -- joined on ONE shared L2 by a QEMU `socket` (mcast) netdev, NO host bridge,
# NO privilege -- and drives the FULL VMS$VAXcluster JOIN to completion, inside
# QEMU, each node on a real executive. Where H1 asserted only the 0x6007 LAVC/SCA
# HELLO transport, H2 turns on the SAME stop-and-wait join sequencer the Docker
# tests/cluster/two-ovmx harness uses -- OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1 --
# so each SCSD.EXE --connect climbs multicast-HELLO -> directed HELLO / NISCA
# channel -> 0x41 START / VC OPEN -> the 8-step join choreography (SCS$DIRECTORY
# -> MSCP$DISK -> VMS$VAXcluster connect) -> member-role 0x5b accept, and BOTH
# nodes reach SCSD-I-VAXCLMEMBER (cluster membership complete).
#
# WHAT H2 PROVES (JOIN / membership ONLY): both nodes reach VMS$VAXcluster SYSAP
# connection OPEN over the socket-netdev L2, same sequencer as Docker, each on a
# real /dev/vms. It deliberately does NOT drive any cross-node $ENQ into the
# executive (that is H3) or assert any DLM grant (that is H4) -- OVMX_DLM_ENQ is
# NOT set here. This init is symmetric; the node letter (A|B) selected by
# ovmx.node= on the kernel cmdline picks the identity store + is used for
# labeling. A passive sca_l2probe capture writes the pcap artifact.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms
# the node FAILS loudly; SCSD prints verbatim what the wire delivered, and the
# host verdict reads membership from each node's own SCSD log -- never fabricates
# it.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=90

echo ""
echo "=== OVMX DLM Harness H2: two-node VMS\$VAXcluster JOIN over a QEMU socket netdev (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H2-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
    poweroff -f
fi

# --- 2. the shared L2 NIC ----------------------------------------------------
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
sleep 2
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC"

# --- 3. this node's cluster identity (distinct SCSNODE/SCSSYSTEMID) -----------
# The two per-node SYSGEN stores are baked into the image (build time, from
# tests/cluster/two-ovmx/mk_sysgen_scratch.py). SCSD adopts SCSNODE/SCSSYSTEMID
# from OVMX_SYSGEN_PATH -- the same read-side adoption the two-ovmx harness uses.
STORE=/etc/ovmx/sysgen-$NODE.dat
if [ ! -f "$STORE" ]; then
    echo "  FAIL: identity store $STORE missing"
    echo "H2-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

# --- 3b. the join sequencer (SAME flags the Docker two-ovmx harness completes
#         the OVMX<->OVMX join with -- see tests/cluster/two-ovmx/evidence/
#         RUNG-ADD.md). MCAST_SOLICIT drives the member-role directed-HELLO +
#         0x41 START initiate; JOIN_SEQ drives the 8-step SCS$DIRECTORY ->
#         MSCP$DISK -> VMS$VAXcluster choreography to member-role accept.
#         NO OVMX_DLM_ENQ: H2 is JOIN/membership only, not the DLM round-trip.
export OVMX_MCAST_SOLICIT=1
export OVMX_JOIN_SEQ=1
echo "join sequencer: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1 (membership only; no DLM \$ENQ)"

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H2-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
# A second AF_PACKET/SOCK_RAW socket on the same iface gets its own copy of
# every 0x6007 frame, so it captures the whole join exchange (HELLO, START, the
# join-sequencer directed frames) without perturbing SCSD. Runs a hair longer
# than SCSD so it brackets the whole run.
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon, driving the full join ------------------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE, join sequencer ON) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# The host verdict (mirroring tests/cluster/two-ovmx/verdict.sh) reads the
# NEW->MEMBER ladder markers from THIS node's SCSD log. SCSD-I-VAXCLMEMBER on a
# node = that node reached VMS$VAXcluster SYSAP connection OPEN; the full-join
# oracle wants it on BOTH. The intermediate rungs are emitted too so a stall is
# visible in CI output.
{
    echo "===H2-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    echo "join_env=OVMX_MCAST_SOLICIT=1,OVMX_JOIN_SEQ=1"
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
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H2-NODE-$NODE-END==="
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
