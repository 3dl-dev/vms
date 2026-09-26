#!/bin/bash
# startloop.sh <artdir> <tag> <N>
# Honours MARK_LOG / MARK / BLACKOUT_S / TRIG_DELAY / JOIN_WAIT_BEATS / SETTLE_S.
#
# JOIN_WAIT_BEATS is how long one arm waits for the joiner's own
# "this node is now a VAXcluster member", in 2-second beats. It has to allow a
# WHOLE re-join cycle after a CLUEXIT (rd vms-0f9): a node that re-incarnates
# starts its admission again from the directory round, and 120 beats (240 s)
# cut two arms off mid-progress rather than mid-failure.
cd /lab/run-b36
setsid nohup env MARK_LOG="${MARK_LOG:-}" MARK="${MARK:-}" \
    BLACKOUT_S="${BLACKOUT_S:-45}" TRIG_DELAY="${TRIG_DELAY:-0}" \
    JOIN_WAIT_BEATS="${JOIN_WAIT_BEATS:-170}" SETTLE_S="${SETTLE_S:-60}" \
    bash /lab/run-b36/loop.sh "$1" "$2" "$3" > /lab/run-b36/loopout-$2.log 2>&1 &
sleep 1; echo "started $2"
