#!/usr/bin/env bash
# vms-29e oracle run 2: periodic broadcasts (REPLY/TERMINAL + REQUEST->OPCOM)
# hitting OPA0 while LOGINOUT reads Username:/Password: with a partial line typed.
set -u
NS=ovmx-lab; STS=vaxlab; NODE=vax1
OUTDIR=${OUTDIR:-/tmp/vms-29e-oracle-out}; mkdir -p "$OUTDIR"
log() { echo "[oracle2 $(date +%T)] $*" >&2; }
kexec() { kubectl -n "$NS" exec "$1" -- sh -c "$2"; }
lb() { echo "/lab/k8s-labs/$1/logs/$NODE"; }
send() { local b; b="$(printf '%s' "$2" | base64 -w0)"; kexec "$1" "{ echo $b | base64 -d; echo; } > $(lb "$1").log.in"; }
sendraw() { local b; b="$(printf '%s' "$2" | base64 -w0)"; kexec "$1" "echo $b | base64 -d > $(lb "$1").log.in"; }
tailc() { kexec "$1" "cat $(lb "$1").log 2>/dev/null | tr -cd '[:print:]\n\r' | tr -d '\r' | tail -${2:-8}" 2>/dev/null || true; }
waitp() { local i; for ((i=0;i<${3:-40};i+=1)); do tailc "$1" 10 | grep -qE "$2" && return 0; sleep 1; done; return 1; }
nb() { kexec "$POD" "cat $(lb "$POD").log" 2>/dev/null | grep -a -c 'BCAST[0-9]'; }
waitnext() { local c0; c0=$(nb); local i; for ((i=0;i<15;i++)); do [ "$(nb)" -gt "$c0" ] && return 0; sleep 1; done; return 1; }

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
login
send "$POD" 'SET TERMINAL/PAGE=0/WIDTH=132'; sleep 2
send "$POD" 'OPEN/WRITE F SYS$COMMON:[SYSMGR]BC29E.COM'; sleep 1
send "$POD" 'WRITE F "$ N = 0"'; sleep 1
send "$POD" 'WRITE F "$ LOOP:"'; sleep 1
send "$POD" 'WRITE F "$ N = N + 1"'; sleep 1
send "$POD" "WRITE F \"\$ REPLY/TERMINAL=OPA0: BCAST'N'\""; sleep 1
send "$POD" "WRITE F \"\$ IF (N/3)*3 .EQ. N THEN REQUEST OPCOMMSG'N'\""; sleep 1
send "$POD" 'WRITE F "$ WAIT 00:00:05"'; sleep 1
send "$POD" 'WRITE F "$ IF N .LT. 40 THEN GOTO LOOP"'; sleep 1
send "$POD" 'CLOSE F'; sleep 1
send "$POD" 'TYPE SYS$COMMON:[SYSMGR]BC29E.COM'; sleep 2
send "$POD" 'RUN/DETACHED SYS$SYSTEM:LOGINOUT/INPUT=SYS$COMMON:[SYSMGR]BC29E.COM/OUTPUT=SYS$COMMON:[SYSMGR]BC29E.LOG/PROCESS_NAME=BC29E'; sleep 3
waitnext && log "broadcasts flowing"
send "$POD" 'WRITE SYS$OUTPUT "%%29E-A2-BEGIN%%"'; sleep 1
send "$POD" 'LOGOUT'; waitp "$POD" 'logged out' 15
send "$POD" ''; waitp "$POD" 'Username:' 15 && log "A: at Username:"
sendraw "$POD" 'SYS'; log "A: typed SYS"
waitnext && log "A: a broadcast arrived during the Username: read"
sleep 1; sendraw "$POD" $'TEM\r'; log "A: typed TEM<CR>"
waitp "$POD" 'Password:' 10 && log "A: at Password:"
sendraw "$POD" 'sys'; log "A: typed sys"
waitnext && log "A: a broadcast arrived during the Password: read"
sleep 1; sendraw "$POD" $'tem\r'; log "A: typed tem<CR>"
waitp "$POD" 'Welcome to OpenVMS|Last interactive' 30 && log "A: login completed"
sleep 3; send "$POD" ''; sleep 3
send "$POD" 'STOP BC29E'; sleep 2
send "$POD" 'WRITE SYS$OUTPUT "%%29E-A2-END%%"'; sleep 2
send "$POD" 'TYPE SYS$COMMON:[SYSMGR]BC29E.LOG'; sleep 4
kexec "$POD" "cat $(lb "$POD").log" > "$OUTDIR/vax1.run3.console.raw" 2>/dev/null
log "saved $OUTDIR/vax1.run3.console.raw"
