#!/bin/bash
# cq358.sh <pod>  (vms-358, spec 4(O.28)) -- CLEAN vs UNCLEAN departure gate.
#
# One pristine boot, coordinator = VAX2 parked in ANALYZE/SYSTEM. Four fresh
# first-joins, NONE a return; the ONLY factor that varies is how the PRIOR
# daemon departed and what CLUB quorum state it left:
#
#   A  fresh join id_A -> GRACEFUL exit (--duration; SCS DISCONNECT, no kill)
#   B  fresh join id_B  (after a CLEAN departure)      -> proposed?
#   C  fresh join id_C -> SIGKILL crash (bare VC break)
#   D  fresh join id_D  (after an UNCLEAN failure)      -> proposed?
#
# Primary observable: the coordinator CLUB `SHOW CLUSTER` Flags (qf_failed_node)
# + Figure of Merit, sampled after each departure. Admission truth = the
# coordinator console line `proposing addition of system <id>` (NOT XITDONE,
# which the verbose READMITMAP verdict text falsely inflates).
set -u
POD=$1; NS=ovmx-lab; NODE=2
L=/lab/k8s-labs/$POD/logs; HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
DAEMON=${DAEMON:-$W/SCSD-358.EXE}
CAD=${CAD:-4}
S=$W/cq358.status; : > "$S"; C=$W/cq358.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax$NODE.log.in" >/dev/null 2>&1; sleep "${2:-2}"; }
shot(){ local cmd=$1 mark=$2 start; start=$(wc -c < "$HOSTL/vax$NODE.log");
  say "$cmd" "$CAD";
  { printf '\n########## %s ##########\n' "$mark";
    tail -c +$start "$HOSTL/vax$NODE.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; } >> "$C"; }
# admission truth: was <id> proposed on the coordinator console during this arm?
proposed(){ kx sh -c "cat /lab/k8s-labs/$POD/logs/vax$NODE.log" 2>/dev/null \
  | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' | grep -qa "proposing addition of system $1" && echo YES || echo NO; }

bash /data/training/vax/cluster/tools/lab2login.sh "$POD" "$NODE" | grep -q LOGGED-IN \
  || { echo "cq358: FATAL -- $POD vax$NODE not at DCL" >&2; exit 2; }
log "RUN cq358 pod=$POD coordinator=vax$NODE daemon=$DAEMON md5=$(md5sum "$DAEMON"|cut -d' ' -f1)"

kubectl -n $NS cp "$DAEMON" "$POD:/lab/SCSD.EXE" >/dev/null 2>&1
for lib in 'LIBVMSFS$SHR.EXE' 'LIBVMSLNM$SHR.EXE'; do
  [ -r "$W/$lib" ] && kubectl -n $NS cp "$W/$lib" "$POD:/lab/$lib" >/dev/null 2>&1
done
for t in A B C D; do kubectl -n $NS cp "$W/sysgen-358$t.dat" "$POD:/lab/cq-$t.sysgen" >/dev/null 2>&1; kx rm -f "/lab/cq-$t.sysgen.cluster" 2>/dev/null; done
kx chmod +x /lab/SCSD.EXE

kubectl -n $NS exec "$POD" -- timeout 900 \
    tcpdump -i br0 -w "$L/d94-cq358.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2
say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'PRE FULL (pristine)'

arm(){  # <tag> <store> <ident> <duration> <exit: grace|kill> <settle>
  local tag=$1 store=$2 id=$3 dur=$4 mode=$5 settle=$6
  kubectl -n $NS exec "$POD" -- sh -c \
    "cd /lab && LD_LIBRARY_PATH=/lab OVMX_SYSGEN_PATH=/lab/cq-$tag.sysgen OVMX_JOIN_SEQ=1 ./SCSD.EXE --connect --duration $dur --iface br0 > $L/scsd-cq$tag.log 2>&1" &
  log "$tag: fresh join $id started (dur=$dur exit=$mode)"
  local T0=$(date +%s)
  while [ $(( $(date +%s) - T0 )) -lt "$dur" ]; do shot "SHOW CLUSTER/NODE=$id" "$tag J+$(( $(date +%s)-T0 ))s"; done
  local prop=$(proposed "$id")
  local adm=$(kx grep -aoE 'READMITMAP-SUMMARY.*admitted=[0-9]+' $L/scsd-cq$tag.log 2>/dev/null | grep -oE 'admitted=[0-9]+' | head -1)
  log "$tag: PROPOSED=$prop  ($adm)  first-join reference"
  if [ "$mode" = kill ]; then kx pkill -9 -f SCSD.EXE 2>/dev/null; log "$tag: === SIGKILL crash (bare VC break) ==="
  else log "$tag: === graceful --duration exit (SCS DISCONNECT) ==="; fi
  # sample CLUB across the settle window
  local T0=$(date +%s)
  while [ $(( $(date +%s) - T0 )) -lt "$settle" ]; do shot 'SHOW CLUSTER' "$tag SETTLE+$(( $(date +%s)-T0 ))s CLUB"; done
  kx pkill -9 -f SCSD.EXE 2>/dev/null; sleep 2
}

arm A sysgen-358A.dat OVX3A0 25 grace 16
arm B sysgen-358B.dat OVX3B0 30 grace 12
arm C sysgen-358C.dat OVX3C0 25 kill  26
arm D sysgen-358F.dat OVX3F0 40 grace 10

say 'EXIT' 4
kx pkill -9 -f SCSD.EXE 2>/dev/null
wait $TCPD 2>/dev/null
log "=== cq358 done ==="
echo "===cq358-DONE===" >> "$S"
