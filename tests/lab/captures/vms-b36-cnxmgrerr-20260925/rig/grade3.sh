#!/bin/bash
# grade3.sh <run-dir> <tag> -- grade one arm of the rd vms-0f9 campaign.
#
# WHAT COUNTS AS A PASS. The bar is Baron's: real VMS clusters do not crash.
#   1. the joiner reached MEMBER, and so did the first OVMX node;
#   2. the real VAX proposed the joiner's addition ITSELF;
#   3. BOTH OVMX nodes' own SHOW CLUSTER name all three systems MEMBER;
#   4. NOTHING bugchecked -- on any console;
#   5. the rd vms-4c9 accept-and-be-hung-up-on loop is ABSENT.
#
# (5) is new here and is measured, not assumed. The loop's own signature is the
# peer tearing down a connection this node had just accepted:
# "%CNXMAN, the VMS$VAXcluster connection to a cluster member closed: remote
# disconnect". A run in the loop showed 457-464 of them in 400 s; a healthy run
# shows 0-5, every one of them a real disconnect during the injected blackout.
# The threshold is 20 -- a twentyfold margin over the healthy runs and a
# twentyfold margin under the loop -- and the RAW COUNT is printed for every
# arm either way, so the number is never hidden behind the verdict.
#
# "reconnect-lines" stays REPORTED, not a criterion: this harness cuts the
# joiner's connectivity on purpose, so "lost connection to a cluster member,
# reconnecting" is the correct line for the fault that was injected.
set -u
D="$1"; TAG="$2"
B=$D/OVMXB.console.log; A=$D/OVMXA.console.log; C=$D/VAXC.console.log
LOOP_MAX="${LOOP_MAX:-20}"

bmem=$(grep -ac 'this node is now a VAXcluster member' "$B")
amem=$(grep -ac 'this node is now a VAXcluster member' "$A")
vaxadd=$(grep -ac 'proposing addition of system OVMXB' "$C")
bug=$(cat "$A" "$B" "$C" | grep -ac 'BUG CHECK\|BUGCHECK\|bugcheck')
loss=$(cat "$A" "$B" | grep -ac 'lost connection to a cluster member')
loop=$(cat "$A" "$B" | grep -ac 'closed: remote disconnect')
refused=$(cat "$A" "$B" | grep -ac 'given up on')
cluexit=$(cat "$A" "$B" | grep -ac 'CLUEXIT')
bfinal=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
afinal=$(tr -d '\r' < "$A" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
wedge=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c 'DISCONNECT')

pass=PASS
[ "$bmem" -ge 1 ] && [ "$amem" -ge 1 ] && [ "$vaxadd" -ge 1 ] \
  && [ "$bfinal" -eq 3 ] && [ "$afinal" -eq 3 ] && [ "$bug" -eq 0 ] \
  && [ "$loop" -le "$LOOP_MAX" ] || pass=FAIL

printf "[%s] %s  B-MEMBER=%s A-MEMBER=%s VAX-proposed-B=%s finalMEMBERrows(B/A)=%s/%s wedged=%s bugchecks=%s 4c9-loop=%s refused-giveup=%s cluexits=%s reconnect-lines=%s\n" \
       "$TAG" "$pass" "$bmem" "$amem" "$vaxadd" "$bfinal" "$afinal" "$wedge" \
       "$bug" "$loop" "$refused" "$cluexit" "$loss"
