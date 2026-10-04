#!/bin/bash
# seq.sh <tag> <founder> <step>...   step = "2" or "1+2" (concurrent)
# node roots: 1=SYS0 2=SYS1 3=SYS2. Founder boots conversational, VOTES=1 EXPECTED_VOTES=1.
set -u
D=/lab/k8s-labs/e88lab; TAG=$1; F=$2; shift 2
root() { case $1 in 1) echo 0;; 2) echo 1;; 3) echo 2;; esac; }
wlog() { local n=$1 pat=$2 lim=$3; for _ in $(seq 1 $lim); do grep -aq "$pat" $D/logs/vax$n.log 2>/dev/null && return 0; sleep 1; done; return 1; }
bash /root/orstop.sh >/dev/null; bash /root/orsetup.sh >/dev/null
mkdir -p $D/$TAG; rm -f $D/$TAG/*
setsid nohup tcpdump -i bre88 -s0 -U -w $D/$TAG/$TAG.pcap "ether proto 0x6007" > $D/$TAG/tcpdump.out 2>&1 < /dev/null &
sleep 1
echo "$(date -u +%T) founder vax$F"
bash /root/orboot.sh $F "B/R5:$(root $F)0000001 DUA0"
wlog $F "SYSBOOT>" 120 || { echo no SYSBOOT; exit 1; }
sleep 2; bash /root/con.sh $F "SET VOTES 1"; sleep 1; bash /root/con.sh $F "SET EXPECTED_VOTES 1"; sleep 1; bash /root/con.sh $F "CONTINUE"
wlog $F "completing VAXcluster state transition" 400 || { echo founder never formed; exit 1; }
echo "$(date -u +%T) founder formed"; sleep ${GAP:-20}
for st in "$@"; do
  for n in ${st//+/ }; do echo "$(date -u +%T) boot vax$n"; bash /root/orboot.sh $n "B/R5:$(root $n)0000000 DUA0"; done
  for n in ${st//+/ }; do wlog $n "now a VAXcluster member" ${JOINWAIT:-500} && echo "$(date -u +%T) vax$n MEMBER" || echo "$(date -u +%T) vax$n NOT member"; done
  sleep ${GAP:-20}
done
sleep 5
cp $D/logs/vax*.log $D/$TAG/
grep -a "CNXMAN" $D/$TAG/vax*.log | tr -d "\r"
python3 /root/op02.py $D/$TAG/$TAG.pcap
echo "$(date -u +%T) DONE"
