#!/bin/bash
# scenD.sh <art> <tag> -- rd vms-8219 lab proof on the run-dlm rig: VAXC + OVMXA + OVMXB (no stall),
# then VAXC takes and releases 40 fresh root names; with every LOCKDIRWT 0 about two thirds of them
# have an OVMX node as their DIRECTORY. Without the directory role the first such $ENQ never returns.
set -u
ART="$1"; TAG="$2"; R=/lab/run-dlm; D=$R/runs/$TAG; mkdir -p $D
C=$R/VAXC.console.log
vin() { printf '%s\r' "$1" > $R/VAXC.console.log.in; }
wait_for() { local f=$1 pat=$2 lim=$3; for _ in $(seq 1 $lim); do grep -aq -- "$pat" $f 2>/dev/null && return 0; sleep 2; done; return 1; }
NOFAULT=1 JOIN_ORDER=a-then-b STALL_S=6 NODE_DUR=1500 bash $R/runarmD.sh "$ART" "$TAG" > $D/runarm.out 2>&1
wait_for $R/OVMXB.console.log "this node is now a VAXcluster member" 200 || echo "[$TAG] B never MEMBER" | tee -a $D/scen.out
sleep 30
# login VAXC, prompt-synchronised (a batched login races the console and fails)
cnt() { tr -d '\r' < $C | grep -ac -- "$1"; true; }
wmore() { local pat=$1 lim=$2 b; b=$(cnt "$pat"); for _ in $(seq 1 $lim); do [ "$(cnt "$pat")" -gt "$b" ] && return 0; sleep 1; done; return 1; }
logged=0
for t in 1 2 3 4 5; do
  b=$(cnt 'Username: *$'); vin ""
  for _ in $(seq 1 15); do [ "$(cnt 'Username: *$')" -gt "$b" ] && break; sleep 1; done
  b=$(cnt 'Password: *$'); vin "SYSTEM"
  for _ in $(seq 1 15); do [ "$(cnt 'Password: *$')" -gt "$b" ] && break; sleep 1; done
  b=$(cnt '^ *\$ *$'); vin "OVMXCLUSTER1"
  for _ in $(seq 1 40); do [ "$(cnt '^ *\$ *$')" -gt "$b" ] && { logged=1; break; }; sleep 1; done
  [ $logged = 1 ] && break
done
echo "[$TAG] VAXC login=$logged" | tee -a $D/scen.out
vin 'SET DEFAULT SYS$LOGIN'; sleep 2
# the driver: type DLMENQ.MAR, assemble, link
vin "CREATE DLMENQ.MAR"; sleep 2
while IFS= read -r l; do printf '%s\r' "$l" > $R/VAXC.console.log.in; sleep 0.3; done < /root/dl/DLMENQ.MAR
sleep 1; printf '\032' > $R/VAXC.console.log.in; sleep 3
vin "MACRO DLMENQ"; sleep 25; vin "LINK DLMENQ"; sleep 15
vin "CREATE DLMLOOP.COM"; sleep 2
while IFS= read -r l; do printf '%s\r' "$l" > $R/VAXC.console.log.in; sleep 0.3; done < /root/dl/d2/DLMLOOP.COM
sleep 1; printf '\032' > $R/VAXC.console.log.in; sleep 3
echo "$(date +%s) LOOP-START" >> $D/scen.out
vin "@DLMLOOP"
wait_for_run() { for _ in $(seq 1 $1); do [ "$(cnt '^DLMD-FINISHED')" -gt 0 ] && return 0; sleep 2; done; return 1; }
wait_for_run 150 && echo "[$TAG] loop FINISHED" | tee -a $D/scen.out || echo "[$TAG] loop did NOT finish" | tee -a $D/scen.out
echo "[$TAG] done=$(cnt '^DLMD-DONE') started=$(cnt '^DLMD-START')" | tee -a $D/scen.out
sleep 5
for p in $(ps -eo pid,args | grep 'tcpdum[p] -i brdlr' | awk '{print $1}'); do kill $p; done
sleep 2; cp $R/OVMXA.console.log $R/OVMXB.console.log $C $D/; gzip -c $R/s8.pcap > $D/s8.pcap.gz
echo "[$TAG] bugchecks=$(cat $D/*.console.log | grep -aci 'bugcheck')" | tee -a $D/scen.out
