#!/bin/bash
# startloop.sh <artdir> <tag> <N>  -- honours MARK_LOG / MARK / BLACKOUT_S / TRIG_DELAY
cd /lab/run-b36
setsid nohup env MARK_LOG="${MARK_LOG:-}" MARK="${MARK:-}" \
    BLACKOUT_S="${BLACKOUT_S:-45}" TRIG_DELAY="${TRIG_DELAY:-0}" \
    bash /lab/run-b36/loop.sh "$1" "$2" "$3" > /lab/run-b36/loopout-$2.log 2>&1 < /dev/null &
sleep 1; echo "started $2"
