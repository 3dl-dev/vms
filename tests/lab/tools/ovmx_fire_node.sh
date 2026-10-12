#!/bin/bash
# ovmx_fire_node.sh <artdir> <tag> : (in-pod half of ovmx_fire.sh) boot OVMXE on br0 (tap3) with a capture.
# Console log lives where ovmx-lab-console looks: /lab/k8s-labs/$POD_NAME/logs/ovmxe.log
ART=$1; TAG=${2:-run}; RE=/lab/run-fire/$TAG; LOGS=/lab/k8s-labs/$POD_NAME/logs
ip tuntap add tap3 mode tap 2>/dev/null; ip link set tap3 master br0; ip link set tap3 up
mkdir -p $RE $LOGS
setsid nohup tcpdump -i br0 -s0 -U -w $RE/wire.pcap 'ether proto 0x6007' > $RE/tcpdump.out 2>&1 < /dev/null &
setsid nohup env ART=$ART OUT=$LOGS/ovmxe.log TAP=tap3 MAC=52:54:00:00:e5:01 NAME=OVMXE SCSNODE=OVMXE SCSSYSID=1030 VOTES=1 EXPVOTES=3 DUR=10800 bash /lab/run-evac/ovmx_evnode.sh > $RE/OVMXE.drv.out 2>&1 < /dev/null &
echo up
