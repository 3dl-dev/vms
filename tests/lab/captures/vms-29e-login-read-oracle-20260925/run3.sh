#!/usr/bin/env bash
# vms-29e oracle run 3: which SYSGEN parameter times out LOGINOUT's Username: and Password: reads.
set -u
NS=ovmx-lab; STS=vaxlab; NODE=vax1
OUTDIR=${OUTDIR:-/tmp/vms-29e-oracle-out}; mkdir -p "$OUTDIR"
log() { echo "[oracle3 $(date +%T)] $*" >&2; }
kexec() { kubectl -n "$NS" exec "$1" -- sh -c "$2"; }
lb() { echo "/lab/k8s-labs/$1/logs/$NODE"; }
send() { local b; b="$(printf '%s' "$2" | base64 -w0)"; kexec "$1" "{ echo $b | base64 -d; echo; } > $(lb "$1").log.in"; }
sendraw() { local b; b="$(printf '%s' "$2" | base64 -w0)"; kexec "$1" "echo $b | base64 -d > $(lb "$1").log.in"; }
tailc() { kexec "$1" "cat $(lb "$1").log 2>/dev/null | tr -cd '[:print:]\n\r' | tr -d '\r' | tail -${2:-8}" 2>/dev/null || true; }
waitp() { local i; for ((i=0;i<${3:-40};i+=1)); do tailc "$1" 4 | grep -qE "$2" && return 0; sleep 1; done; return 1; }
cnt() { kexec "$POD" "cat $(lb "$POD").log" 2>/dev/null | grep -a -c 'Timeout period expired'; }
measure() { # label -- time from now until a NEW 'Timeout period expired'
  local c0 t0 i; c0=$(cnt); t0=$(date +%s)
  for ((i=0;i<150;i++)); do [ "$(cnt)" -gt "$c0" ] && { log "$1: timeout after $(( $(date +%s)-t0 ))s"; return 0; }; sleep 1; done
  log "$1: no timeout within 150s"; }
PREV="$(kubectl -n $NS get sts $STS -o jsonpath='{.spec.replicas}')"
POD="$STS-$PREV"
cleanup() { log "scaling $STS back to $PREV"; kubectl -n $NS scale sts/$STS --replicas="$PREV" >/dev/null 2>&1 || true; }
trap cleanup EXIT
kubectl -n $NS scale sts/$STS --replicas=$((PREV+1)) >/dev/null
log "isolated replica $POD"
for i in $(seq 1 60); do kubectl -n $NS get pod "$POD" 2>/dev/null | grep -q Running && break; sleep 4; done
for i in $(seq 1 48); do tailc "$POD" 15 | grep -qE 'Username:|job terminated' && break; sleep 10; done
log "booted"
login() {
  send "$POD" ''; waitp "$POD" 'Username:' 30 || { send "$POD" ''; waitp "$POD" 'Username:' 30; }
  send "$POD" SYSTEM; waitp "$POD" 'Password:' 20 || log "no Password:"
  send "$POD" system; waitp "$POD" 'Welcome to OpenVMS' 45 || log "no welcome"
  send "$POD" ''; waitp "$POD" '\$ *$' 30
}
sysgen() { send "$POD" 'MCR SYSGEN'; sleep 2; for c in "$@"; do send "$POD" "$c"; sleep 1; done; send "$POD" 'WRITE ACTIVE'; sleep 1; send "$POD" 'SHOW/LGI'; sleep 2; send "$POD" 'EXIT'; sleep 2; }
login
# baseline (defaults 30/20)
send "$POD" 'LOGOUT'; waitp "$POD" 'logged out' 15; send "$POD" ''; waitp "$POD" 'Username: *$' 15; measure "U baseline PWD_TMO=30 RETRY_TMO=20"
# password prompt, baseline
send "$POD" ''; waitp "$POD" 'Username: *$' 15; send "$POD" SYSTEM; waitp "$POD" 'Password:' 10; measure "P baseline PWD_TMO=30 RETRY_TMO=20"
login
sysgen 'SET LGI_RETRY_TMO 60'
send "$POD" 'LOGOUT'; waitp "$POD" 'logged out' 15; send "$POD" ''; waitp "$POD" 'Username: *$' 15; measure "U PWD_TMO=30 RETRY_TMO=60"
send "$POD" ''; waitp "$POD" 'Username: *$' 15; send "$POD" SYSTEM; waitp "$POD" 'Password:' 10; measure "P PWD_TMO=30 RETRY_TMO=60"
login
sysgen 'SET LGI_RETRY_TMO 20' 'SET LGI_PWD_TMO 60'
send "$POD" 'LOGOUT'; waitp "$POD" 'logged out' 15; send "$POD" ''; waitp "$POD" 'Username: *$' 15; measure "U PWD_TMO=60 RETRY_TMO=20"
send "$POD" ''; waitp "$POD" 'Username: *$' 15; send "$POD" SYSTEM; waitp "$POD" 'Password:' 10; measure "P PWD_TMO=60 RETRY_TMO=20"
kexec "$POD" "cat $(lb "$POD").log" > "$OUTDIR/vax1.run4-lgi.console.raw" 2>/dev/null
log saved
