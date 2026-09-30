#!/bin/bash
# orboot.sh <n> <bootcmd>   e.g. orboot.sh 3 "B/R5:20000001 DUA0"
D=/lab/k8s-labs/eb3lab; n=$1; b=$2
: > $D/logs/vax$n.log
cd $D && setsid nohup python3 /usr/local/bin/nodedrv.py $D/vax$n $D/logs/vax$n.log \
   --date "$(date '+%d-%b-%Y %H:%M' | tr a-z A-Z)" --boot "$b" > $D/vax$n.drv.out 2>&1 < /dev/null &
echo booted vax$n "$b"
