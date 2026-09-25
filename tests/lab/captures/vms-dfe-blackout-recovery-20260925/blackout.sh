#!/bin/bash
# blackout.sh <tap> <log> <marker> <delay-s> <blackout-s>
# Wait (50 ms poll) until <marker> appears in <log>, wait <delay-s> more, then
# drop EVERY frame on <tap> for <blackout-s> with tc-netem, then restore.
# A transient loss of connectivity DURING an admission -- the fault the rd
# vms-dfe capture suffered -- made deterministic. netem only DROPS: nothing is
# injected on the wire and no frame is altered.
set -u
TAP="$1"; LOG="$2"; MARKER="$3"; DELAY="$4"; SECS="$5"
for _ in $(seq 1 12000); do
    grep -qaF "$MARKER" "$LOG" 2>/dev/null && break
    sleep 0.05
done
grep -qaF "$MARKER" "$LOG" 2>/dev/null || { echo "BLACKOUT: marker never appeared"; exit 1; }
echo "MARKER seen at $(date -u +%H:%M:%S.%3N): $MARKER"
sleep "$DELAY"
tc qdisc replace dev "$TAP" root netem loss 100%
echo "BLACKOUT on  $TAP at $(date -u +%H:%M:%S.%3N)"
tail -3 "$LOG" | tr -d '\r'
sleep "$SECS"
tc qdisc del dev "$TAP" root 2>/dev/null
echo "BLACKOUT off $TAP at $(date -u +%H:%M:%S.%3N) after ${SECS}s"
