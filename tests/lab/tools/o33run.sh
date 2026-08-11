#!/bin/bash
# o33run.sh -- spec 4(O.33) decisive isolation (vms-694 rejoin long pole).
# Reads the COORDINATOR's CM DECISION STATE through the op-0x04 abort.
# Single-factor bracket on a VIRGIN lab-2 pod, current-HEAD SCS daemon
# (/lab/o33-SCSD.EXE, built from origin/main src/vmsscs HEAD 2d82d3f8).
#
# THE question (4(O.33)): the connection is OPEN and the addition is PROPOSED,
# yet the coordinator ABORTS the CM state transition (cat 0x01 op 0x04 role 0x50)
# for a RETURNING identity where a fresh FIRST-JOIN of the SAME identity commits.
# WHAT state does the coordinator hold about the returning SCSSYSTEMID at the
# "proposing addition" decision point that flips accept->abort?
#
# Arms differ ONLY in first-join vs return of the SAME identity OVXAC1/1986:
#   F1 = fresh first-join (no sidecar) -> clean-leave depart on --duration exit
#   SETTLE (> RECNXINTERVAL=20s: prior CSB removed by the 4(O.30) last-gasp)
#   J  = return the SAME identity (sidecar present) -> clean-leave first-join form
set -u
POD=vaxlab-2
NS=ovmx-lab
IDENT=OVXAC1
DAEMON=/lab/o33-SCSD.EXE
SYSGEN=/lab/o33.sysgen
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
J1=${J1:-35}; SETTLE=${SETTLE:-42}; J2=${J2:-60}; CAD=${CAD:-4}
S=$W/o33.status; : > "$S"
C1=$W/o33.vax1.conn; : > "$C1"
C2=$W/o33.vax2.conn; : > "$C2"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
# base64-safe console send (avoids $ / quote mangling through kubectl exec sh -c)
sayN(){ local n=$1 cmd=$2 s=${3:-2}; local b; b=$(printf '%s\r' "$cmd" | base64 -w0);
  kubectl -n $NS exec "$POD" -- sh -c "echo $b | base64 -d > $L/vax$n.log.in" >/dev/null 2>&1; sleep "$s"; }
shotN(){ # node cmd mark -> append console delta to per-node file
  local n=$1 cmd=$2 mark=$3 start CF; CF=$([ "$n" = 1 ] && echo "$C1" || echo "$C2")
  start=$(wc -c < "$HOSTL/vax$n.log")
  sayN "$n" "$cmd" "$CAD"
  { printf '\n########## vax%s %s ##########\n' "$n" "$mark"
    tail -c +$start "$HOSTL/vax$n.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
  } >> "$CF"
}
snapclu(){ # dense SHOW CLUSTER on both members with a mark
  shotN 1 'SHOW CLUSTER' "$1"; shotN 2 'SHOW CLUSTER' "$1"; }

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
kx rm -f ${SYSGEN}.cluster >/dev/null 2>&1 || true   # no stale sidecar: F1 is a true first-join

log "RUN o33 pod=$POD ident=$IDENT daemon=$DAEMON J1=$J1 SETTLE=$SETTLE J2=$J2 cad=$CAD"
log "daemon md5 in pod: $(kx md5sum $DAEMON 2>/dev/null | cut -c1-32)"

# enable OPCOM cluster/config messages so CNXMAN transition + abort reasons
# print to the console log, and quiet the pager
for n in 1 2; do
  sayN "$n" 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST' 1
  sayN "$n" 'REPLY/ENABLE=(CLUSTER,CONFIG,SECURITY)' 2
done
log "OPCOM cluster/config enabled on both members"

# whole-run capture
kx timeout $((J1+SETTLE+J2+80)) tcpdump -i br0 -w "$L/o33.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2

snapclu 'PRE'

# ---- F1: fresh first-join ----
kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=$SYSGEN $DAEMON --connect --duration $J1 --iface br0 > $L/scsd-o33-F1.log 2>&1" &
log "F1 fresh first-join started (dur $J1, clean-leave on exit)"
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$J1" ]; do
  t=$(( $(date +%s) - T0 ))
  snapclu "F1+${t}s"
done
kx pkill -f SCSD.EXE >/dev/null 2>&1 || true
X1=$(kx grep -ac XITDONE $L/scsd-o33-F1.log 2>/dev/null | tr -d '\r')
ADM1=$(kx grep -aoE 'admitted=[0-9]+' $L/scsd-o33-F1.log 2>/dev/null | tail -1 | tr -d '\r')
SIDE=$(kx sh -c "test -r ${SYSGEN}.cluster && echo yes || echo no" | tr -d '\r')
log "F1 done: XITDONE_count=$X1 $ADM1 sidecar=$SIDE"

# ---- SETTLE (prior CSB removed by 4(O.30) last-gasp; > RECNXINTERVAL) ----
log "=== SETTLE ${SETTLE}s (clean-leave; coordinator removes $IDENT) ==="
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$SETTLE" ]; do
  t=$(( $(date +%s) - T0 )); snapclu "SETTLE+${t}s"; sleep 3
done
snapclu 'SETTLE-END'

# ---- J: return the SAME identity (sidecar present) ----
kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=$SYSGEN $DAEMON --connect --duration $J2 --iface br0 > $L/scsd-o33-J.log 2>&1" &
log "J RETURN started (dur $J2) -- dense SHOW CLUSTER on both members through the abort"
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$J2" ]; do
  t=$(( $(date +%s) - T0 )); snapclu "J+${t}s"
done
# post-abort SDA read of the coordinator's residual view of the returner
for n in 1 2; do
  sayN "$n" 'ANALYZE/SYSTEM' 5
  shotN "$n" "SHOW CONNECTIONS/NODE=$IDENT" "J-POST SDA CONN"
  shotN "$n" 'SHOW CLUSTER' 'J-POST SDA CLU'
  sayN "$n" 'EXIT' 3
done

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
wait $TCPD 2>/dev/null || true

X2=$(kx grep -ac XITDONE $L/scsd-o33-J.log 2>/dev/null | tr -d '\r')
ADM2=$(kx grep -aoE 'admitted=[0-9]+|XITABORT|cm_abort_seen|NO-ENGAGE|READMITMAP-SUMMARY.*' $L/scsd-o33-J.log 2>/dev/null | tr -d '\r' | tr '\n' '|')
log "J done: XITDONE_count=$X2  verdict=[$ADM2]"
log "wire identities: $(strings -a "$HOSTL/o33.pcap" 2>/dev/null | grep -oE 'OVX[A-Z0-9]{3}' | sort -u | tr '\n' ' ')"
echo "===o33-DONE===" >> "$S"
log "wrote $S $C1 $C2 ; pcap=$HOSTL/o33.pcap ; daemon logs scsd-o33-F1/J.log"
