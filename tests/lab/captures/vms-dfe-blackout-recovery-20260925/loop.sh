#!/bin/bash
# loop.sh <art> <tag> <N> -- N consecutive arms of the rd vms-dfe blackout
# repro, each graded independently. One line per arm on stdout.
set -u
ART="$1"; TAG="$2"; N="$3"
R=/lab/run-dfe
: > "$R/loop-$TAG.log"
for i in $(seq 1 "$N"); do
    echo "===== $TAG run $i/$N start $(date -u +%H:%M:%S) =====" | tee -a "$R/loop-$TAG.log"
    BLACKOUT_S=45 bash "$R/runarm.sh" "$ART" "$TAG-$i" >> "$R/loop-$TAG.log" 2>&1
    if ! grep -q "OVMXB booting" "$R/arm-$TAG-$i.out" 2>/dev/null; then :; fi
    # wait up to 240 s from B's boot for the membership verdict, then hold for
    # three SHOW CLUSTER polls so the final table is a fresh read.
    for _ in $(seq 1 120); do
        grep -qa 'this node is now a VAXcluster member' "$R/OVMXB.console.log" 2>/dev/null && break
        sleep 2
    done
    sleep 60
    bash "$R/grade.sh" "$TAG-$i" | tee -a "$R/loop-$TAG.log"
    D="$R/runs/$TAG-$i"; mkdir -p "$D"
    cp "$R"/OVMXA.console.log "$R"/OVMXB.console.log "$R"/VAXC.console.log \
       "$R"/blackout.out "$D"/ 2>/dev/null
    # the capture too: a peer bugcheck can only be attributed from the wire.
    for p in $(ps -eo pid,args | grep 'tcpdum[p] -i brdfe' | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 2
    gzip -c "$R"/dfe.pcap > "$D"/dfe.pcap.gz 2>/dev/null
done
echo "===== $TAG done: $(grep -c ' PASS ' "$R/loop-$TAG.log") PASS / $(grep -c ' FAIL ' "$R/loop-$TAG.log") FAIL =====" | tee -a "$R/loop-$TAG.log"
