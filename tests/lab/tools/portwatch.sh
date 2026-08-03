#!/bin/bash
# connwatch.sh <pod> <tag> <store> <duration> <identity> [ENV=V ...]
#
# vms-2f3 sec 4M.16: csbwatch.sh with SDA *SHOW CONNECTIONS/NODE=<identity>*
# in place of SHOW CLUSTER/NODE=. Lab-2 (pod) variant of connpoll.sh, which is
# lab-1 only.
#
# WHY. sec 4M.16 proved that NOTHING OVMX transmits differs by a single
# non-per-run byte between a join and a refused rejoin -- op9, the peer's op8 to
# us, and our 0x81/0x03 all carry zero class-discriminating offsets. The peer
# completes op8->op9 with us and then simply never sends op6 (the directory
# DISCONNECT-REQUEST), on every link it opens, starting ~0.7 s BEFORE it
# proposes our addition. So the discriminator is state the PEER holds about our
# identity.
#
# SHOW CONNECTIONS is the only oracle in the lab that names a REJECTION rather
# than describing a silence -- it carries a `Rej/Disconn Reason` field per CDT
# (sec 4d.9 identified it and it has still never been read on lab-2). The
# question it is pointed at: at the moment the peer declines to send op6, how
# many CDTs does it hold for our identity, in what state, and with what reason?
#
# Cadence note: the op6 decision itself is sub-millisecond and CANNOT be timed
# by console polling. This samples STATE, not timing -- that is the point.
#
# All of csbwatch.sh's harness lessons are inherited unchanged: stay inside SDA,
# narrow the query with /NODE=, no console markers, abort loudly on consecutive
# empty samples. See csbwatch.sh and connpoll.sh headers for why each matters.
set -u
POD=$1; TAG=$2; STORE=$3; DUR=$4; IDENT=$5; shift 5
NS=ovmx-lab
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
REPO=/home/baron/projects/vms
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

log "RUN $TAG pod=$POD ident=$IDENT store=$STORE dur=$DUR cad=${CAD}s env='$*'"

kubectl -n $NS cp "$REPO/build-d94/bin/SCSD.EXE" "$POD:/lab/SCSD.EXE" >/dev/null 2>&1
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
shot 'SHOW PORT/VC' 'T-PRE PORTVC'
shot "SHOW PORT/VC" "T-PRE $IDENT"

kubectl -n $NS exec "$POD" -- sh -c \
    "cd /lab && OVMX_SYSGEN_PATH=/lab/$TAG.sysgen $* ./SCSD.EXE --connect --duration $DUR --iface br0 > $L/scsd-$TAG.log 2>&1" &
SCSDP=$!
T0=$(date +%s)
log "SCSD started (tcpdump=$TCPD scsd=$SCSDP)"

while [ $(( $(date +%s) - T0 )) -lt "$DUR" ]; do
  shot "SHOW PORT/VC" "T+$(( $(date +%s) - T0 ))s" || break
done
shot 'SHOW PORT/VC' 'T-END PORTVC'
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
