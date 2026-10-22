#!/bin/bash
# ovmx_fire_node.sh <artdir> <tag> [expected-votes] : (in-pod half of ovmx_fire.sh) boot one OVMX node on br0 with a capture.
# Console log lives where ovmx-lab-console looks: /lab/k8s-labs/$POD_NAME/logs/<node>.log
# A second OVMX node in the same pod (the demo topology, rd vms-94a) sets OVMX_NODE, OVMX_SYSID,
# OVMX_MAC and OVMX_TAP; the capture is started only by the first node of a tag (OVMX_NOCAP=1 skips it).
ART=$1; TAG=${2:-run}; EV=${3:-3}; RE=/lab/run-fire/$TAG; LOGS=/lab/k8s-labs/$POD_NAME/logs
NODE=${OVMX_NODE:-OVMXE}; SYSID=${OVMX_SYSID:-1030}; MAC=${OVMX_MAC:-52:54:00:00:e5:01}; TAP=${OVMX_TAP:-tap3}
LOG=$LOGS/$(echo "$NODE" | tr A-Z a-z).log
ip tuntap add $TAP mode tap 2>/dev/null; ip link set $TAP master br0; ip link set $TAP up
mkdir -p $RE $LOGS
[ "${OVMX_NOCAP:-0}" = 1 ] || setsid nohup tcpdump -i br0 -s0 -U -w $RE/wire.pcap 'ether proto 0x6007' > $RE/tcpdump.out 2>&1 < /dev/null &
setsid nohup env ART=$ART OUT=$LOG TAP=$TAP MAC=$MAC NAME=$NODE SCSNODE=$NODE SCSSYSID=$SYSID VOTES=1 EXPVOTES=$EV DUR=10800 \
  LOCKDIRWT="${LOCKDIRWT:-}" RM_RESIDUE_FILL="${RM_RESIDUE_FILL:-}" bash /lab/run-evac/ovmx_evnode.sh > $RE/$NODE.drv.out 2>&1 < /dev/null &
echo up $NODE
