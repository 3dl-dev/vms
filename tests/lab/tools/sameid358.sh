#!/bin/bash
# sameid358.sh <pod>  (vms-358, spec 4(O.28)) -- can the SAME id rejoin?
# Single factor = how the return is presented, on ONE id (OVX3X0/1980):
#   F1  first-join id_X (no sidecar)            -> admit -> SIGKILL crash
#   R   FAST return id_X, sidecar present (REJOIN form) -> expect REFUSED (O.27)
#   (kill R; LONG clean settle so the coordinator fully removes id_X)
#   J   return id_X as FRESH FIRST-JOIN (sidecar DELETED) after settle -> admit?
# Admission truth = coordinator CSB reaches `member` + console `proposing
# addition of system OVX3X0` (grepped from the full console, post-arm).
set -u
POD=$1; NS=ovmx-lab; NODE=2; ID=OVX3X0; STORE=/data/training/vax/cluster/work/sysgen-358X.dat
L=/lab/k8s-labs/$POD/logs; HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work; DAEMON=${DAEMON:-$W/SCSD-358.EXE}
CAD=${CAD:-4}
S=$W/sameid358.status; : > "$S"; C=$W/sameid358.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax$NODE.log.in" >/dev/null 2>&1; sleep "${2:-2}"; }
shot(){ local cmd=$1 mark=$2 start; start=$(wc -c < "$HOSTL/vax$NODE.log");
  say "$cmd" "$CAD";
  { printf '\n########## %s ##########\n' "$mark";
    tail -c +$start "$HOSTL/vax$NODE.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; } >> "$C"; }
member_seen(){ kx sh -c "cat $L/vax$NODE.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
  | grep -aE "proposing addition of system $ID|$ID +[0-9A-F]+ +[0-9] +.*member" | tail -3; }

bash /data/training/vax/cluster/tools/lab2login.sh "$POD" "$NODE" | grep -q LOGGED-IN \
  || { echo "sameid358: FATAL -- $POD vax$NODE not at DCL" >&2; exit 2; }
log "RUN sameid358 pod=$POD coord=vax$NODE id=$ID daemon=$DAEMON md5=$(md5sum "$DAEMON"|cut -d' ' -f1)"

kubectl -n $NS cp "$DAEMON" "$POD:/lab/SCSD.EXE" >/dev/null 2>&1
for lib in 'LIBVMSFS$SHR.EXE' 'LIBVMSLNM$SHR.EXE'; do
  [ -r "$W/$lib" ] && kubectl -n $NS cp "$W/$lib" "$POD:/lab/$lib" >/dev/null 2>&1
done
kubectl -n $NS cp "$STORE" "$POD:/lab/sid.sysgen" >/dev/null 2>&1
kx rm -f /lab/sid.sysgen.cluster 2>/dev/null   # start FRESH
kx chmod +x /lab/SCSD.EXE
kubectl -n $NS exec "$POD" -- timeout 500 tcpdump -i br0 -w "$L/d94-sameid.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2
say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'PRE'

daemon(){ kubectl -n $NS exec "$POD" -- sh -c \
  "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/sid.sysgen OVMX_JOIN_SEQ=1 ./SCSD.EXE --connect --duration $1 --iface br0 > $L/scsd-sid$2.log 2>&1" & }
sample(){ local dur=$1 tag=$2; local T0=$(date +%s);
  while [ $(( $(date +%s) - T0 )) -lt "$dur" ]; do shot "SHOW CLUSTER/NODE=$ID" "$tag +$(( $(date +%s)-T0 ))s"; done; }

# F1: fresh first-join -> crash
daemon 25 F1; log "F1 first-join $ID"; sample 25 F1
log "F1 member? -> $(member_seen | tail -1)"
kx pkill -9 -f SCSD.EXE 2>/dev/null; log "F1 SIGKILL crash"; sleep 6

# R: FAST return, sidecar present (REJOIN form)
SIDE=$(kx sh -c "test -r /lab/sid.sysgen.cluster && echo yes || echo no")
log "R sidecar=$SIDE (rejoin form) -- FAST return $ID"
daemon 45 R; sample 45 R
XR=$(kx grep -aoE 'READMITMAP-SUMMARY.*admitted=[0-9]+' $L/scsd-sidR.log 2>/dev/null | grep -oE 'admitted=[0-9]+' | head -1)
PR=$(kx sh -c "cat $L/vax$NODE.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -ac "proposing addition of system $ID")
log "R (rejoin-form fast return): proposing_count=$PR readmit=[$XR]"
kx pkill -9 -f SCSD.EXE 2>/dev/null

# LONG clean settle: coordinator removes id_X, no daemon hammering
log "=== LONG SETTLE 35s (clean removal of $ID, no daemon) ==="
T0=$(date +%s); while [ $(( $(date +%s) - T0 )) -lt 35 ]; do shot "SHOW CLUSTER/NODE=$ID" "SETTLE +$(( $(date +%s)-T0 ))s"; done

# J: return id_X as FRESH FIRST-JOIN (sidecar deleted) after settle
kx rm -f /lab/sid.sysgen.cluster 2>/dev/null
log "J sidecar DELETED (first-join form) -- return $ID as fresh JOIN after settle"
PBEFORE=$(kx sh -c "cat $L/vax$NODE.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -ac "proposing addition of system $ID")
daemon 45 J; sample 45 J
XJ=$(kx grep -aoE 'READMITMAP-SUMMARY.*admitted=[0-9]+|XITDONE' $L/scsd-sidJ.log 2>/dev/null | head -3)
PAFTER=$(kx sh -c "cat $L/vax$NODE.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -ac "proposing addition of system $ID")
log "J member? -> $(member_seen | tail -1)"
log "J (first-join-after-settle): proposing_before=$PBEFORE proposing_after=$PAFTER  daemon=[$XJ]"
shot "SHOW CLUSTER/NODE=$ID" 'J-END CSB'
shot 'SHOW CLUSTER' 'J-END FULL'
say 'EXIT' 4
kx pkill -9 -f SCSD.EXE 2>/dev/null
wait $TCPD 2>/dev/null
log "SUMMARY R(rejoin-fast)=proposing+$((PR)) J(firstjoin-settle)=proposing_delta+$((PAFTER-PBEFORE))"
echo "===sameid358-DONE===" >> "$S"
