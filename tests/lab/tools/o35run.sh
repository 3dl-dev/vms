#!/bin/bash
# o35run.sh -- spec 4(O.35): is a DETERMINISTIC return-side reclaim-engagement
# fix OVMX-forceable? Reads the ORDERING of the coordinating member's CSB
# reclaim (BRK_NON -> MEMBER, CLUSTER_NODES 2->3) against OVMX's op-emission
# timeline (scsd log, wall-clock stamped) on a single-factor first-join-vs-return
# bracket. Virgin lab-2 pod. Oracle = each member's F$GETSYI("CLUSTER_NODES")
# and DCL SHOW CLUSTER (NOT OVMX's derived verdicts, per 4(O.33)/#346).
#
# Params: DAEMON=<host SCSD.EXE>  TAG=<label>  IDENT store fixed OVMXE0/1815.
set -u
POD=${POD:-vaxlab-2}; NS=ovmx-lab
IDENT=OVMXE0
STORE=${STORE:-/data/training/vax/cluster/work/OVMXE0.sysgen}
DAEMON=${DAEMON:?set DAEMON to a host SCSD.EXE}
TAG=${TAG:-o35}
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
J1=${J1:-35}; SETTLE=${SETTLE:-40}; J2=${J2:-75}; CAD=${CAD:-3}
LIBDIR=$(dirname "$DAEMON")
S=$W/$TAG.status; : > "$S"
TL=$W/$TAG.timeline; : > "$TL"
log(){ echo "[$(date +%T.%3N)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
sayN(){ local n=$1 cmd=$2 s=${3:-2} b; b=$(printf '%s\r' "$cmd" | base64 -w0);
  kubectl -n $NS exec "$POD" -- sh -c "echo $b | base64 -d > $L/vax$n.log.in" >/dev/null 2>&1; sleep "$s"; }
cleanN(){ tr -d '\000' < "$HOSTL/vax$1.log" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }
# dense membership sample: CLUSTER_NODES oracle + the IDENT row's STATUS, both members, wall-clock stamped
sample(){ local mark=$1 n cn st
  for n in 1 2; do
    local start; start=$(wc -c < "$HOSTL/vax$n.log")
    sayN "$n" 'WRITE SYS$OUTPUT "CN'$n'_"+F$STRING(F$GETSYI("CLUSTER_NODES"))' 1
    sayN "$n" 'SHOW CLUSTER' 1
    local delta; delta=$(tail -c +$start "$HOSTL/vax$n.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g')
    cn=$(echo "$delta" | grep -aoE "CN${n}_[0-9]+" | tail -1)
    st=$(echo "$delta" | grep -aE "$IDENT" | grep -aoiE 'MEMBER|BRK_[A-Z]+|NEW|removed' | head -1)
    [ -z "$st" ] && st="absent"
    echo "[$(date +%T.%3N)] $mark vax$n ${cn:-CN?} ident=$st" | tee -a "$TL"
  done
}

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
# stage daemon + libs + store
kubectl -n $NS cp "$DAEMON" "$POD:/lab/$TAG-SCSD.EXE" >/dev/null 2>&1
for lib in 'LIBVMSFS$SHR.EXE' 'LIBVMSLNM$SHR.EXE'; do
  [ -r "$W/$lib" ] && kubectl -n $NS cp "$W/$lib" "$POD:/lab/$lib" >/dev/null 2>&1
done
kubectl -n $NS cp "$STORE" "$POD:/lab/$TAG.sysgen" >/dev/null 2>&1
kx rm -f /lab/$TAG.sysgen.cluster >/dev/null 2>&1 || true   # F1 = true first-join
kx chmod +x /lab/$TAG-SCSD.EXE
POD_MD5=$(kx md5sum /lab/$TAG-SCSD.EXE 2>/dev/null | awk '{print $1}')
log "RUN $TAG pod=$POD ident=$IDENT daemon=$DAEMON md5=$(md5sum "$DAEMON"|cut -c1-12) pod_md5=$(echo $POD_MD5|cut -c1-12)"

for n in 1 2; do sayN "$n" 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST' 1; sayN "$n" 'REPLY/ENABLE=(CLUSTER,CONFIG)' 1; done

kx timeout $((J1+SETTLE+J2+80)) tcpdump -i br0 -w "$L/$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2

sample 'PRE'

# ---- F1: fresh first-join (Release/whatever DAEMON) ----
kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/$TAG.sysgen /lab/$TAG-SCSD.EXE --connect --duration $J1 --iface br0 > $L/scsd-$TAG-F1.log 2>&1" &
log "F1 fresh first-join started (dur $J1)"
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$J1" ]; do sample "F1+$(( $(date +%s)-T0 ))s"; sleep 1; done
kx pkill -f SCSD.EXE >/dev/null 2>&1 || true
X1=$(kx grep -ac XITDONE $L/scsd-$TAG-F1.log 2>/dev/null | tr -d '\r')
log "F1 done: XITDONE_count=$X1 sidecar=$(kx sh -c "test -r /lab/$TAG.sysgen.cluster && echo yes || echo no" | tr -d '\r')"

# ---- SETTLE: departed OVMX identity lingers BRK_NON on members (residual CSB) ----
log "=== SETTLE ${SETTLE}s ==="
T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$SETTLE" ]; do sample "SET+$(( $(date +%s)-T0 ))s"; sleep 3; done

# ---- J: return the SAME identity; dense sampling through the reclaim race ----
JT0=$(date +%s)
kx sh -c "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/$TAG.sysgen /lab/$TAG-SCSD.EXE --connect --duration $J2 --iface br0 > $L/scsd-$TAG-J.log 2>&1" &
log "J RETURN started (dur $J2) daemon-launch-wall=$(date +%T.%3N)"
while [ $(( $(date +%s) - JT0 )) -lt "$J2" ]; do sample "J+$(( $(date +%s)-JT0 ))s"; done

kx pkill -9 -f SCSD.EXE >/dev/null 2>&1 || true
kx pkill -9 -f tcpdump >/dev/null 2>&1 || true
wait $TCPD 2>/dev/null || true

X2=$(kx grep -ac XITDONE $L/scsd-$TAG-J.log 2>/dev/null | tr -d '\r')
ADM2=$(kx grep -aoE 'admitted=[0-9]+|READMITMAP.*' $L/scsd-$TAG-J.log 2>/dev/null | tail -2 | tr '\n' '|')
# final oracle read
sample 'J-END'
CNJ1=$(grep 'J-END vax1' "$TL" | tail -1); CNJ2=$(grep 'J-END vax2' "$TL" | tail -1)
log "J done: XITDONE_count=$X2 verdict=[$ADM2]"
log "FINAL ORACLE: $CNJ1 || $CNJ2"
log "wire idents: $(strings -a "$HOSTL/$TAG.pcap" 2>/dev/null | grep -oE 'OVX[A-Z0-9]{3}|OVMX[A-Z0-9]{2}' | sort -u | tr '\n' ' ')"
echo "===$TAG-DONE===" >> "$S"
log "wrote $S (status), $TL (timeline); pcap=$HOSTL/$TAG.pcap; daemon logs scsd-$TAG-F1/J.log"
