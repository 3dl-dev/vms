#!/bin/bash
# s4.sh <artdir> <tag> <victim A|B|D> : a removal specimen. Real V7.3 VAXC founds; OVMXA, OVMXB,
# OVMXC (D) join in that order (CSID slots 1..4). The victim's QEMU is SIGKILLed; the real VAX
# coordinates its removal and sends cat-0x01 op-0x08 to each surviving OVMX node. Archived.
set -u
ART=$1; TAG=$2; V=$3; R=/lab/run-af4rig
export ACCEL="${ACCEL:-tcg,thread=multi}" ACCEL_CPU="${ACCEL_CPU:--cpu max}"
cd $R
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i bre88r|tstail.py' | grep -v grep | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 $p 2>/dev/null; done
sleep 3
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk nodeC/data/d0.dsk
cp /lab/cluster-demo-nodeC-v73/data/nvram.bin nodeC/data/nvram.bin
rm -f *.console.log *.tlog *.caps s8.pcap *.drv.out
for n in VAXC OVMXA OVMXB OVMXC; do : > $n.console.log; setsid nohup python3 /tmp/tstail.py $n.console.log $n.tlog >/dev/null 2>&1 < /dev/null & done
member() { grep -qa 'this node is now a VAXcluster member' $1.console.log; }
bash b36start.sh cap; bash b36start.sh C
for i in $(seq 1 300); do grep -qa 'now a VAXcluster member -- system VAXC' VAXC.console.log && break; sleep 2; done
grep -qa 'now a VAXcluster member -- system VAXC' VAXC.console.log || { echo "[$TAG] FATAL VAXC never formed"; exit 1; }
for n in A:OVMXA B:OVMXB D:OVMXC; do
  ART_ROOT=$ART DUR=1200 bash b36start.sh ${n%%:*}
  for i in $(seq 1 150); do member ${n#*:} && break; sleep 2; done
  member ${n#*:} || { echo "[$TAG] FATAL ${n#*:} never joined"; exit 1; }
  echo "[$TAG] ${n#*:} MEMBER $(date +%s.%N)"
done
sleep 30
case $V in A) T=tapAe;; B) T=tapBe;; D) T=tapDe;; esac
P=$(ps -eo pid,args | grep qemu-system-x86_64 | grep "ifname=$T" | grep -v grep | awk '{print $1}')
echo "[$TAG] KILL $V ($T) $(date +%s.%N)"; kill -9 $P
for i in $(seq 1 60); do grep -qa 'removed from VAXcluster system' VAXC.console.log && break; sleep 2; done
sleep 20
D=runs/$TAG; mkdir -p $D
cp *.console.log *.tlog $D/ 2>/dev/null; gzip -c s8.pcap > $D/s8.pcap.gz
echo "[$TAG] VAXC: $(grep -a 'removed from VAXcluster system' VAXC.console.log | tr -d '\r')"
python3 /tmp/scan08.py $D/s8.pcap.gz
echo "[$TAG] END"
