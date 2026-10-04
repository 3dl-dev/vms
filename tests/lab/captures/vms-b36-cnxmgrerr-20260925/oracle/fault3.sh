#!/bin/bash
# fault3.sh -- the rd vms-b36 fault, on THREE REAL OpenVMS VAX V7.3 nodes.
#
# VAX1 founds, VAX2 is a member, VAX3 is the joiner. VAX3's tap is blacked out
# (tc netem loss 100%, nothing injected, no frame altered) the instant the
# coordinator logs "proposing addition of system VAX3" -- the same point in the
# same transition where the OVMX rig's real VAX takes CNXMGRERR.
#
# This is the CONTROL: what a real VMS member does when the node being admitted
# vanishes mid-transition, and whether anything bugchecks.
set -u
D=/lab/k8s-labs/b36lab
SECS="${SECS:-45}"
TAG="${TAG:-f1}"

for p in $(ps -eo pid,args | grep 'tcpdum[p] -i br0' | grep -v grep | awk '{print $1}'); do kill "$p"; done
sleep 1
setsid nohup tcpdump -i br0 -s0 -U -w "$D/oracle-3node-fault-$TAG.pcap" 'ether proto 0x6007' \
    > "$D/tcpdumpf.out" 2>&1 < /dev/null &
sleep 2

# stop VAX3 (its driver + emulator), let the cluster remove it, then bring it back
for p in $(ps -eo pid,args | grep 'nodedrv[.]py .*vax3' | grep -v grep | awk '{print $1}'); do kill -9 "$p"; done
V3=$(ls -l /proc/*/cwd 2>/dev/null | grep "$D/vax3" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|')
for p in $V3; do kill -9 "$p" 2>/dev/null; done
echo "[$TAG] vax3 stopped at $(date -u +%H:%M:%S)"
sleep 40

: > "$D/logs/vax3.log"
cd "$D" && setsid nohup python3 /usr/local/bin/nodedrv.py "$D/vax3" "$D/logs/vax3.log" \
    --date "25-SEP-2026 15:50" --boot "B/R5:20000000 DUA0" > "$D/vax3.drv.out" 2>&1 < /dev/null &
echo "[$TAG] vax3 rebooting at $(date -u +%H:%M:%S)"

# arm: the coordinator's own console line that the transition has opened.
# The member consoles are append-only across the whole session, so the trigger
# is an INCREASE over the baseline, never the mere presence of the line.
marks() { cat "$D/logs/vax1.log" "$D/logs/vax2.log" 2>/dev/null \
            | grep -ac 'proposing addition of system VAX3'; }
BASE=$(marks)
echo "[$TAG] baseline 'proposing addition of system VAX3' count = $BASE"
for _ in $(seq 1 6000); do
    [ "$(marks)" -gt "$BASE" ] && break
    sleep 0.05
done
if [ "$(marks)" -le "$BASE" ]; then
    echo "[$TAG] MARKER never appeared -- no fault injected"; exit 1
fi
echo "[$TAG] MARKER 'proposing addition of system VAX3' at $(date -u +%H:%M:%S.%3N)"
tc qdisc replace dev tap3 root netem loss 100%
echo "[$TAG] BLACKOUT on tap3 at $(date -u +%H:%M:%S.%3N)"
sleep "$SECS"
tc qdisc del dev tap3 root 2>/dev/null
echo "[$TAG] BLACKOUT off tap3 at $(date -u +%H:%M:%S.%3N) after ${SECS}s"
