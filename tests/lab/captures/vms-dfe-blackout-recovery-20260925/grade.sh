#!/bin/bash
# grade.sh <tag> -- grade one arm of the rd vms-dfe blackout repro, from the
# three consoles and the capture. Prints one PASS/FAIL line plus the evidence.
set -u
R=/lab/run-dfe; TAG="$1"
B=$R/OVMXB.console.log; A=$R/OVMXA.console.log; C=$R/VAXC.console.log
bo_on=$(grep -ao 'BLACKOUT on .*' "$R/blackout.out" 2>/dev/null | head -1)
bo_off=$(grep -ao 'BLACKOUT off .*' "$R/blackout.out" 2>/dev/null | head -1)
bmem=$(grep -ac 'this node is now a VAXcluster member' "$B")
amem=$(grep -ac 'this node is now a VAXcluster member' "$A")
vaxadd=$(grep -ac 'proposing addition of system OVMXB' "$C")
bug=$(cat "$A" "$B" "$C" | grep -ac 'BUG CHECK\|BUGCHECK\|bugcheck')
loss=$(cat "$A" "$B" | grep -ac 'lost connection\|quorum lost\|was removed from the cluster')
# the LAST SHOW CLUSTER table on each OVMX node: how many MEMBER rows
btab=$(tr -d '\r' < "$B" | tac | grep -m1 -A14 'View of Cluster' | grep -c 'MEMBER')
btab=$(tr -d '\r' < "$B" | awk '/View of Cluster/{n=0;buf=""} {buf=buf"\n"$0} END{print buf}' | grep -c 'MEMBER *|')
bfinal=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
afinal=$(tr -d '\r' < "$A" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
dis=$(tr -d '\r' < "$B" | grep -c 'DISCONNECT')
frames=$(tcpdump -r "$R/dfe.pcap" -nn 2>/dev/null | wc -l)
pass=PASS
[ "$bmem" -ge 1 ] || pass=FAIL
[ "$amem" -ge 1 ] || pass=FAIL
[ "$vaxadd" -ge 1 ] || pass=FAIL
[ "$bfinal" -eq 3 ] || pass=FAIL
[ "$afinal" -eq 3 ] || pass=FAIL
[ "$bug" -eq 0 ] || pass=FAIL
[ "$loss" -eq 0 ] || pass=FAIL
echo "[$TAG] $pass  B-member=$bmem A-member=$amem VAX-proposed-B=$vaxadd" \
     "B-final-MEMBER-rows=$bfinal A-final-MEMBER-rows=$afinal" \
     "B-DISCONNECT-lines=$dis bugchecks=$bug losses=$loss frames=$frames"
echo "    $bo_on"
echo "    $bo_off"
