#!/bin/bash
# stop ONLY this lab's emulators/drivers/tcpdump (e88lab dir / bre88)
D=/lab/k8s-labs/e88lab
for f in $D/logs/vax*.log.pid; do [ -f "$f" ] && kill $(cat $f) 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$D/vax" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 $p 2>/dev/null; done
pkill -f "tcpdump -i bre88" 2>/dev/null
sleep 2; rm -f $D/data/d0.dsk $D/data/d1.dsk $D/logs/*.log.in
ps aux | grep -E "[v]ax.ini|[t]cpdump" | wc -l
