#!/usr/bin/env bash
# capture_lab.sh (rd vms-370) -- capture the keystroke-oracle goldens from a
# real OpenVMS console in YOUR OWN ovmx-lab pod.
#
#   tools/oracle/keystroke/capture_lab.sh vax   <pod> [password] [CASE...]
#   tools/oracle/keystroke/capture_lab.sh alpha <pod> [password] [CASE...]
#
# Copies ksplay.py + the case scripts into the pod and plays them into the
# node's console (OPA0:) through the console driver's RAW FIFO (<log>.raw:
# tests/lab/nodedrv.py and tests/lab-alpha/tools/srmdrv.py -R), which passes
# every byte verbatim and at once -- ^J and exact keystroke timing need it.
# Writes docs/oracle/keystroke/<CASE>/<vax73|alpha84>.ks.txt (+ .timing.jsonl).
#
# The pod must be one you own, booted to a SYSTEM-loginable console, and its
# console driver must have the raw FIFO (a pod from an image older than the
# vms-370 drivers: mount the in-repo nodedrv.py/srmdrv.py over the image's, as
# the vms-370 capture did with a ConfigMap). Rule 8: the goldens are observed
# output of the real system; nothing is disassembled.
#
# VAX TRAP: SIMH's console "WRU" (break to sim>) is ^E by default -- and ^E is
# VMS line editing (end of line). This script first moves WRU to ^\ (0x1C;
# `set console wru=1c` -- the value is HEX: `034` sets it to '4').
set -euo pipefail

ARCH="${1:?arch: vax|alpha}"; POD="${2:?pod}"; shift 2
PW="${1:-}"; [ $# -gt 0 ] && shift
NS="${OVMX_LAB_NS:-ovmx-lab}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
case "$ARCH" in
  vax)   NODE=vax1;   TAG=vax73;   LOG="/lab/k8s-labs/$POD/logs/vax1.log";        LABEL="OpenVMS VAX V7.3 OPA0:"; PW="${PW:-system}" ;;
  alpha) NODE=alpha1; TAG=alpha84; LOG="/lab/k8s-labs/$POD/alpha1/logs/alpha1.log"; LABEL="OpenVMS Alpha V8.4 OPA0:"; PW="${PW:-ovmxlab2026}" ;;
  *) echo "arch must be vax|alpha" >&2; exit 2 ;;
esac
kx() { kubectl -n "$NS" exec "$POD" -- sh -c "$1"; }

kx "test -p $LOG.raw" || { echo "no raw FIFO $LOG.raw in $POD -- see header" >&2; exit 1; }
kx "rm -rf /tmp/ks && mkdir -p /tmp/ks/cases"
kubectl cp "$HERE/ksplay.py" "$NS/$POD:/tmp/ks/ksplay.py"
for f in "$HERE"/cases/*.ks; do kubectl cp "$f" "$NS/$POD:/tmp/ks/cases/$(basename "$f")"; done

if [ "$ARCH" = vax ]; then
  # Only if ^E actually reached sim> (a pod started with wru=1c already set
  # passes ^E to VMS, where it is harmless at an empty DCL line).
  kx "printf '\005' > $LOG.raw; sleep 2; if tail -c 20 $LOG | grep -q 'sim> \$'; then \
      printf 'set console wru=1c\r' > $LOG.raw; sleep 1; printf 'cont\r' > $LOG.raw; sleep 2; \
      else printf '\025' > $LOG.raw; fi"
fi

CASES=""
for c in "$@"; do CASES="$CASES cases/$c.ks"; done
echo "[capture_lab] playing into $POD:$NODE OPA0: ($TAG) ..."
kx "cd /tmp/ks; nohup python3 ksplay.py run --transport fifo:$LOG --user SYSTEM --password '$PW' \
      --out out --label '$LABEL' $CASES > run.log 2>&1 & echo \$! > run.pid"
while kx "kill -0 \$(cat /tmp/ks/run.pid) 2>/dev/null"; do sleep 15; done
kx "cat /tmp/ks/run.log"

for t in $(kx "cd /tmp/ks/out && ls *.ks.txt"); do
  c="${t%.ks.txt}"
  mkdir -p "$REPO/docs/oracle/keystroke/$c"
  kubectl cp "$NS/$POD:/tmp/ks/out/$t" "$REPO/docs/oracle/keystroke/$c/$TAG.ks.txt"
  kubectl cp "$NS/$POD:/tmp/ks/out/$c.timing.jsonl" "$REPO/docs/oracle/keystroke/$c/$TAG.timing.jsonl"
done
echo "[capture_lab] goldens written under docs/oracle/keystroke/*/$TAG.ks.txt"
