#!/bin/bash
# o32run.sh -- spec 4(O.32) decisive isolation (vms-694 rejoin long pole).
# ONE single-factor bracket on a VIRGIN lab-2 pod, current-HEAD SCS daemon
# (md5 d782c694 == origin/main src/vmsscs HEAD, commit 2d82d3f8 / 4(O.31)).
#
# THE question: on a CLEAN-LEAVE RETURN of identity OVXAB0, does a real VAX
# MEMBER ISSUE its own member-driven VMS$VAXcluster CONNECT-REQ toward the
# returner, and does that CDT reach State OPEN -- read from the MEMBER's SDA
# SHOW CONNECTIONS/NODE=OVXAB0 (the CDT/SYSAP layer, NOT SHOW CLUSTER/NODE which
# only shows the CSB) on BOTH members, cross-checked with tcpdump on br0.
#
# Arms differ ONLY in first-join vs return (same pod, daemon, identity, sysgen):
#   F1 = fresh first-join (no sidecar) -> clean-leave depart on --duration exit
#   SETTLE (> RECNXINTERVAL: prior CSB fully removed)
#   J  = return the SAME identity (sidecar present -> op 0x02 REJOIN form)
set -u
POD=vaxlab-2
NS=ovmx-lab
IDENT=OVXAB0
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
J1=${J1:-30}; SETTLE=${SETTLE:-35}; J2=${J2:-50}; CAD=${CAD:-4}
S=$W/o32.status; : > "$S"
C1=$W/o32.vax1.conn; : > "$C1"
C2=$W/o32.vax2.conn; : > "$C2"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
sayN(){ local n=$1 cmd=$2 s=${3:-2}; kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$cmd' > $L/vax$n.log.in" >/dev/null 2>&1; sleep "$s"; }
shotN(){ # node cmd mark
  local n=$1 cmd=$2 mark=$3 start CF; CF=$([ "$n" = 1 ] && echo "$C1" || echo "$C2")
  start=$(wc -c < "$HOSTL/vax$n.log")
  sayN "$n" "$cmd" "$CAD"
  { printf '\n########## vax%s %s ##########\n' "$n" "$mark"
    tail -c +$start "$HOSTL/vax$n.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
  } >> "$CF"
}

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
# fresh sidecar-less store already staged as /lab/o32.sysgen
kx rm -f /lab/o32.sysgen.cluster >/dev/null 2>&1 || true

log "RUN o32 pod=$POD ident=$IDENT daemon=/lab/SCSD.EXE(HEAD d782c694) J1=$J1 SETTLE=$SETTLE J2=$J2 cad=$CAD"

# tcpdump for the whole run
kx timeout $((J1+SETTLE+J2+70)) tcpdump -i br0 -w "$L/o32.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2

# park BOTH members in SDA
for n in 1 2; do sayN "$n" 'ANALYZE/SYSTEM' 6; sayN "$n" 'SET OUTPUT SYS$OUTPUT' 2; done
log "both members parked in SDA"
shotN 1 "SHOW CONNECTIONS/NODE=$IDENT" 'PRE'
shotN 2 "SHOW CONNECTIONS/NODE=$IDENT" 'PRE'

# ---- F1: fresh first-join ----
kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/o32.sysgen ./SCSD.EXE --connect --duration $J1 --iface br0 > $L/scsd-o32-F1.log 2>&1" &
log "F1 fresh first-join started (dur $J1, clean-leave on exit)"
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$J1" ]; do
  t=$(( $(date +%s) - T0 ))
  shotN 2 "SHOW CONNECTIONS/NODE=$IDENT" "F1+${t}s"
  shotN 1 "SHOW CONNECTIONS/NODE=$IDENT" "F1+${t}s"
done
kx pkill -f SCSD.EXE >/dev/null 2>&1 || true
X1=$(kx grep -ac XITDONE $L/scsd-o32-F1.log 2>/dev/null | tr -d '\r')
ADM1=$(kx grep -aoE 'admitted=[0-9]+' $L/scsd-o32-F1.log 2>/dev/null | tail -1 | tr -d '\r')
SIDE=$(kx sh -c 'test -r /lab/o32.sysgen.cluster && echo yes || echo no' | tr -d '\r')
log "F1 done: XITDONE_count=$X1 $ADM1 sidecar=$SIDE"

# ---- SETTLE (prior CSB removed) ----
log "=== SETTLE ${SETTLE}s (clean-leave; coordinator removes $IDENT) ==="
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$SETTLE" ]; do
  t=$(( $(date +%s) - T0 ))
  shotN 2 "SHOW CLUSTER/NODE=$IDENT" "SETTLE+${t}s"
  sleep 3
done
shotN 1 "SHOW CONNECTIONS/NODE=$IDENT" 'SETTLE-END'
shotN 2 "SHOW CONNECTIONS/NODE=$IDENT" 'SETTLE-END'

# ---- J: return the same identity (sidecar present) ----
kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/o32.sysgen ./SCSD.EXE --connect --duration $J2 --iface br0 > $L/scsd-o32-J.log 2>&1" &
log "J RETURN started (dur $J2) -- sampling BOTH members' CONNECTIONS densely"
T0=$(date +%s); nn=0
while [ $(( $(date +%s) - T0 )) -lt "$J2" ]; do
  t=$(( $(date +%s) - T0 ))
  shotN 2 "SHOW CONNECTIONS/NODE=$IDENT" "J+${t}s"
  shotN 1 "SHOW CONNECTIONS/NODE=$IDENT" "J+${t}s"
  if [ $(( nn % 2 )) -eq 1 ]; then shotN 2 "SHOW CLUSTER/NODE=$IDENT" "J+${t}s CSB"; fi
  nn=$((nn+1))
done
shotN 2 'SHOW CLUSTER' 'J-END FULL'
shotN 1 'SHOW CLUSTER' 'J-END FULL'
shotN 2 "SHOW CONNECTIONS/NODE=$IDENT" 'J-END'
shotN 1 "SHOW CONNECTIONS/NODE=$IDENT" 'J-END'
for n in 1 2; do sayN "$n" 'EXIT' 3; done

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
wait $TCPD 2>/dev/null || true

X2=$(kx grep -ac XITDONE $L/scsd-o32-J.log 2>/dev/null | tr -d '\r')
ADM2=$(kx grep -aoE 'admitted=[0-9]+|OBS-0-CM-RESP|OBS-OP02-DRIVEN-0-CM-RESP|OBS-LATCH-0-CM-RESP|READMITMAP-SUMMARY.*' $L/scsd-o32-J.log 2>/dev/null | tr -d '\r' | tr '\n' '|')
log "J done: XITDONE_count=$X2  verdict=[$ADM2]"
log "wire identities: $(strings -a "$HOSTL/o32.pcap" 2>/dev/null | grep -oE 'OVX[A-Z0-9]{3}' | sort -u | tr '\n' ' ')"
echo "===o32-DONE===" >> "$S"
log "wrote $S $C1 $C2 ; pcap=$HOSTL/o32.pcap"
