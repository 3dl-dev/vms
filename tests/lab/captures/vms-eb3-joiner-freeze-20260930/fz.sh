#!/bin/bash
# fz.sh <tag> <founder> <member> <joiner> <match> <secs>
# founder forms; member joins; then the joiner boots and is frozen in the
# op-0a/op-0b window of its own admission. Everything on breb3 captured.
set -u
D=/lab/k8s-labs/eb3lab; TAG=$1; F=$2; M=$3; J=$4; MATCH=$5; SECS=$6
root() { case $1 in 1) echo 0;; 2) echo 1;; 3) echo 2;; esac; }
sid() { case $1 in 1) echo 1025;; 2) echo 1026;; 3) echo 1027;; esac; }
wlog() { local n=$1 pat=$2 lim=$3; for _ in $(seq 1 $lim); do grep -aq "$pat" $D/logs/vax$n.log 2>/dev/null && return 0; sleep 1; done; return 1; }
bash /root/orstop.sh >/dev/null; bash /root/orsetup.sh >/dev/null
mkdir -p $D/$TAG; rm -f $D/$TAG/*
setsid nohup tcpdump -i breb3 -s0 -U -w $D/$TAG/$TAG.pcap "ether proto 0x6007" > $D/$TAG/tcpdump.out 2>&1 < /dev/null &
sleep 1
bash /root/orboot.sh $F "B/R5:$(root $F)0000001 DUA0"
wlog $F "SYSBOOT>" 120 || { echo no SYSBOOT; exit 1; }
sleep 2; bash /root/con.sh $F "SET VOTES 1"; sleep 1; bash /root/con.sh $F "SET EXPECTED_VOTES 1"; sleep 1; bash /root/con.sh $F "CONTINUE"
wlog $F "completing VAXcluster state transition" 400 || { echo founder never formed; exit 1; }
echo "$(date -u +%T) founder vax$F formed"; sleep 15
bash /root/orboot.sh $M "B/R5:$(root $M)0000000 DUA0"
wlog $M "now a VAXcluster member" 500 && echo "$(date -u +%T) vax$M MEMBER" || { echo "vax$M NOT member"; exit 1; }
sleep 20
bash /root/orboot.sh $J "B/R5:$(root $J)0000000 DUA0"
sleep 3
JP=$(ls -l /proc/*/cwd 2>/dev/null | grep "$D/vax$J\$" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|' | while read p; do [ "$(cat /proc/$p/comm)" = vax ] && echo $p; done | head -1)
echo "$(date -u +%T) joiner vax$J simh pid=$JP"
DROP_GO=${DROP_GO:-0} python3 /root/freeze.py tapo$J $(sid $J) $JP $MATCH $SECS $D/$TAG/freeze.log &
FZ=$!
wait $FZ
cat $D/$TAG/freeze.log
sleep ${SETTLE:-240}
cp $D/logs/vax*.log $D/$TAG/
grep -a "CNXMAN\|BUG\|CLUEXIT" $D/$TAG/vax*.log | tr -d "\r" | tail -40
echo "$(date -u +%T) DONE"
