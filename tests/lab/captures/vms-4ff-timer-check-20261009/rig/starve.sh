#!/bin/bash
# Starve a running qemu-system-x86_64 the way a browser deschedules a Worker:
# SIGSTOP for <off> ms, SIGCONT for <on> ms, for <dur> seconds.
# usage: starve.sh <on_ms> <off_ms> <dur_s>
set -u
ON=$1; OFF=$2; DUR=$3
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
  pids=$(pgrep -u "$USER" -f 'qemu-system-x86_64 -nographic' | tr '\n' ' ')
  [ -z "$pids" ] && { sleep 0.05; continue; }
  kill -STOP $pids 2>/dev/null
  python3 -c "import time;time.sleep($OFF/1000.0)"
  kill -CONT $pids 2>/dev/null
  python3 -c "import time;time.sleep($ON/1000.0)"
done
