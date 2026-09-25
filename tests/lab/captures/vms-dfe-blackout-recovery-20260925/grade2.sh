#!/bin/bash
# grade2.sh <run-dir> <tag> -- grade one arm of the rd vms-dfe blackout repro.
#
# WHAT COUNTS AS A PASS, and why "losses" is NOT one of the criteria: this
# harness CUTS the joiner's connectivity on purpose, so
# "%CNXMAN, lost connection to a cluster member, reconnecting" is the CORRECT
# line for the fault that was injected, not a defect. It is REPORTED (a count
# in the hundreds is the peer-driven connect/disconnect loop this capture
# records) but the teeth are: the joiner reached MEMBER, BOTH OVMX nodes'
# own SHOW CLUSTER names all three systems MEMBER, the real VAX proposed the
# addition itself, and nothing bugchecked.
set -u
D="$1"; TAG="$2"
B=$D/OVMXB.console.log; A=$D/OVMXA.console.log; C=$D/VAXC.console.log
bmem=$(grep -ac 'this node is now a VAXcluster member' "$B")
amem=$(grep -ac 'this node is now a VAXcluster member' "$A")
vaxadd=$(grep -ac 'proposing addition of system OVMXB' "$C")
bug=$(cat "$A" "$B" "$C" | grep -ac 'BUG CHECK\|BUGCHECK\|bugcheck')
loss=$(cat "$A" "$B" | grep -ac 'lost connection to a cluster member')
bfinal=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
afinal=$(tr -d '\r' < "$A" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
wedge=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c 'DISCONNECT')
pass=PASS
[ "$bmem" -ge 1 ] && [ "$amem" -ge 1 ] && [ "$vaxadd" -ge 1 ] \
  && [ "$bfinal" -eq 3 ] && [ "$afinal" -eq 3 ] && [ "$bug" -eq 0 ] || pass=FAIL
printf "[%s] %s  B-MEMBER=%s A-MEMBER=%s VAX-proposed-B=%s finalMEMBERrows(B/A)=%s/%s wedged=%s bugchecks=%s reconnect-lines=%s\n" \
       "$TAG" "$pass" "$bmem" "$amem" "$vaxadd" "$bfinal" "$afinal" "$wedge" "$bug" "$loss"
