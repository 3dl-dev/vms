#!/usr/bin/env bash
# vms-29e oracle: what a REAL OpenVMS VAX V7.3 terminal does when
#  (A) a broadcast (REPLY/TERMINAL) and an OPCOM message (REQUEST) arrive while
#      LOGINOUT is reading Username: / Password: with a partial line typed, and
#  (B) the Username: prompt is left idle past LGI_PWD_TMO and SYSTEM<CR> is then
#      typed in one burst.
# Isolated replica (sts vaxlab scaled +1), FIFO drive only, scaled back on exit.
set -u
NS=ovmx-lab; STS=vaxlab; NODE=vax1
OUTDIR=${OUTDIR:-/tmp/vms-29e-oracle-out}; mkdir -p "$OUTDIR"
log() { echo "[oracle $(date +%T)] $*" >&2; }
kexec() { kubectl -n "$NS" exec "$1" -- sh -c "$2"; }
lb() { echo "/lab/k8s-labs/$1/logs/$NODE"; }
send() { local b; b="$(printf '%s' "$2" | base64 -w0)"; kexec "$1" "{ echo $b | base64 -d; echo; } > $(lb "$1").log.in"; }
sendraw() { local b; b="$(printf '%s' "$2" | base64 -w0)"; kexec "$1" "echo $b | base64 -d > $(lb "$1").log.in"; }
tailc() { kexec "$1" "cat $(lb "$1").log 2>/dev/null | tr -cd '[:print:]\n\r' | tr -d '\r' | tail -${2:-8}" 2>/dev/null || true; }
waitp() { local i; for ((i=0;i<${3:-40};i+=2)); do tailc "$1" 10 | grep -qE "$2" && return 0; sleep 2; done; return 1; }

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
send "$POD" 'MCR SYSGEN SHOW/LGI'; sleep 5
send "$POD" 'SHOW TERMINAL OPA0:'; sleep 4
send "$POD" 'OPEN/WRITE F SYS$LOGIN:BCAST29E.COM'; sleep 1
send "$POD" 'WRITE F "$ WAIT 00:00:35"'; sleep 1
send "$POD" 'WRITE F "$ REPLY/TERMINAL=OPA0: ""ORACLE-BCAST-ONE"""'; sleep 1
send "$POD" 'WRITE F "$ WAIT 00:00:15"'; sleep 1
send "$POD" 'WRITE F "$ REQUEST ""ORACLE-OPCOM-TWO"""'; sleep 1
send "$POD" 'CLOSE F'; sleep 1
send "$POD" 'TYPE SYS$LOGIN:BCAST29E.COM'; sleep 2
send "$POD" 'RUN/DETACHED SYS$SYSTEM:LOGINOUT/INPUT=SYS$LOGIN:BCAST29E.COM/OUTPUT=SYS$LOGIN:BCAST29E.LOG/PROCESS_NAME=BCAST29E'
T0=$(date +%s); log "detached started T0"
send "$POD" 'WRITE SYS$OUTPUT "%%29E-A-BEGIN%%"'; sleep 1
send "$POD" 'LOGOUT'; sleep 6
send "$POD" ''; waitp "$POD" 'Username:' 20 && log "A: at Username:"
sleep 2; sendraw "$POD" 'SYS'; log "A: typed SYS (no CR)"
waitp "$POD" 'ORACLE-BCAST-ONE' 40 && log "A: broadcast one seen at +$(( $(date +%s)-T0 ))s"
sleep 4; sendraw "$POD" $'TEM\r'; log "A: typed TEM<CR>"
waitp "$POD" 'Password:' 20 && log "A: at Password:"
sleep 2; sendraw "$POD" 'sys'; log "A: typed sys (no CR)"
waitp "$POD" 'ORACLE-OPCOM-TWO' 40 && log "A: opcom two seen at +$(( $(date +%s)-T0 ))s"
sleep 4; sendraw "$POD" $'tem\r'; log "A: typed tem<CR>"
waitp "$POD" 'Welcome to OpenVMS|Last interactive' 45 && log "A: login completed"
send "$POD" ''; waitp "$POD" '\$ *$' 30
send "$POD" 'WRITE SYS$OUTPUT "%%29E-A-END%%"'; sleep 2
# (B) idle timeout then a one-burst SYSTEM<CR>
send "$POD" 'WRITE SYS$OUTPUT "%%29E-B-BEGIN%%"'; sleep 1
send "$POD" 'LOGOUT'; sleep 6
send "$POD" ''; waitp "$POD" 'Username:' 20 && log "B: at Username: -- idling"
waitp "$POD" 'Timeout period expired|expired' 120 && log "B: timeout seen"
sleep 10
sendraw "$POD" $'SYSTEM\r'; log "B: typed SYSTEM<CR> in one burst"
sleep 15
if tailc "$POD" 6 | grep -q 'Password:'; then log "B: Password: reached from the one burst"; sendraw "$POD" $'system\r'; sleep 20;
else log "B: no Password: after the burst"; fi
log "B: done"
kexec "$POD" "cat $(lb "$POD").log" > "$OUTDIR/vax1.console.raw" 2>/dev/null
log "saved $OUTDIR/vax1.console.raw ($(wc -c < "$OUTDIR/vax1.console.raw") bytes)"
