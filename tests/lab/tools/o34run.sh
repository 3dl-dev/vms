#!/bin/bash
# o34run.sh -- spec 4(O.34): does a REAL VAX survivor FULLY DEALLOCATE a
# departed member's CSB after a CLEAN leave, or does it linger in
# BRK_NON/long_break,removed the way OVMX's departure does (4(O.33))?
#
# THE decisive unmeasured observable. Every prior increment measured OVMX
# DEPARTING (4(O.33): its CSB lingers >100s on the real members). NONE measured
# a REAL VAX departing cleanly with a LONG survivor-side CSB poll. This runs
# exactly that, VAX-vs-VAX, on a virgin lab-2 pod -- no OVMX in the loop.
#
# Arms (single factor = clean-leave vs residual):
#   PRE   : VAX2 is MEMBER on VAX1's CSB list (baseline)
#   LEAVE : VAX2 does @SYS$SYSTEM:SHUTDOWN with the REMOVE_NODE option (the
#           authentic clean leave that emits the port last-gasp, 4(O.30)) --
#           NOT a kill. VAX1 is the survivor (REMOVE_NODE adjusts quorum).
#   WATCH : VAX1 parked in SDA, sampling SHOW CLUSTER/NODE=VAX2 every ~CAD s for
#           WATCH s. The deallocation detector is the narrow per-node dump going
#           to "SCSNODE not found" (CSB freed / dropped from the CSB list) vs
#           lingering with a State/Status (BRK_NON / long_break,removed).
#
# Reads: docs/cluster-protocol-spec.md 4(O.30)/(O.33); tests/lab/README.md;
# tools/departure.sh (the authentic SHUTDOWN dialogue), csbcycle.sh (narrow
# per-node CSB sampling that survives the OPCOM flood).
set -u
POD=${POD:-vaxlab-2}
NS=ovmx-lab
DEP=${DEP:-2}                 # departing node (VAX2); survivor is VAX1
SURV=1
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
CAD=${CAD:-6}
WATCH=${WATCH:-220}          # post-departure CSB poll window (s) -- > 100s (4(O.33) OVMX linger) and >> RECNXINTERVAL=20s
GRACE=${GRACE:-600}          # max wait for VMS shutdown to complete
OPT=${OPT:-REMOVE_NODE}      # shutdown option; REMOVE_NODE = fullest clean leave
TAG=${TAG:-o34}

S=$W/$TAG.status; : > "$S"
C=$W/$TAG.csb;    : > "$C"
T=$W/$TAG.trace;  : > "$T"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
# base64-safe console send (avoids $/quote mangling through kubectl exec sh -c)
sayN(){ local n=$1 cmd=$2 s=${3:-2} b; b=$(printf '%s\r' "$cmd" | base64 -w0);
  kubectl -n $NS exec "$POD" -- sh -c "echo $b | base64 -d > $L/vax$n.log.in" >/dev/null 2>&1; sleep "$s"; }
cleanN(){ tr -d '\000' < "$HOSTL/vax$1.log" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }

empty=0
# One SDA round-trip on the survivor: fire cmd, append only what arrived since,
# with a leading newline so markers never glue (csbcycle.sh sec 4k.7 lessons).
shot(){
  local cmd=$1 mark=$2 start bytes
  start=$(wc -c < "$HOSTL/vax$SURV.log")
  sayN "$SURV" "$cmd" "$CAD"
  { printf '\n########## %s ##########\n' "$mark"
    tail -c +$start "$HOSTL/vax$SURV.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
  } >> "$C"
  bytes=$(( $(wc -c < "$HOSTL/vax$SURV.log") - start ))
  if [ "$bytes" -lt 30 ]; then empty=$((empty+1));
    [ "$empty" -ge 5 ] && { log "FATAL survivor console wedged ($empty empty)"; return 1; }
  else empty=0; fi
  return 0
}
# classify VAX2's presence in the last narrow sample: MEMBER / <status> / GONE
classify(){
  local blk; blk=$(awk '/########## '"$1"' /{f=1} f' "$C" | tail -25)
  if echo "$blk" | grep -qaiE 'not found|no such|does not exist|SCSNODE'; then echo GONE
  elif echo "$blk" | grep -qaiE 'MEMBER'; then echo "MEMBER"
  else echo "$blk" | grep -aoiE 'BRK_NON|BRK_[A-Z]+|long_break|removed|new|closed|con_sent|reconnect' | sort -u | tr '\n' ',' | sed 's/,$//'; fi
}

log "RUN $TAG pod=$POD survivor=VAX$SURV departer=VAX$DEP opt=$OPT CAD=${CAD}s WATCH=${WATCH}s"

# --- Phase 0: both consoles at DCL, healthy 2-node cluster -------------------
for n in $SURV $DEP; do
  bash /data/training/vax/cluster/tools/lab2login.sh "$POD" $n | grep -q LOGGED-IN \
    || { log "FATAL: VAX$n not at DCL"; exit 2; }
done
log "both consoles LOGGED-IN"
for n in $SURV $DEP; do
  sayN "$n" 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST' 1
  sayN "$n" 'WRITE SYS$OUTPUT "CN'$n'_"+F$STRING(F$GETSYI("CLUSTER_NODES"))' 2
done
CN1=$(cleanN $SURV | grep -aoE "CN${SURV}_[0-9]+" | tail -1)
CN2=$(cleanN $DEP  | grep -aoE "CN${DEP}_[0-9]+"  | tail -1)
log "cluster health: survivor=$CN1 departer=$CN2 (want _2/_2)"
[ "$CN1" = "CN${SURV}_2" ] && [ "$CN2" = "CN${DEP}_2" ] || { log "FATAL: not a healthy 2-node cluster"; exit 2; }
# votes ground truth (does VAX$DEP hold a vote? REMOVE_NODE handles quorum, but record it)
sayN "$SURV" 'WRITE SYS$OUTPUT "EV_"+F$STRING(F$GETSYI("EXPECTED_VOTES"))+"_V_"+F$STRING(F$GETSYI("VOTES"))' 2
sayN "$DEP"  'WRITE SYS$OUTPUT "EV_"+F$STRING(F$GETSYI("EXPECTED_VOTES"))+"_V_"+F$STRING(F$GETSYI("VOTES"))' 2
log "survivor votes: $(cleanN $SURV | grep -aoE 'EV_[0-9]+_V_[0-9]+' | tail -1)  departer votes: $(cleanN $DEP | grep -aoE 'EV_[0-9]+_V_[0-9]+' | tail -1)"
sayN "$SURV" 'REPLY/ENABLE=(CLUSTER,CONFIG)' 2

# whole-run capture inside the pod
kx timeout $((WATCH+GRACE+120)) tcpdump -i br0 -w "$L/$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2

# --- Phase 1: park survivor in SDA, baseline ---------------------------------
sayN "$SURV" 'ANALYZE/SYSTEM' 8
sayN "$SURV" 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'PRE FULL CSB list'
shot "SHOW CLUSTER/NODE=VAX$DEP" "PRE VAX$DEP"
log "PRE: VAX$DEP on survivor = $(classify "PRE VAX$DEP")"

# --- Phase 2: VAX$DEP departs cleanly (SHUTDOWN + REMOVE_NODE) ---------------
# prompt-driven (departure.sh): answer what the console actually asks; feed the
# shutdown OPTION (REMOVE_NODE) at the options prompt. Survivor keeps polling
# so the CSB transition is captured live, not only before/after.
log "PHASE2 driving @SYS\$SYSTEM:SHUTDOWN opt=$OPT on VAX$DEP"
sayN "$DEP" '@SYS$SYSTEM:SHUTDOWN' 5
DEPT0=$(date +%s); ANSWERED=0
for i in $(seq 1 90); do
  tl=$(cleanN "$DEP" | grep -avE '^\s*$' | tail -1)
  echo "[$i] $tl" >> "$T"
  case "$tl" in
    *"minutes until final shutdown"*)     sayN "$DEP" '0' 3 ;;
    *"Reason for shutdown"*)              sayN "$DEP" 'O34 clean-leave CSB dealloc test' 3 ;;
    *"spin down the disk"*)               sayN "$DEP" 'NO' 3 ;;
    *"site-specific shutdown"*)           sayN "$DEP" 'NO' 3 ;;
    *"automatic system reboot"*)          sayN "$DEP" 'NO' 3 ;;
    *"When will the system be rebooted"*) sayN "$DEP" '' 3 ;;
    *"Shutdown options"*)                 sayN "$DEP" "$OPT" 3; ANSWERED=1 ;;
    *)
      if cleanN "$DEP" | tail -6 | grep -qaiE 'SHUTDOWN-I-|SYSTEM SHUTDOWN COMPLETE|halted|HALT instruct'; then break; fi
      # sample the survivor CSB even during the dialogue
      shot "SHOW CLUSTER/NODE=VAX$DEP" "DLG+$(( $(date +%s)-DEPT0 ))s" || true
      ;;
  esac
done
log "PHASE2 options answered=$ANSWERED; waiting for VAX$DEP to go down (grace ${GRACE}s)"

# wait for the departure to actually complete on VAX$DEP (be patient -- an
# emulated VAX shutdown takes minutes; the announcement is near the END).
DOWN=0
for i in $(seq 1 $((GRACE/CAD))); do
  if cleanN "$DEP" | grep -qai 'SYSTEM SHUTDOWN COMPLETE'; then log "VAX$DEP: SHUTDOWN COMPLETE (~$((i*CAD))s)"; DOWN=1; fi
  if cleanN "$DEP" | grep -avE '^[[:space:]]*$' | tail -4 | grep -qaiE 'halted|HALT instruct|sim>|SIMH>'; then log "VAX$DEP: halted to console (~$((i*CAD))s)"; DOWN=1; fi
  # keep polling the survivor's CSB the entire time
  shot "SHOW CLUSTER/NODE=VAX$DEP" "SD+$(( $(date +%s)-DEPT0 ))s" || break
  [ "$DOWN" = 1 ] && break
done
LEAVET=$(date +%s)
log "departure-complete marker seen=$DOWN; SURVIVOR CSB for VAX$DEP now = $(classify "SD")"

# --- Phase 3: LONG CSB poll on the survivor ---------------------------------
log "PHASE3 polling survivor CSB for VAX$DEP for ${WATCH}s (deallocation = GONE)"
FIRSTGONE=""
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$WATCH" ]; do
  el=$(( $(date +%s) - T0 ))
  shot "SHOW CLUSTER/NODE=VAX$DEP" "W+${el}s" || break
  st=$(classify "W+${el}s")
  echo "[watch +${el}s] VAX$DEP = $st" | tee -a "$S"
  if [ "$st" = GONE ] && [ -z "$FIRSTGONE" ]; then FIRSTGONE=$el; log ">>> CSB DEALLOCATED (GONE) at watch+${el}s (~$(( LEAVET-DEPT0 + el ))s after shutdown start)"; fi
done
shot 'SHOW CLUSTER' 'END FULL CSB list'
shot "SHOW CLUSTER/NODE=VAX$DEP" "END VAX$DEP"
sayN "$SURV" 'EXIT' 3

kx pkill -9 -f tcpdump >/dev/null 2>&1 || true
wait $TCPD 2>/dev/null || true

log "==== O34 RESULT ===="
log "survivor CSB for VAX$DEP: PRE=$(classify 'PRE VAX')  final=$(classify 'END VAX')"
if [ -n "$FIRSTGONE" ]; then
  log "VERDICT: real-VAX clean leave DEALLOCATES the survivor CSB at ~${FIRSTGONE}s into the watch window."
else
  log "VERDICT: real-VAX clean leave does NOT deallocate within ${WATCH}s -- CSB LINGERS (same class as OVMX, 4(O.33))."
fi
log "wrote $C (per-sample CSB), $S (timeline), $T (shutdown dialogue), pcap=$HOSTL/$TAG.pcap"
echo "===$TAG-DONE===" >> "$S"
