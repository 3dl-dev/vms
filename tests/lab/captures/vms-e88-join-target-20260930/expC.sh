#!/bin/bash
# expC.sh <tag> <hide_seconds>
# Founder VAX3 (1027, highest -- the demo's VAXC 1989). VAX1 (1025 -- OVMXA 1987) joins.
# VAX2 (1026 -- OVMXB 1988) then boots while every frame between VAX2 and VAX3 is
# dropped at the bridge, so VAX2 discovers ONLY the lower, non-coordinating member
# first; the block lifts <hide_seconds> after VAX1's first frame reaches VAX2.
set -u
D=/lab/k8s-labs/e88lab; TAG=$1; HIDE=$2
M1=08:00:2b:20:e5:74; M2=08:00:2b:13:f1:57; M3=08:00:2b:00:d2:ff
wlog() { local n=$1 pat=$2 lim=$3; for _ in $(seq 1 $lim); do grep -aq "$pat" $D/logs/vax$n.log 2>/dev/null && return 0; sleep 1; done; return 1; }
unblock() { tc qdisc del dev tape2 clsact 2>/dev/null; tc qdisc del dev tape3 clsact 2>/dev/null; }
bash /root/orstop.sh >/dev/null; unblock; bash /root/orsetup.sh >/dev/null
mkdir -p $D/$TAG; rm -f $D/$TAG/*
setsid nohup tcpdump -i bre88 -s0 -U -w $D/$TAG/$TAG.pcap "ether proto 0x6007" > $D/$TAG/tcpdump.out 2>&1 < /dev/null &
sleep 1
bash /root/orboot.sh 3 "B/R5:20000001 DUA0"
wlog 3 "SYSBOOT>" 120 || { echo no SYSBOOT; exit 1; }
sleep 2; bash /root/con.sh 3 "SET VOTES 1"; sleep 1; bash /root/con.sh 3 "SET EXPECTED_VOTES 1"; sleep 1; bash /root/con.sh 3 "CONTINUE"
wlog 3 "completing VAXcluster state transition" 400 || { echo founder never formed; exit 1; }
echo "$(date -u +%T.%N) vax3 formed"; sleep 15
bash /root/orboot.sh 1 "B/R5:00000000 DUA0"
wlog 1 "now a VAXcluster member" 500 && echo "$(date -u +%T.%N) vax1 MEMBER" || { echo vax1 not member; exit 1; }
sleep 20
# hide VAX3 <-> VAX2 at the bridge egress ports
tc qdisc add dev tape2 clsact; tc filter add dev tape2 egress protocol all flower src_mac $M3 action drop
tc qdisc add dev tape3 clsact; tc filter add dev tape3 egress protocol all flower src_mac $M2 action drop
echo "$(date -u +%T.%N) block on"
bash /root/orboot.sh 2 "B/R5:10000000 DUA0"
timeout 300 tcpdump -i tape2 -c 1 -n "ether src $M1 and ether dst $M2" > /dev/null 2>&1
echo "$(date -u +%T.%N) vax2 first unicast from vax1"
sleep $HIDE; unblock
echo "$(date -u +%T.%N) block off"
wlog 2 "now a VAXcluster member" ${JOINWAIT:-400} && echo "$(date -u +%T.%N) vax2 MEMBER" || echo "$(date -u +%T.%N) vax2 NOT member"
sleep 20
cp $D/logs/vax*.log $D/$TAG/
grep -a "CNXMAN" $D/$TAG/vax*.log | tr -d "\r"
python3 /root/op02.py $D/$TAG/$TAG.pcap
echo "$(date -u +%T) DONE"
