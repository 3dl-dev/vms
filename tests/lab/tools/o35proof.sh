#!/bin/bash
# o35proof.sh -- spec 4(O.35) determinism proof. N consecutive RETURNS of one
# identity, each starting from a CONFIRMED CLUSTER_NODES=2 (the departed node
# fully dropped) so every win is a genuine 2->3. Oracle = each member's
# F$GETSYI("CLUSTER_NODES"). Same patched Debug daemon; MODE selects kill-switch:
#   MODE=ctrl -> OVMX_JOIN_ALL_MEMBERS unset  (current behaviour = reliable LOSE)
#   MODE=fix  -> OVMX_JOIN_ALL_MEMBERS=1       (the 4(O.35) total-connectivity fix)
# Departures are CLEAN: the daemon exits at --duration expiry (SIGALRM ->
# teardown + port last-gasp), never SIGKILL.
set -u
POD=${POD:-vaxlab-2}; NS=ovmx-lab
IDENT=OVMXE0
STORE=/data/training/vax/cluster/work/OVMXE0.sysgen
DAEMON=${DAEMON:-/data/training/vax/o35fix-SCSD.EXE}
MODE=${MODE:-fix}
N=${N:-10}; DUR=${DUR:-20}; DROP_TMO=${DROP_TMO:-60}
L=/lab/k8s-labs/$POD/logs; HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
EXTRA=""; [ "$MODE" = fix ] && EXTRA="OVMX_JOIN_ALL_MEMBERS=1"
S=$W/o35proof-$MODE.status; : > "$S"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
sayN(){ local n=$1 cmd=$2 s=${3:-1} b; b=$(printf '%s\r' "$cmd" | base64 -w0);
  kubectl -n $NS exec "$POD" -- sh -c "echo $b | base64 -d > $L/vax$n.log.in" >/dev/null 2>&1; sleep "$s"; }
cn(){ local n=$1 start; start=$(wc -c < "$HOSTL/vax$n.log")
  sayN "$n" 'WRITE SYS$OUTPUT "QQ"+F$STRING(F$GETSYI("CLUSTER_NODES"))' 1
  tail -c +$start "$HOSTL/vax$n.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -aoE 'QQ[0-9]+' | tail -1 | sed 's/QQ//'; }
# wait until BOTH members report CLUSTER_NODES=2 (departed node dropped), timeout DROP_TMO
wait_cn2(){ local T0=$(date +%s) c1 c2
  while [ $(( $(date +%s) - T0 )) -lt "$DROP_TMO" ]; do
    c1=$(cn 1); c2=$(cn 2)
    [ "${c1:-9}" = 2 ] && [ "${c2:-9}" = 2 ] && { echo "2"; return 0; }
    sleep 2
  done
  echo "${c1:-?}/${c2:-?}"; return 1; }

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
kubectl -n $NS cp "$DAEMON" "$POD:/lab/o35p-SCSD.EXE" >/dev/null 2>&1
for lib in 'LIBVMSFS$SHR.EXE' 'LIBVMSLNM$SHR.EXE'; do kubectl -n $NS cp "$W/$lib" "$POD:/lab/$lib" >/dev/null 2>&1; done
kubectl -n $NS cp "$STORE" "$POD:/lab/o35p.sysgen" >/dev/null 2>&1
kx rm -f /lab/o35p.sysgen.cluster >/dev/null 2>&1 || true
kx chmod +x /lab/o35p-SCSD.EXE
log "PROOF mode=$MODE daemon_md5=$(md5sum "$DAEMON"|cut -c1-12) env='$EXTRA' N=$N DUR=$DUR"
for n in 1 2; do sayN "$n" 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST' 1; done
log "pre CN: vax1=$(cn 1) vax2=$(cn 2) (want 2/2)"

run_daemon(){ local dur=$1 tag=$2
  kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/o35p.sysgen $EXTRA /lab/o35p-SCSD.EXE --connect --duration $dur --iface br0 > $L/scsd-o35p-$tag.log 2>&1" & }

# seed: F1 fresh first-join, then CLEAN leave at duration expiry -> residual CSB
run_daemon 18 F1
T0=$(date +%s); while [ $(( $(date +%s) - T0 )) -lt 22 ]; do sleep 2; done   # let it join + clean-leave
kx pkill -f SCSD.EXE >/dev/null 2>&1 || true
d=$(wait_cn2); log "seed F1 done; departed node dropped -> CN=$d"

WINS=0; STREAK=0; MINSTREAK=999; RESULTS=""
for i in $(seq 1 "$N"); do
  pre=$(wait_cn2)
  if [ "$pre" != 2 ]; then log "return #$i: SKIP-SETUP (CN never dropped to 2: $pre)"; RESULTS="$RESULTS X"; STREAK=0; continue; fi
  run_daemon "$DUR" "r$i"
  best=2; T0=$(date +%s)
  while [ $(( $(date +%s) - T0 )) -lt "$DUR" ]; do
    c1=$(cn 1); c2=$(cn 2)
    [ "${c1:-2}" -gt "$best" ] 2>/dev/null && best=$c1
    [ "${c2:-2}" -gt "$best" ] 2>/dev/null && best=$c2
    [ "$best" -ge 3 ] && break
  done
  kx pkill -f SCSD.EXE >/dev/null 2>&1 || true   # SIGTERM -> clean leave
  if [ "$best" -ge 3 ]; then WINS=$((WINS+1)); STREAK=$((STREAK+1)); RESULTS="$RESULTS W"; log "return #$i: WIN  (2 -> $best)"
  else STREAK=0; RESULTS="$RESULTS L"; log "return #$i: LOSE (stayed $best)"; fi
done
kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
log "==== RESULT mode=$MODE: $WINS/$N wins; sequence:$RESULTS ===="
echo "===o35proof-$MODE-DONE===" >> "$S"
