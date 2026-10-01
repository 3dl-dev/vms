#!/bin/bash
# darkm.sh <tap> <secs> <n> <gap> : like dark.sh, but before each trial confirms on VAX1's own
# SHOW CLUSTER that VAX2 is a MEMBER (a trial on a cluster that already lost the member is void).
set -u
T=$1; S=$2; N=$3; G=$4
L=/lab/k8s-labs/b98lab/logs/vax1.log
for i in $(seq 1 $N); do
  until awk '{exit !($1<20)}' /proc/loadavg; do sleep 5; done
  s=$(wc -c < $L); printf 'SHOW CLUSTER\r' > $L.in; sleep 4
  if ! tail -c +$s $L | tr -d '\r' | grep -aqE '\| VAX2 +\| VMS V7.3 \| MEMBER'; then echo "T$i ABORT: VAX2 not a member"; exit 1; fi
  sleep $(python3 -c "import random;print(round(random.uniform(0,3),2))")
  echo "T$i STOP $(date +%s.%N) load=$(cut -d' ' -f1 /proc/loadavg)"; ip link set $T down; sleep $S; ip link set $T up; echo "T$i CONT $(date +%s.%N)"
  sleep $G
done
