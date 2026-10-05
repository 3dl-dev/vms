#!/bin/bash
# scenF.sh <rigdir> <art> <tag> -- rd vms-f29: a real V7.3 VAX (VAXC, VOTES 1, EXPECTED_VOTES 2 set at
# SYSBOOT) and ONE booted OVMX node (VOTES 1, EXPECTED_VOTES 2, SCSSYSTEMID 1993 > 1989): neither vote is
# quorum alone, so the VAX must cold-FORM the cluster with the OVMX node as a founding member.
set -u
R="$1"; ART="$2"; TAG="$3"; D=$R/runs/$TAG; mkdir -p $D
for p in $(ps -eo pid,args | grep -E "qemu-system-x86_64|nodedrv.py $R|tcpdum[p] -i $(basename $R | sed 's/run-dlm/brdl/')r" | grep -v grep | grep "$R/" | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 $p; done
sleep 3
BR=$(grep -o "brdl[0-9]*r" $R/b36start.sh | head -1); TAPA=$(grep -o "TAP=tapA[a-z0-9]*" $R/b36start.sh | head -1 | cut -d= -f2)
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk $R/nodeC/data/d0.dsk
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/nvram.bin $R/nodeC/data/nvram.bin
rm -f $R/*.console.log $R/s8.pcap
setsid nohup tcpdump -i $BR -s0 -w $R/s8.pcap 'ether proto 0x6007' > $R/tcpdump.out 2>&1 < /dev/null &
cd $R && setsid nohup python3 $R/nodedrv.py $R/nodeC $R/VAXC.console.log --boot "B/R5:1 DUA0" > $R/nodeC.drv.out 2>&1 < /dev/null &
for i in $(seq 1 120); do grep -aq "SYSBOOT>" $R/VAXC.console.log 2>/dev/null && break; sleep 2; done
for c in "SET EXPECTED_VOTES 2" "SHOW VOTES" "SHOW EXPECTED_VOTES" "CONTINUE"; do sleep 2; printf '%s\r' "$c" > $R/VAXC.console.log.in; done
echo "[$TAG] VAXC continuing at $(date -u +%T)" | tee $D/scen.out
sleep 60
setsid nohup env ART=$ART OUT=$R/OVMXA.console.log TAP=$TAPA MAC=52:54:00:00:df:0a NAME=OVMXA DUR=900 \
   SCSNODE=OVMXA SCSSYSID=1993 VOTES=1 EXPVOTES=2 bash $R/b36node.sh > $R/A.drv.out 2>&1 < /dev/null &
for i in $(seq 1 300); do
  grep -aq "this node is now a VAXcluster member" $R/OVMXA.console.log 2>/dev/null && grep -aq "now a VAXcluster member -- system VAXC" $R/VAXC.console.log && break
  sleep 2
done
sleep 60
echo "[$TAG] VAX: $(tr -d '\r' < $R/VAXC.console.log | grep -a 'CNXMAN' | sed 's/%CNXMAN,  //' | tr '\n' ';')" | tee -a $D/scen.out
echo "[$TAG] OVMXA member=$(grep -ac 'this node is now a VAXcluster member' $R/OVMXA.console.log) bugchecks=$(cat $R/VAXC.console.log $R/OVMXA.console.log | grep -aci bugcheck)" | tee -a $D/scen.out
tr -d '\r' < $R/OVMXA.console.log | tac | grep -m1 -B10 'View of Cluster' | tac | tee -a $D/scen.out
for p in $(ps -eo pid,args | grep "tcpdum[p] -i $BR" | awk '{print $1}'); do kill $p; done
sleep 2; cp $R/OVMXA.console.log $R/VAXC.console.log $D/; gzip -c $R/s8.pcap > $D/s8.pcap.gz
