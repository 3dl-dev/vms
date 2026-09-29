#!/bin/bash
# stall.sh <tap-or-name> <watch-log> <marker> <delay-s> <stall-s>
#
# THE FAULT A WIRE BLACKOUT IS NOT (rd vms-8c54).
#
# A visitor's slower machine starves the emulator: the OVMX GUEST STOPS
# RUNNING. That is not the same fault as tc-netem dropping every frame on its
# tap, and the difference is the whole bug:
#
#   netem loss 100%  -- the guest keeps running and its frames are DESTROYED.
#                       The peer never sees them; on restore the guest's state
#                       and the wire agree that nothing got through.
#   SIGSTOP          -- the guest stops. Its frames are not destroyed, they are
#                       NOT YET WRITTEN; whatever its executive had decided
#                       before the stop is still true when it wakes, and it
#                       resumes mid-stream into a cluster that has moved on.
#
# So this injector SIGSTOPs the QEMU process tree of the node under test for
# <stall-s> seconds and then SIGCONTs it. Nothing on the wire is touched: no
# qdisc, no frame altered, no frame injected. The only thing that changes is
# whether that guest's CPU runs.
#
# Armed on <marker> appearing in <watch-log> -- for this item, the REAL VAX's
# own "completing VAXcluster state transition" after it proposed the node's
# addition, i.e. the instant the node became a member.
set -u
WHICH="$1"; LOG="$2"; MARKER="$3"; DELAY="$4"; SECS="$5"

pids_of() {
    # The qemu processes serving these taps. $WHICH may name SEVERAL taps
    # separated by commas: a visitor's machine starves the WHOLE browser tab,
    # so BOTH OVMX guests stop together, and that is a different fault again
    # from stalling one of them (rd vms-8c54).
    local pat
    pat=$(printf '%s' "$WHICH" | tr ',' '|')
    ps -eo pid,args | grep "qemu-system-x86_64" | grep -E "ifname=($pat)" \
        | grep -v grep | awk '{print $1}'
}

# grep -c PRINTS the count and EXITS 1 when the count is zero, so the old
# `|| echo 0` appended a SECOND line and every integer test below died with
# "integer expression expected" -- the whole L matrix ran with injected=0 and
# graded six arms that never had a fault in them. Count through one helper that
# returns exactly one integer.
count_of() {
    local n
    n=$(grep -ac "$MARKER" "$LOG" 2>/dev/null | head -1)
    case "$n" in ''|*[!0-9]*) n=0 ;; esac
    printf %s "$n"
}

base=$(count_of)
echo "STALL: baseline count of '$MARKER' = $base"
for _ in $(seq 1 12000); do
    n=$(count_of)
    [ "$n" -gt "$base" ] && break
    sleep 0.05
done
n=$(count_of)
if [ "$n" -le "$base" ]; then
    echo "STALL: marker never appeared -- no fault injected"; exit 1
fi
echo "MARKER seen at $(date -u +%H:%M:%S.%3N): $MARKER"
sleep "$DELAY"

P=$(pids_of)
if [ -z "$P" ]; then
    echo "STALL: no qemu found for $WHICH -- no fault injected"; exit 1
fi
echo "STALL on  $WHICH pids [$P] at $(date -u +%H:%M:%S.%3N) for ${SECS}s"
for p in $P; do kill -STOP "$p" 2>/dev/null; done
sleep "$SECS"
for p in $P; do kill -CONT "$p" 2>/dev/null; done
echo "STALL off $WHICH pids [$P] at $(date -u +%H:%M:%S.%3N) after ${SECS}s"
