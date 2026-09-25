#!/bin/bash
# fault3b.sh -- the rd vms-b36 fault landed INSIDE the transition, on three
# REAL OpenVMS VAX V7.3 nodes.
#
# f1 armed on the coordinator's "proposing addition" line, and real VMS
# completed the transition in under a millisecond, so the fault landed AFTER
# it. This arms on the JOINER's own "sending VAXcluster membership request"
# line -- the last thing it prints before the members run the transition -- and
# adds the bench rig's 80+-40 ms jitter to its tap so the transition is still
# open when the tap goes dark.
#
# tc netem only DROPS/DELAYS: nothing is injected and no frame is altered.
set -u
D=/lab/k8s-labs/b36lab
SECS="${SECS:-45}"
TAG="${TAG:-f2}"
JIT="${JIT:-80ms 40ms}"

for p in $(ps -eo pid,args | grep 'tcpdum[p] -i br0' | grep -v grep | awk '{print $1}'); do kill "$p"; done
sleep 1
setsid nohup tcpdump -i br0 -s0 -U -w "$D/oracle-3node-fault-$TAG.pcap" 'ether proto 0x6007' \
    > "$D/tcpdumpf.out" 2>&1 < /dev/null &
sleep 2

tc qdisc del dev tap3 root 2>/dev/null
for p in $(ps -eo pid,args | grep 'nodedrv[.]py .*vax3' | grep -v grep | awk '{print $1}'); do kill -9 "$p"; done
V3=$(ls -l /proc/*/cwd 2>/dev/null | grep "$D/vax3" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|')
for p in $V3; do kill -9 "$p" 2>/dev/null; done
echo "[$TAG] vax3 stopped at $(date -u +%H:%M:%S)"
sleep 45

tc qdisc replace dev tap3 root netem delay $JIT distribution normal
: > "$D/logs/vax3.log"
cd "$D" && setsid nohup python3 /usr/local/bin/nodedrv.py "$D/vax3" "$D/logs/vax3.log" \
    --date "25-SEP-2026 15:50" --boot "B/R5:20000000 DUA0" > "$D/vax3.drv.out" 2>&1 < /dev/null &
echo "[$TAG] vax3 rebooting under jitter '$JIT' at $(date -u +%H:%M:%S)"

for _ in $(seq 1 12000); do
    grep -qa 'sending VAXcluster membership request' "$D/logs/vax3.log" 2>/dev/null && break
    sleep 0.02
done
if ! grep -qa 'sending VAXcluster membership request' "$D/logs/vax3.log" 2>/dev/null; then
    echo "[$TAG] MARKER never appeared -- no fault injected"; exit 1
fi
echo "[$TAG] MARKER (vax3 sent its membership request) at $(date -u +%H:%M:%S.%3N)"
sleep "${DELAY:-0}"
tc qdisc replace dev tap3 root netem loss 100%
echo "[$TAG] BLACKOUT on tap3 at $(date -u +%H:%M:%S.%3N)"
sleep "$SECS"
tc qdisc replace dev tap3 root netem delay $JIT distribution normal
echo "[$TAG] BLACKOUT off tap3 at $(date -u +%H:%M:%S.%3N) after ${SECS}s"
