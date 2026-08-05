#!/bin/bash
# csbcycle.sh <pod> <tag> [node ...]  -- watch CSBs on a peer across a REAL
# node's kill -> removal -> rejoin, on a pod that ALSO carries dead/refused OVMX
# identities, so every history appears in the same dump.
#
# vms-2f3 sec 4j: this is what proved a dead REAL node's CSB is identical to a
# dead OVMX identity's (so holding a CSB is not the problem), and that
# readmission ALLOCATES A NEW CSB at a new address with a fresh CDT, which OVMX
# never gets.
#
# --------------------------------------------------------------------------
# sec 4j.6 recorded two defects in the first version. BOTH ARE FIXED HERE, and
# the first one's diagnosis was WRONG -- see sec 4k.7:
#
#   1. CONSOLE DEATH.  The first version fired a bare `SHOW CLUSTER` every
#      sample: ~6.3 KB each. It died the moment VAX2 rebooted, i.e. exactly when
#      the OPCOM flood hits -- the documented console-overrun mode (sec 4e.1;
#      connpoll.sh's first version lost 3 of 4 snapshots the same way).
#      It was NOT the backgrounded `kubectl exec ... nodedrv.py &`: lab2rejoin.sh
#      reboots with the identical construct and its console survived.
#      FIX: sample `SHOW CLUSTER/NODE=<name>` per tracked node -- a few hundred
#      bytes each instead of 6.3 KB. `/NODE=` and `/CSID=` are verified
#      supported on this VMS (they return semantic errors, not %CLI-W-SYNTAX;
#      `/CSB` IS a syntax error). The full `SHOW CLUSTER` is taken ONCE at the
#      start and ONCE at the end, where no flood is in progress.
#
#   2. MARKER GLUE.  `tail -c +N` output need not end in a newline, so the next
#      `########## T+Ns ##########` landed on the same line as the previous
#      sample's last byte and `grep '^#####'` found 1 marker in 26.
#      FIX: emit a leading newline before every marker.
#
# Also new: empty-sample detection. If N consecutive samples come back with no
# console output the console is wedged and the rest of the run is worthless, so
# say so loudly and stop rather than writing 17 empty samples.
# --------------------------------------------------------------------------
#
# Stays parked inside SDA and slices the console INCREMENTALLY per sample, so
# timestamps interleave with samples instead of all landing at the end (which is
# lab2rejoin.sh's known flaw -- its ORDER is trustworthy, its TIMING is not).
set -u
POD=$1; TAG=$2; shift 2
NODES=("$@"); [ ${#NODES[@]} -gt 0 ] || NODES=(VAX2)
NS=ovmx-lab
LAB=/lab/k8s-labs/$POD; L=$LAB/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
CAD=${CAD:-6}; DEAD=${DEAD:-75}; TOTAL=${TOTAL:-300}
MAXEMPTY=${MAXEMPTY:-4}

S=$W/$TAG.status; : > "$S"; C=$W/$TAG.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax1.log.in" >/dev/null 2>&1; sleep "${2:-2}"; }

bash /data/training/vax/cluster/tools/lab2login.sh "$POD" 1 | grep -q LOGGED-IN \
  || { echo "csbcycle: FATAL -- $POD vax1 not at DCL" >&2; exit 2; }

empty=0
# One console round-trip: fire $1 inside SDA, append ONLY what arrived since.
# Leading newline so the marker can never glue to the previous sample.
shot(){
  local cmd=$1 mark=$2 start bytes
  start=$(wc -c < "$HOSTL/vax1.log")
  say "$cmd" "$CAD"
  { printf '\n########## %s ##########\n' "$mark"
    tail -c +$start "$HOSTL/vax1.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
  } >> "$C"
  bytes=$(( $(wc -c < "$HOSTL/vax1.log") - start ))
  if [ "$bytes" -lt 40 ]; then
    empty=$((empty+1))
    [ "$empty" -ge "$MAXEMPTY" ] && {
      log "FATAL -- $empty consecutive empty samples: VAX1's console is wedged."
      log "         Everything after this point is worthless. Stopping."
      log "         (sec 4k.7: shrink the per-sample output or lengthen CAD.)"
      echo "===$TAG-CONSOLE-DEAD===" >> "$S"; exit 3; }
  else
    empty=0
  fi
}

# One sample = one narrow per-node CSB dump for each tracked node.
sample(){
  local mark=$1 n
  for n in "${NODES[@]}"; do shot "SHOW CLUSTER/NODE=$n" "$mark $n"; done
}

log "RUN $TAG pod=$POD nodes=${NODES[*]} cadence=${CAD}s dead=${DEAD}s total=${TOTAL}s"
say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'T-PRE FULL'      # full dump once, before any flood
sample 'T-PRE'

V2PID=$(kx sh -c 'for p in $(pgrep -x vax); do case "$(readlink /proc/$p/cwd)" in *vax2) echo $p;; esac; done' | tr -d "\r" | head -1)
[ -n "$V2PID" ] || { log "FATAL -- no VAX2 simh pid"; exit 2; }
log "VAX2 simh pid=$V2PID -- SIGKILL"
kx kill -9 "$V2PID"
T0=$(date +%s); rebooted=0
while [ $(( $(date +%s) - T0 )) -lt "$TOTAL" ]; do
  el=$(( $(date +%s) - T0 ))
  if [ $rebooted -eq 0 ] && [ $el -ge $DEAD ]; then
    log "T+${el}s REBOOTING VAX2 (unchanged identity)"
    kubectl -n $NS exec "$POD" -- sh -c \
      "cd $LAB && python3 /usr/local/bin/nodedrv.py $LAB/vax2 $L/vax2.log --date '2-AUG-2026 02:00' --boot 'B/R5:10000000 DUA0'" >/dev/null 2>&1 &
    rebooted=1
  fi
  sample "T+${el}s"
done
shot 'SHOW CLUSTER' 'T-END FULL'
say 'EXIT' 4
log "wrote $C"
echo "===$TAG-DONE===" >> "$S"
