#!/bin/bash
# csbwatch.sh <pod> <tag> <store> <duration> <identity> [ENV=V ...]
#
# vms-2f3 sec 4k.7: run ONE OVMX attempt against a lab-2 pod while VAX1 stays
# parked in SDA sampling `SHOW CLUSTER/NODE=<identity>`, so the peer's CSB for
# OUR identity is observed THROUGH the attempt rather than only before and after.
#
# WHY THIS TOOL EXISTS. sec 4j got the "before" and the "after" of the CSB but
# never the transition, and sec 4h.3 item 2 (poll the CSB through a refused
# rejoin) was written down and never run. sec 4k.5 then found the divergence
# point on the wire -- the peer completes round 1 and NEVER sends the DISC-REQ
# that tears down its own directory connection -- so the question is whether the
# CSB moves at that same instant. This is lab2run.sh's runner with csbcycle.sh's
# narrow per-node SDA sampling in place of the CLUSTER_NODES console poll.
#
# The CLUSTER_NODES verdict is LOST (the console is busy inside SDA) and is not
# missed: `SHOW CLUSTER/NODE=` names the state directly, and "SCSNODE not found"
# is itself a datum -- it says the peer has not created a CSB for us yet.
#
# ⚠ LAB-2 ONLY. Different SIMH binary from lab-1; never mix them in one
# comparison (tests/lab/README.md, and the lab-2 warning atop the handoff).
set -u
POD=$1; TAG=$2; STORE=$3; DUR=$4; IDENT=$5; shift 5
NS=ovmx-lab
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
REPO=${REPO:-/home/baron/projects/vms}
# vms-449: same override lab2run.sh grew in vms-578 -- a worktree runs its OWN
# build instead of the shared build-d94 artifact another agent may be mid-run
# with. Default unchanged.
SCSD_BIN=${SCSD_BIN:-$REPO/build-d94/bin/SCSD.EXE}
CAD=${CAD:-5}
MAXEMPTY=${MAXEMPTY:-6}

STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
[ -r "$STORE" ] || { echo "csbwatch: FATAL -- unreadable store '$STORE'" >&2; exit 2; }
kubectl -n $NS get pod "$POD" >/dev/null 2>&1 || { echo "csbwatch: FATAL -- no pod $POD" >&2; exit 2; }

S=$W/$TAG.status; : > "$S"; C=$W/$TAG.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax1.log.in" >/dev/null 2>&1; sleep "${2:-2}"; }

empty=0
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
    [ "$empty" -ge "$MAXEMPTY" ] && { log "FATAL -- console wedged ($empty empty samples)"; \
      echo "===$TAG-CONSOLE-DEAD===" >> "$S"; return 1; }
  else empty=0; fi
  return 0
}

bash /data/training/vax/cluster/tools/lab2login.sh "$POD" 1 | grep -q LOGGED-IN \
  || { echo "csbwatch: FATAL -- $POD vax1 not at DCL" >&2; exit 2; }

log "RUN $TAG pod=$POD ident=$IDENT store=$STORE dur=$DUR cad=${CAD}s daemon=$SCSD_BIN env='$*'"

[ -x "$SCSD_BIN" ] || { echo "csbwatch: FATAL -- no daemon at $SCSD_BIN" >&2; exit 2; }
kubectl -n $NS cp "$SCSD_BIN" "$POD:/lab/SCSD.EXE" >/dev/null 2>&1
kubectl -n $NS cp "$STORE" "$POD:/lab/$TAG.sysgen" >/dev/null 2>&1
if [ -r "$STORE.cluster" ]; then
  kubectl -n $NS cp "$STORE.cluster" "$POD:/lab/$TAG.sysgen.cluster" >/dev/null 2>&1
  log "carried prior-admission sidecar ($(wc -c < "$STORE.cluster") bytes) -- this identity HAS been admitted before"
else
  kx rm -f "/lab/$TAG.sysgen.cluster" 2>/dev/null
  log "no prior-admission sidecar -- this identity has never been admitted anywhere"
fi
kx chmod +x /lab/SCSD.EXE

# Hold the exec sessions open on the host side (lab2run.sh's lesson: a nohup'd
# tcpdump dies with the exec teardown and yields a short capture).
kubectl -n $NS exec "$POD" -- timeout $((DUR+40)) \
    tcpdump -i br0 -w "$L/d94-$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!
sleep 2

# Park VAX1 in SDA and take the PRE sample BEFORE SCSD starts, so the baseline
# is the peer's state for this identity with nothing on the wire.
say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'T-PRE FULL'
shot "SHOW CLUSTER/NODE=$IDENT" "T-PRE $IDENT"

kubectl -n $NS exec "$POD" -- sh -c \
    "cd /lab && OVMX_SYSGEN_PATH=/lab/$TAG.sysgen $* ./SCSD.EXE --connect --duration $DUR --iface br0 > $L/scsd-$TAG.log 2>&1" &
SCSDP=$!
T0=$(date +%s)
log "SCSD started (tcpdump=$TCPD scsd=$SCSDP)"

while [ $(( $(date +%s) - T0 )) -lt "$DUR" ]; do
  shot "SHOW CLUSTER/NODE=$IDENT" "T+$(( $(date +%s) - T0 ))s" || break
done
shot 'SHOW CLUSTER' 'T-END FULL'
say 'EXIT' 4

kx pkill -f SCSD.EXE 2>/dev/null
wait $SCSDP 2>/dev/null; wait $TCPD 2>/dev/null

log "XITDONE=$(kx grep -ac XITDONE $L/scsd-$TAG.log 2>/dev/null)  RETX=$(kx grep -ac 'RETRANSMIT 0x7b' $L/scsd-$TAG.log 2>/dev/null)"
log "identity on the wire: $(strings -a "$HOSTL/d94-$TAG.pcap" 2>/dev/null | grep -oE 'OVMX[A-Z0-9]{2}' | sort -u | tr '\n' ' ')"
if kubectl -n $NS exec "$POD" -- test -r "/lab/$TAG.sysgen.cluster" 2>/dev/null; then
  kubectl -n $NS cp "$POD:/lab/$TAG.sysgen.cluster" "$STORE.cluster" >/dev/null 2>&1
  log "pulled prior-admission sidecar back ($(wc -c < "$STORE.cluster" 2>/dev/null) bytes)"
fi
log "wrote $C"
echo "===$TAG-DONE===" >> "$S"
