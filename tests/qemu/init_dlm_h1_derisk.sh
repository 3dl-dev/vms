#!/bin/busybox sh
# init_dlm_h1_derisk.sh - PID 1 for the H1 NETDEV DE-RISK guest (rd vms-534).
#
# The make-or-break question H1 must settle BEFORE the full two-node harness is
# built: does a QEMU `socket` (mcast / listen-connect) netdev faithfully FLOOD
# the 0x6007 group multicast (AB-00-04-01-01-01) between two guests, with NO
# host bridge and NO privilege? This guest answers it in isolation -- no vms.ko,
# no SCSD, no SYSGEN identity: just sca_l2probe on the guest's virtio-net NIC.
#
# Each guest simultaneously SENDS 0x6007 group-multicast frames (marker carries
# its node letter) and RECEIVES on the same ethertype, writing a pcap. If the
# netdev floods, each guest logs SCA-L2PROBE-PEER-OK with the OTHER guest's src
# MAC. That -- plus the pcap -- is the deliverable that settles the question.
#
# Results go to ttyS1 (the host runner reads them); the pcap goes base64 to
# ttyS2 (binary-clean off a dedicated UART). Nothing here fabricates a result:
# sca_l2probe prints verbatim what recvfrom() delivered.

/bin/busybox --install -s /bin

mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev

NODE=$(sed -n 's/.*ovmx.node=\([A-Za-z0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$NODE" ] && NODE=X
WINDOW=$(sed -n 's/.*ovmx.window=\([0-9]*\).*/\1/p' /proc/cmdline)
[ -z "$WINDOW" ] && WINDOW=12

echo ""
echo "=== OVMX DLM Harness H1 NETDEV DE-RISK: node=$NODE window=${WINDOW}s ==="
echo "Kernel: $(uname -r) ($(uname -m))"

# Bring up the one virtio-net NIC. cmdline carries net.ifnames=0 so it is eth0.
ip link set eth0 up 2>/dev/null || ifconfig eth0 up 2>/dev/null
# Give the link a moment to carry.
sleep 2
ip -o link show eth0 2>/dev/null || ifconfig eth0 2>/dev/null
MAC=$(cat /sys/class/net/eth0/address 2>/dev/null)
echo "eth0 mac=$MAC"

if [ ! -x /bin/sca_l2probe ]; then
    echo "H1DERISK-$NODE: FAIL (sca_l2probe missing from image)" > /dev/ttyS1
    poweroff -f
fi

# Receiver (writes the pcap) in the background for the whole window.
sca_l2probe recv eth0 "$WINDOW" /tmp/$NODE.pcap > /tmp/recv.log 2>&1 &
RPID=$!
sleep 1
# Sender: beacon for the whole window at 2 Hz so the two guests' windows overlap.
sca_l2probe send eth0 $((WINDOW * 2)) 500 "OVMX-H1-$NODE" > /tmp/send.log 2>&1 &
SPID=$!

wait "$RPID"
kill "$SPID" 2>/dev/null

# Emit the machine-checkable result block on ttyS1.
{
    echo "===H1DERISK-$NODE-BEGIN==="
    echo "node=$NODE mac=$MAC"
    cat /tmp/recv.log
    echo "--- send ---"
    cat /tmp/send.log
    echo "===H1DERISK-$NODE-END==="
} > /dev/ttyS1 2>&1

# Emit the pcap (binary) base64-encoded on a dedicated UART so the host can
# reconstruct a real .pcap artifact without a shared filesystem.
if [ -f /tmp/$NODE.pcap ]; then
    {
        echo "===PCAP-$NODE-B64-BEGIN==="
        base64 /tmp/$NODE.pcap
        echo "===PCAP-$NODE-B64-END==="
    } > /dev/ttyS2 2>&1
fi

sync
poweroff -f
