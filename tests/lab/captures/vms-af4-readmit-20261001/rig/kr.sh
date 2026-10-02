#!/bin/bash
# kr.sh <artdir> <tag> : one "kill+reboot a member inside the reconnect window" arm (rd vms-af4).
# Real V7.3 VAXC founds; OVMXA, OVMXB join. OVMXB's QEMU is SIGKILLed (no departure, no last
# gasp) and the SAME node (SCSNODE/SCSSYSTEMID) is booted again at once. Graded on: the
# returning node becomes a member again, the real VAX admits it in its own words, OVMXA's own
# SHOW CLUSTER ends with three MEMBER rows, and nothing bugchecked.
set -u
ART=$1; TAG=$2; R=/lab/run-af4rig
export ACCEL="${ACCEL:-tcg,thread=multi}" ACCEL_CPU="${ACCEL_CPU:--cpu max}"
cd $R
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i bre88r|tstail.py' | grep -v grep | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 $p 2>/dev/null; done
sleep 3
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk nodeC/data/d0.dsk
cp /lab/cluster-demo-nodeC-v73/data/nvram.bin nodeC/data/nvram.bin
rm -f *.console.log *.tlog *.caps s8.pcap *.drv.out
for n in VAXC OVMXA OVMXB; do : > $n.console.log; setsid nohup python3 /tmp/tstail.py $n.console.log $n.tlog >/dev/null 2>&1 < /dev/null & done
member() { grep -qa 'this node is now a VAXcluster member' $1; }
bash b36start.sh cap; bash b36start.sh C
for i in $(seq 1 300); do grep -qa 'now a VAXcluster member -- system VAXC' VAXC.console.log && break; sleep 2; done
grep -qa 'now a VAXcluster member -- system VAXC' VAXC.console.log || { echo "[$TAG] NOFAULT VAXC never formed"; exit 0; }
ART_ROOT=$ART DUR=1200 bash b36start.sh A
for i in $(seq 1 150); do member OVMXA.console.log && break; sleep 2; done
ART_ROOT=$ART DUR=1200 bash b36start.sh B
for i in $(seq 1 150); do member OVMXB.console.log && break; sleep 2; done
if ! member OVMXA.console.log || ! member OVMXB.console.log; then echo "[$TAG] NOFAULT initial join failed"; exit 0; fi
add0=$(grep -acE 'proposing addition of system OVMXB|proposed addition of node OVMXB' VAXC.console.log)
sleep 45
P=$(ps -eo pid,args | grep qemu-system-x86_64 | grep 'ifname=tapBe' | grep -v grep | awk '{print $1}')
tk=$(date +%s.%N); kill -9 $P; sleep 3
cp OVMXB.console.log OVMXB-incarnation1.console.log
for p in $(ps -eo pid,args | grep "tstail.py OVMXB" | grep -v grep | awk '{print $1}'); do kill $p; done
mv OVMXB.tlog OVMXB-incarnation1.tlog; : > OVMXB.console.log
setsid nohup python3 /tmp/tstail.py OVMXB.console.log OVMXB.tlog >/dev/null 2>&1 < /dev/null &
ART_ROOT=$ART DUR=600 bash b36start.sh B
tm=""
for i in $(seq 1 180); do member OVMXB.console.log && { tm=$(date +%s.%N); break; }; sleep 2; done
sleep 60
D=runs/$TAG; mkdir -p $D
cp *.console.log *.tlog $D/ 2>/dev/null; gzip -c s8.pcap > $D/s8.pcap.gz
add1=$(grep -acE 'proposing addition of system OVMXB|proposed addition of node OVMXB' VAXC.console.log)
bug=$(cat VAXC.console.log OVMXA.console.log OVMXB-incarnation1.console.log OVMXB.console.log | grep -ac 'BUG CHECK\|BUGCHECK\|bugcheck')
arows=$(tr -d '\r' < OVMXA.console.log | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
brows=$(tr -d '\r' < OVMXB.console.log | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
dt=""; [ -n "$tm" ] && dt=$(python3 -c "print('%.1f' % ($tm-$tk))")
v=FAIL; [ -n "$tm" ] && [ "$add1" -gt "$add0" ] && [ "$arows" -eq 3 ] && [ "$brows" -eq 3 ] && [ "$bug" -eq 0 ] && v=PASS
echo "[$TAG] $v readmitted=${tm:+yes} kill->member=${dt}s VAX-readded=$((add1-add0)) finalMEMBERrows(A/B)=$arows/$brows bugchecks=$bug"
