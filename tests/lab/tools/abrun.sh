#!/bin/bash
# abrun.sh <pod>  (vms-ab1, spec 4(O.29)) -- THE REJOIN THREAD-CLOSER BRACKET.
#
# One identity (OVXAB0/1981). cleanleave DEFAULT (the fix) unless MODE=ctrl.
#
#   F1  fresh first-join (no sidecar) -> admit -> GRACEFUL depart (--duration
#       expiry -> SIGALRM -> scsd_shutdown_teardown -> clean SCS DISCONNECT
#       [+ last-gasp when built]).  NOT SIGKILL: this is a CLEAN leave.
#   SETTLE (no daemon): sample coordinator SDA and watch the departed node's CSB
#       get REMOVED -- no wedged State 03/09 long_break survivor.
#   J   return the SAME id as a FRESH FIRST-JOIN (cleanleave default; the sidecar
#       from F1 is present but no longer forces the rejoin form) -> expect ADMIT,
#       XITDONE 0->1, coordinator 'proposing addition of system OVXAB0' + member.
#
# MODE=ctrl runs J with OVMX_REJOIN_CLEANLEAVE=0 (legacy rejoin-form return) to
# reproduce the SS 4(O.28) wedge as the fail-pre control.
#
# Admission truth = coordinator console ('proposing addition' + 'member') AND the
# daemon's own SCSD-I-XITDONE / READMITMAP-SUMMARY admitted= (authoritative).
set -u
POD=$1; NS=ovmx-lab; NODE=2; ID=${ID:-OVXAB0}
MODE=${MODE:-fix}
STORE=${STORE:-/data/training/vax/cluster/work/sysgen-OVXAB0.dat}
L=/lab/k8s-labs/$POD/logs; HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
DAEMON=${DAEMON:?set DAEMON to the current-source SCSD.EXE}
J1=${J1:-30}; SETTLE=${SETTLE:-32}; J2=${J2:-45}; CAD=${CAD:-4}
S=$W/abrun-$MODE.status; : > "$S"; C=$W/abrun-$MODE.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax$NODE.log.in" >/dev/null 2>&1; sleep "${2:-2}"; }
shot(){ local cmd=$1 mark=$2 start; start=$(wc -c < "$HOSTL/vax$NODE.log");
  say "$cmd" "$CAD";
  { printf '\n########## %s ##########\n' "$mark";
    tail -c +$start "$HOSTL/vax$NODE.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; } >> "$C"; }
# OVMX picks its OWN op-02 coordinator by highest-node rule, which may be EITHER
# real node -- so scan BOTH consoles for the admission signal, not just $NODE.
member_seen(){ kx sh -c "cat $L/vax1.log $L/vax2.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
  | grep -aE "proposing addition of system $ID|$ID +[0-9A-F]+ +[0-9] +.*member" | tail -3; }
prop_count(){ kx sh -c "cat $L/vax1.log $L/vax2.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -ac "proposing addition of system $ID"; }

bash /data/training/vax/cluster/tools/lab2login.sh "$POD" "$NODE" | grep -q LOGGED-IN \
  || { echo "abrun: FATAL -- $POD vax$NODE not at DCL" >&2; exit 2; }
log "RUN abrun MODE=$MODE pod=$POD coord=vax$NODE id=$ID J1=$J1 SETTLE=$SETTLE J2=$J2"
log "daemon=$DAEMON md5=$(md5sum "$DAEMON" | cut -d' ' -f1)"

kubectl -n $NS cp "$DAEMON" "$POD:/lab/SCSD.EXE" >/dev/null 2>&1
LIBDIR=$(dirname "$(dirname "$DAEMON")")/lib
for lib in 'LIBVMSFS$SHR.EXE' 'LIBVMSLNM$SHR.EXE'; do
  [ -r "$W/$lib" ] && kubectl -n $NS cp "$W/$lib" "$POD:/lab/$lib" >/dev/null 2>&1
  [ -r "$LIBDIR/$lib" ] && kubectl -n $NS cp "$LIBDIR/$lib" "$POD:/lab/$lib" >/dev/null 2>&1
done
kubectl -n $NS cp "$STORE" "$POD:/lab/ab.sysgen" >/dev/null 2>&1
kx rm -f /lab/ab.sysgen.cluster 2>/dev/null   # F1 is a FRESH first-join
kx chmod +x /lab/SCSD.EXE
POD_MD5=$(kx md5sum /lab/SCSD.EXE 2>/dev/null | awk '{print $1}')
log "staged in-pod SCSD.EXE md5=$POD_MD5"

kubectl -n $NS exec "$POD" -- timeout 400 tcpdump -i br0 -w "$L/d94-abrun-$MODE.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2
say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'PRE'

# daemon runs cleanleave DEFAULT (fix) unless the caller passes env; F1 always fix.
daemon(){ local dur=$1 tag=$2 extra=${3:-}
  kubectl -n $NS exec "$POD" -- sh -c \
  "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/ab.sysgen OVMX_JOIN_SEQ=1 $extra ./SCSD.EXE --connect --duration $dur --iface br0 > $L/scsd-ab-$tag.log 2>&1" & }
sample(){ local dur=$1 tag=$2; local T0=$(date +%s);
  while [ $(( $(date +%s) - T0 )) -lt "$dur" ]; do shot "SHOW CLUSTER/NODE=$ID" "$tag +$(( $(date +%s)-T0 ))s"; done; }

# --- F1: fresh first-join, then GRACEFUL depart at --duration expiry ---
log "F1 fresh first-join $ID (cleanleave default), graceful depart at ${J1}s"
daemon "$J1" F1
# sample through the run; the daemon exits GRACEFULLY on its own at J1 (no pkill)
sample $((J1+4)) F1
log "F1 member? -> $(member_seen | tail -1)"
XF1=$(kx grep -aoE 'SCSD-I-XITDONE|READMITMAP-SUMMARY.*admitted=[0-9]+' $L/scsd-ab-F1.log 2>/dev/null | tail -2 | tr '\n' ' ')
log "F1 daemon: [$XF1]"
kx pkill -9 -f SCSD.EXE 2>/dev/null; sleep 2   # ensure gone before settle

# --- SETTLE: no daemon; watch the coordinator REMOVE the CSB (no long_break wedge) ---
log "=== SETTLE ${SETTLE}s (no daemon; coordinator removes $ID) ==="
sample "$SETTLE" SETTLE
shot "SHOW CLUSTER/NODE=$ID" 'SETTLE-END CSB'

# --- J: return SAME id as a FRESH FIRST-JOIN (sidecar present) ---
PBEFORE=$(prop_count)
JEXTRA=""
[ "$MODE" = ctrl ] && JEXTRA="OVMX_REJOIN_CLEANLEAVE=0"
log "J return $ID mode=$MODE extra=[$JEXTRA] proposing_before=$PBEFORE"
daemon "$J2" J "$JEXTRA"
sample "$J2" J
PAFTER=$(prop_count)
XJ=$(kx grep -aoE 'SCSD-I-XITDONE|READMITMAP-SUMMARY.*admitted=[0-9]+|XITDONE' $L/scsd-ab-J.log 2>/dev/null | tail -4 | tr '\n' ' ')
log "J member? -> $(member_seen | tail -1)"
log "J daemon: [$XJ]"
log "J proposing_before=$PBEFORE proposing_after=$PAFTER (delta=$((PAFTER-PBEFORE)))"
shot "SHOW CLUSTER/NODE=$ID" 'J-END CSB'
shot 'SHOW CLUSTER' 'J-END FULL'
say 'EXIT' 4
kx pkill -9 -f SCSD.EXE 2>/dev/null
wait $TCPD 2>/dev/null
log "SUMMARY MODE=$MODE F1=[$XF1] J=[$XJ] proposing_delta=+$((PAFTER-PBEFORE))"
echo "===abrun-$MODE-DONE===" >> "$S"
