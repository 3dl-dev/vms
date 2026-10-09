#!/bin/bash
# Does Node A's post-mount boot phase (ACP staging of the first-hop images)
# survive a Worker-style stall? Boot, wait for %OVMX-I-MOUNTED, then
# SIGSTOP/SIGCONT the qemu process hard for <starve_s> seconds, release it, and
# watch whether the boot reaches %STDRV-I-STARTUP.
#
# usage: stall-exp.sh <tag> <on_ms> <off_ms> <starve_s> <watch_s> [extra cmdline]
set -u
RIG=/tmp/4ff-rig
TAG=$1; ON=$2; OFF=$3; STARVE=$4; WATCH=$5; shift 5
EXTRA="${*:-no_timer_check}"
LOG=$RIG/exp-$TAG.log
rm -f "$LOG"; touch "$LOG"
TOTAL=$(( 60 + STARVE + WATCH ))
bash "$RIG/boot.sh" "$LOG" "$TOTAL" $EXTRA >/dev/null 2>&1 &
BOOTPID=$!

# Wait for the mount line (the phase boundary under study).
for i in $(seq 1 600); do
  grep -q 'OVMX-I-MOUNTED' "$LOG" && break
  python3 -c "import time;time.sleep(0.1)"
done
if ! grep -q 'OVMX-I-MOUNTED' "$LOG"; then
  echo "$TAG: never mounted"; wait $BOOTPID; exit 2
fi
echo "$TAG: mounted at $(grep -m1 'OVMX-I-MOUNTED' "$LOG" | cut -d] -f1 | tr -d '[ ')s -- starving ${ON}ms/${OFF}ms for ${STARVE}s"
bash "$RIG/starve.sh" "$ON" "$OFF" "$STARVE" >/dev/null 2>&1
echo "$TAG: released -- watching ${WATCH}s"
PROG_AT_RELEASE=$(wc -l < "$LOG")
python3 -c "import time;time.sleep($WATCH)"
echo "$TAG: lines at release=$PROG_AT_RELEASE now=$(wc -l < "$LOG")"
grep -qE 'STDRV-I-STARTUP' "$LOG" && echo "$TAG: RESULT reached STDRV" || echo "$TAG: RESULT NO STDRV (wedged or still staging)"
tail -6 "$LOG"
wait $BOOTPID 2>/dev/null
