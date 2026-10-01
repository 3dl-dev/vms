#!/bin/bash
# af4arm.sh <artdir> <tag> : one rd vms-af4 arm, from scratch. VAXC (real V7.3) founds, OVMXA
# joins, OVMXB joins; OVMXB's QEMU is SIGKILLed (no departure, no last gasp) and the SAME node
# is booted again at once, inside the survivors' RECNXINTERVAL. Archived to runs/<tag>.
set -u
ART=$1; TAG=$2; R=/lab/run-b98rig
export ACCEL="${ACCEL:-tcg,thread=multi}" ACCEL_CPU="${ACCEL_CPU:--cpu max}"
cd $R
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i bre88r|tstail.py' | grep -v grep | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 $p 2>/dev/null; done
sleep 3
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk nodeC/data/d0.dsk
cp /lab/cluster-demo-nodeC-v73/data/nvram.bin nodeC/data/nvram.bin
rm -f *.console.log *.tlog *.caps s8.pcap *.drv.out
for n in VAXC OVMXA OVMXB; do : > $n.console.log; setsid nohup python3 /tmp/tstail.py $n.console.log $n.tlog >/dev/null 2>&1 < /dev/null & done
bash b36start.sh cap; bash b36start.sh C
for i in $(seq 1 150); do grep -qa 'now a VAXcluster member -- system VAXC' VAXC.console.log && break; sleep 2; done
echo "[$TAG] VAXC formed $(date +%s.%N)"
ART_ROOT=$ART DUR=1200 bash b36start.sh A
for i in $(seq 1 150); do grep -qa 'this node is now a VAXcluster member' OVMXA.console.log && break; sleep 2; done
echo "[$TAG] A MEMBER $(date +%s.%N)"
ART_ROOT=$ART DUR=900 bash b36start.sh B
for i in $(seq 1 150); do grep -qa 'this node is now a VAXcluster member' OVMXB.console.log && break; sleep 2; done
echo "[$TAG] B MEMBER $(date +%s.%N)"
sleep 45
P=$(ps -eo pid,args | grep qemu-system-x86_64 | grep 'ifname=tapBe' | grep -v grep | awk '{print $1}')
echo "[$TAG] KILL B $(date +%s.%N)"; kill -9 $P
sleep 3
cp OVMXB.console.log OVMXB-incarnation1.console.log
for p in $(ps -eo pid,args | grep "tstail.py OVMXB" | grep -v grep | awk '{print $1}'); do kill $p; done
mv OVMXB.tlog OVMXB-incarnation1.tlog
echo "[$TAG] REBOOT B $(date +%s.%N)"
ART_ROOT=$ART DUR=600 bash b36start.sh B
setsid nohup python3 /tmp/tstail.py OVMXB.console.log OVMXB.tlog >/dev/null 2>&1 < /dev/null &
for i in $(seq 1 150); do grep -qa 'this node is now a VAXcluster member' OVMXB.console.log && break; sleep 2; done
echo "[$TAG] B2 MEMBER-or-timeout $(date +%s.%N) member=$(grep -ac 'this node is now a VAXcluster member' OVMXB.console.log)"
sleep 60
D=runs/$TAG; mkdir -p $D
cp VAXC.console.log OVMXA.console.log OVMXB.console.log OVMXB-incarnation1.console.log *.tlog $D/
gzip -c s8.pcap > $D/s8.pcap.gz
echo "[$TAG] END $(date +%s.%N)"
