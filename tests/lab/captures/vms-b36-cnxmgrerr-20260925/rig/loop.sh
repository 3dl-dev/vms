#!/bin/bash
# loop.sh <art> <tag> <N> -- N consecutive arms of the rd vms-b36 repro, each
# graded independently and archived whole (consoles + pcap). One line per arm.
#
# A peer bugcheck can only be ATTRIBUTED from the wire, so the pcap is archived
# for every arm, not only the failing ones.
set -u
ART="$1"; TAG="$2"; N="$3"
R=/lab/run-b36
: > "$R/loop-$TAG.log"
for i in $(seq 1 "$N"); do
    echo "===== $TAG run $i/$N start $(date -u +%H:%M:%S) =====" | tee -a "$R/loop-$TAG.log"
    BLACKOUT_S="${BLACKOUT_S:-45}" bash "$R/runarm.sh" "$ART" "$TAG-$i" >> "$R/loop-$TAG.log" 2>&1
    for _ in $(seq 1 ${JOIN_WAIT_BEATS:-120}); do
        grep -qa 'this node is now a VAXcluster member' "$R/OVMXB.console.log" 2>/dev/null && break
        sleep 2
    done
    sleep "${SETTLE_S:-60}"
    D="$R/runs/$TAG-$i"; mkdir -p "$D"
    cp "$R"/OVMXA.console.log "$R"/OVMXB.console.log "$R"/VAXC.console.log \
       "$R"/blackout.out "$D"/ 2>/dev/null
    for p in $(ps -eo pid,args | grep 'tcpdum[p] -i brb36' | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 2
    gzip -c "$R"/b36.pcap > "$D"/b36.pcap.gz 2>/dev/null
    bash "$R/grade2.sh" "$D" "$TAG-$i" | tee -a "$R/loop-$TAG.log"
done
echo "===== $TAG done: $(grep -c ' PASS ' "$R/loop-$TAG.log") PASS / $(grep -c ' FAIL ' "$R/loop-$TAG.log") FAIL =====" | tee -a "$R/loop-$TAG.log"
