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
# grep -c prints the count AND exits 1 on zero, so `|| echo 0` yielded two
# lines and every integer test on it died (rd vms-8c54).
STALL=$(grep -ac "STALL on" "$D/fault.out" 2>/dev/null | head -1)
case "$STALL" in ''|*[!0-9]*) STALL=0 ;; esac
STALLSECS=$(cat "$D/stall-seconds" 2>/dev/null || echo "?")

bmem=$(grep -ac 'this node is now a VAXcluster member' "$B")
amem=$(grep -ac 'this node is now a VAXcluster member' "$A")
# The VAX says it twice -- its %CNXMAN line and its OPCOM line -- and console
# interleaving can cut the first ("%CNXMAN,  p"), which graded arm S-4 FAIL
# while the VAX's OPCOM line read "proposed addition of node OVMXB" (rd
# vms-1f40). Same criterion, either rendering; gate-eval.mjs's VADD matches both.
vaxadd=$(grep -acE 'proposing addition of system OVMXB|proposed addition of node OVMXB' "$C")
bug=$(cat "$A" "$B" "$C" | grep -ac 'BUG CHECK\|BUGCHECK\|bugcheck')
loss=$(cat "$A" "$B" | grep -ac 'lost connection to a cluster member')
loop=$(cat "$A" "$B" | grep -ac 'closed: remote disconnect')
refused=$(cat "$A" "$B" | grep -ac 'given up on')
cluexit=$(cat "$A" "$B" | grep -ac 'CLUEXIT')
bfinal=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
afinal=$(tr -d '\r' < "$A" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c '| MEMBER')
wedge=$(tr -d '\r' < "$B" | tac | grep -m1 -B14 'View of Cluster' | tac | grep -c 'DISCONNECT')

# AN ARM WITH NO FAULT IN IT IS NOT A PASS (rd vms-8c54, measured: the first L
# matrix graded 6/6 PASS while `injected=0` on every arm -- the trigger had
# died on a shell integer test and no guest was ever stalled). A stall was
# asked for, so a stall must have happened, or the arm grades NOFAULT and
# counts for nothing.
pass=PASS
[ "$bmem" -ge 1 ] && [ "$amem" -ge 1 ] && [ "$vaxadd" -ge 1 ] \
  && [ "$bfinal" -eq 3 ] && [ "$afinal" -eq 3 ] && [ "$bug" -eq 0 ] \
  && [ "$loop" -le "$LOOP_MAX" ] || pass=FAIL
[ "$STALLSECS" = "?" ] || [ "$STALL" -ge 1 ] || pass=NOFAULT

printf "[%s] %s  stall=%ss injected=%s B-MEMBER=%s A-MEMBER=%s VAX-proposed-B=%s finalMEMBERrows(B/A)=%s/%s wedged=%s bugchecks=%s 4c9-loop=%s refused-giveup=%s cluexits=%s reconnect-lines=%s\n" \
       "$TAG" "$pass" "$STALLSECS" "$STALL" "$bmem" "$amem" "$vaxadd" "$bfinal" "$afinal" "$wedge" \
       "$bug" "$loop" "$refused" "$cluexit" "$loss"
