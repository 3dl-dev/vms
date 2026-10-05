#!/bin/bash
# r2.sh -- ORACLE (as r1) plus a capture ON tapd3, i.e. AFTER netem: the order the real VAX3 actually RECEIVED.
S=/root/dl; D=/lab/k8s-labs/dlmlab; TAG=${1:-R2}
bash $S/run.sh $TAG 0 0 || exit 1
tc qdisc replace dev tapd3 root netem delay 80ms 40ms distribution normal
setsid nohup tcpdump -i tapd3 -s0 -U -w $D/$TAG/$TAG-rx3.pcap "ether proto 0x6007" > $D/$TAG/tcpdump3.out 2>&1 < /dev/null &
sleep 1
bash $S/join3.sh 0
sleep 120
tc qdisc del dev tapd3 root 2>/dev/null
cp $D/logs/vax?.log $D/$TAG/
echo R2-DONE
