#!/bin/bash
# coord358.sh <pod> <tag> <storeS> <identS> <storeF> <identF>  (vms-358, spec 4(O.28))
#
# SCSSYSTEMID-FRESHNESS single-factor flip, coordinator-side SDA.
# One boot, coordinator = VAX2, parked in ANALYZE/SYSTEM the whole run.
#
#   A  FRESH join id_S (no sidecar)          -> control, expect XITDONE=1
#   B  CRASH id_S, FAST return id_S (GAP<RECNX) -> the failing arm; SDA reads
#      the coordinator's retained per-id state (CSB/CDT/votes) for id_S
#   C  FAST return presenting FRESH id_F      -> the FLIP; the coordinator has
#      never held id_F, so it can only take NEW->JOIN. expect XITDONE=1
#
# The ONLY factor differing between B (fail) and C (admit) is the presented
# SCSSYSTEMID. Everything else -- pod, boot, daemon, timing, op02 form -- is held.
# Reads SHOW CLUSTER (votes/quorum + CSB list), SHOW CLUSTER/NODE (CSB), and
# SHOW CONNECTIONS/NODE (CDT connectivity state / Remote Con.ID) on the coordinator.
set -u
POD=$1; TAG=$2; STORES=$3; IDS=$4; STOREF=$5; IDF=$6
NS=ovmx-lab; NODE=2
L=/lab/k8s-labs/$POD/logs; HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
DAEMON=${DAEMON:?set DAEMON to the current-source SCSD.EXE}
J1=${J1:-30}; GAP=${GAP:-8}; J2=${J2:-70}; CAD=${CAD:-4}
S=$W/$TAG.status; : > "$S"; C=$W/$TAG.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax$NODE.log.in" >/dev/null 2>&1; sleep "${2:-2}"; }
shot(){ local cmd=$1 mark=$2 start; start=$(wc -c < "$HOSTL/vax$NODE.log");
  say "$cmd" "$CAD";
  { printf '\n########## %s ##########\n' "$mark";
    tail -c +$start "$HOSTL/vax$NODE.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; } >> "$C"; }

bash /data/training/vax/cluster/tools/lab2login.sh "$POD" "$NODE" | grep -q LOGGED-IN \
  || { echo "coord358: FATAL -- $POD vax$NODE not at DCL" >&2; exit 2; }
log "RUN $TAG pod=$POD coordinator=vax$NODE idS=$IDS idF=$IDF J1=$J1 GAP=$GAP J2=$J2"
log "daemon=$DAEMON md5=$(md5sum "$DAEMON" | cut -d' ' -f1)"

kubectl -n $NS cp "$DAEMON" "$POD:/lab/SCSD.EXE" >/dev/null 2>&1
LIBDIR=$(dirname "$(dirname "$DAEMON")")/lib
for lib in 'LIBVMSFS$SHR.EXE' 'LIBVMSLNM$SHR.EXE'; do
  [ -r "$LIBDIR/$lib" ] && kubectl -n $NS cp "$LIBDIR/$lib" "$POD:/lab/$lib" >/dev/null 2>&1
done
kubectl -n $NS cp "$STORES" "$POD:/lab/$TAG-S.sysgen" >/dev/null 2>&1
kubectl -n $NS cp "$STOREF" "$POD:/lab/$TAG-F.sysgen" >/dev/null 2>&1
kx rm -f "/lab/$TAG-S.sysgen.cluster" "/lab/$TAG-F.sysgen.cluster" 2>/dev/null
kx chmod +x /lab/SCSD.EXE

kubectl -n $NS exec "$POD" -- timeout $((J1+GAP+J2+J2+90)) \
    tcpdump -i br0 -w "$L/d94-$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2

say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'PRE FULL'

run_daemon(){  # <store> <duration> <logtag>
  kubectl -n $NS exec "$POD" -- sh -c \
    "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/$1 OVMX_JOIN_SEQ=1 ./SCSD.EXE --connect --duration $2 --iface br0 > $L/scsd-$TAG-$3.log 2>&1" &
}

# ---- A: FRESH join id_S ----
run_daemon "$TAG-S.sysgen" $((J1+5)) A
log "A: FRESH join $IDS started"
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$J1" ]; do shot "SHOW CLUSTER/NODE=$IDS" "A J1+$(( $(date +%s)-T0 ))s"; done
XA=$(kx grep -ac XITDONE $L/scsd-$TAG-A.log 2>/dev/null)
SIDE=$(kx sh -c "test -r /lab/$TAG-S.sysgen.cluster && echo yes || echo no")
log "A XITDONE=$XA sidecar=$SIDE"

# ---- CRASH id_S ----
kx pkill -9 -f SCSD.EXE 2>/dev/null
log "=== CRASH id_S (SIGKILL); dead ${GAP}s (< RECNXINTERVAL 20) ==="
shot "SHOW CLUSTER/NODE=$IDS" 'CRASH+0 S'
sleep "$GAP"

# ---- B: FAST return SAME id_S ----
run_daemon "$TAG-S.sysgen" $J2 B
log "B: FAST return SAME id $IDS -- sampling coordinator"
T0=$(date +%s); n=0
while [ $(( $(date +%s) - T0 )) -lt "$J2" ]; do
  t=$(( $(date +%s) - T0 ))
  case $(( n % 3 )) in
    0) shot "SHOW CLUSTER/NODE=$IDS" "B R+${t}s CSB";;
    1) shot "SHOW CONNECTIONS/NODE=$IDS" "B R+${t}s CDT";;
    2) shot 'SHOW CLUSTER' "B R+${t}s VOTES";;
  esac; n=$((n+1))
done
XB=$(kx grep -ac XITDONE $L/scsd-$TAG-B.log 2>/dev/null)
log "B (return SAME id) XITDONE=$XB"
kx pkill -9 -f SCSD.EXE 2>/dev/null
sleep 3

# ---- C: return presenting FRESH id_F (never held by the coordinator) ----
run_daemon "$TAG-F.sysgen" $J2 C
log "C: return presenting FRESH id $IDF -- sampling coordinator"
T0=$(date +%s); n=0
while [ $(( $(date +%s) - T0 )) -lt "$J2" ]; do
  t=$(( $(date +%s) - T0 ))
  case $(( n % 3 )) in
    0) shot "SHOW CLUSTER/NODE=$IDF" "C R+${t}s CSB";;
    1) shot "SHOW CONNECTIONS/NODE=$IDF" "C R+${t}s CDT";;
    2) shot 'SHOW CLUSTER' "C R+${t}s VOTES";;
  esac; n=$((n+1))
done
XC=$(kx grep -ac XITDONE $L/scsd-$TAG-C.log 2>/dev/null)
log "C (fresh id) XITDONE=$XC"
shot 'SHOW CLUSTER' 'END FULL'
say 'EXIT' 4
kx pkill -9 -f SCSD.EXE 2>/dev/null
wait $TCPD 2>/dev/null
log "SUMMARY  A(freshS)=$XA  B(returnS)=$XB  C(freshF)=$XC"
echo "===$TAG-DONE===" >> "$S"
