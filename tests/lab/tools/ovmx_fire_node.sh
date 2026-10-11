#!/bin/bash
# ovmx_fire_node.sh <artdir> <tag> [expected-votes] : (in-pod half of ovmx_fire.sh) boot OVMXE on br0 (tap3) with a capture.
# expected-votes defaults to 3 (OVMXE joins the VAX pair); 1 lets it FOUND a cluster
# alone, so the VAXes join one OVMX coordinates (rd vms-64b, the demo's OVMX-first order).
# Console log lives where ovmx-lab-console looks: /lab/k8s-labs/$POD_NAME/logs/ovmxe.log
ART=$1; TAG=${2:-run}; EV=${3:-3}; RE=/lab/run-fire/$TAG; LOGS=/lab/k8s-labs/$POD_NAME/logs
ip link add br0 type bridge 2>/dev/null; ip link set br0 up
ip tuntap add tap3 mode tap 2>/dev/null; ip link set tap3 master br0; ip link set tap3 up
mkdir -p $RE $LOGS
setsid nohup tcpdump -i br0 -s0 -U -w $RE/wire.pcap 'ether proto 0x6007' > $RE/tcpdump.out 2>&1 < /dev/null &
setsid nohup env ART=$ART OUT=$LOGS/ovmxe.log TAP=tap3 MAC=52:54:00:00:e5:01 NAME=OVMXE SCSNODE=OVMXE SCSSYSID=1030 VOTES=1 EXPVOTES=$EV DUR=10800 bash /lab/run-evac/ovmx_evnode.sh > $RE/OVMXE.drv.out 2>&1 < /dev/null &
echo up
