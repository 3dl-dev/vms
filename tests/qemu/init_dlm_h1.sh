#!/bin/busybox sh
# init_dlm_h1.sh - PID 1 inside a DLM harness H1 OVMX node (rd vms-534).
#
# H1 (DLM epic vms-7fa, harness rung 1) is the TWO-node foundation that H0's
# single node could not be: TWO OVMX QEMU nodes, EACH with a REAL /dev/vms
# (vms.ko insmod'd, exactly as H0/tests/qemu/init.sh), joined on ONE shared L2
# by a QEMU `socket` (mcast) netdev -- NO host bridge, NO privilege (the make-or-
# break netdev proven by sca_l2probe / Dockerfile.dlm-harness-h1-derisk). Each
# node runs SCSD.EXE --connect --iface eth0, which multicasts a spec HELLO to
# AB-00-04-01-01-01 (ethertype 0x6007, DEC LAVC/SCA) and logs every 0x6007 frame
# it receives.
#
# WHAT H1 PROVES (transport/HELLO ONLY -- grants are H3/H4): EACH node emits its
# own HELLO (SCSD-I-HELLOSENT) AND receives the OTHER node's HELLO -- SCSD-I-
# FRAME with src = the peer's distinct HW MAC. That is the cross-node datalink
# the DLM (H2+) rides on. This init is symmetric; the node letter (A|B) selected
# by ovmx.node= on the kernel cmdline picks the identity store + is only used for
# labeling. A passive sca_l2probe capture writes the pcap artifact.
#
# INV-6 / Rule 9: nothing here fakes a pass. If vms.ko does not yield /dev/vms
# the node FAILS loudly; SCSD prints verbatim what the wire delivered.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
DURATION=$(sed -n 's/.*ovmx.duration=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$DURATION" ] && DURATION=15

echo ""
echo "=== OVMX DLM Harness H1: two-node SCS HELLO over a QEMU socket netdev (node=$NODE) ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# --- 1. the real executive ---------------------------------------------------
echo "--- Loading vms.ko ---"
insmod /lib/modules/vms.ko
if [ -c /dev/vms ]; then
    echo "  PASS: vms.ko loaded, /dev/vms present (char device)"
else
    echo "  FAIL: /dev/vms is NOT a char device -- no real executive present"
    dmesg | tail -20
    echo "H1-NODE-$NODE: FAIL (no /dev/vms)" > /dev/ttyS1
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
    echo "H1-NODE-$NODE: FAIL (no SYSGEN store)" > /dev/ttyS1
    poweroff -f
fi
export OVMX_SYSGEN_PATH="$STORE"

if [ ! -x /bin/SCSD.EXE ]; then
    echo "  FAIL: /bin/SCSD.EXE missing"
    echo "H1-NODE-$NODE: FAIL (no SCSD.EXE)" > /dev/ttyS1
    poweroff -f
fi

# --- 4. passive pcap capture of the 0x6007 wire (artifact) -------------------
# A second AF_PACKET/SOCK_RAW socket on the same iface gets its own copy of
# every 0x6007 frame, so it captures SCSD's HELLOs (ours + the peer's) without
# perturbing SCSD. Runs a hair longer than SCSD so it brackets the whole run.
if [ -x /bin/sca_l2probe ]; then
    sca_l2probe recv eth0 $((DURATION + 3)) /tmp/$NODE.pcap > /tmp/pcap.log 2>&1 &
    PCAP_PID=$!
    sleep 1
fi

# --- 5. the real SCS datalink daemon ----------------------------------------
echo "--- SCSD.EXE --connect --iface eth0 --duration $DURATION (node $NODE) ---"
SCSD.EXE --connect --iface eth0 --duration "$DURATION" > /tmp/scsd-$NODE.log 2>&1
scsd_rc=$?
echo "(SCSD.EXE exit code: $scsd_rc)"

[ -n "${PCAP_PID:-}" ] && wait "$PCAP_PID" 2>/dev/null

# --- 6. emit the machine-checkable node log on ttyS1 -------------------------
# HELLOSENT proves this node beaconed; every SCSD-I-FRAME src= line records a
# received 0x6007 frame -- the host verdict looks for one whose src is the PEER's
# MAC (self frames loop back too, so the src match is the cross-node proof).
{
    echo "===H1-NODE-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC store=$STORE scsd_rc=$scsd_rc"
    echo "--- HELLOSENT (this node's beacon) ---"
    grep -a 'SCSD-I-HELLOSENT' /tmp/scsd-$NODE.log
    echo "--- FRAMES received (0x6007) ---"
    grep -a 'SCSD-I-FRAME' /tmp/scsd-$NODE.log
    echo "--- IDENT ---"
    grep -a 'SCSD-I-IDENT' /tmp/scsd-$NODE.log
    echo "--- pcap capture summary ---"
    grep -a 'SCA-L2PROBE-DONE' /tmp/pcap.log 2>/dev/null
    echo "===H1-NODE-$NODE-END==="
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
