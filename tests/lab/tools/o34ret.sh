#!/bin/bash
# o34ret.sh -- spec 4(O.34) POSITIVE half: isolate the AUTHENTIC CSB
# deallocation trigger. o34run.sh proved a clean leave does NOT deallocate the
# survivor's CSB (it lingers long_break,removed, same address/CSID, >220s, like
# OVMX / Davis p.7-25). Per Davis p.7-25 the old CSB is deallocated + rebuilt at
# the RETURN. This reboots the departed REAL VAX and watches the survivor:
#   - old CSB (same address, same CSID) DEALLOCATED
#   - VAX2 readmitted with a FRESH CSID (seq incremented) at a new CSV entry
#   - Index of next CSID advances
# demonstrating the trigger is the REJOIN, not the leave -- and that a REAL VAX
# does the reclaim deterministically. Same-identity return (authentic).
set -u
POD=${POD:-vaxlab-2}
NS=ovmx-lab
SURV=1; DEP=2
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
LAB=/lab/k8s-labs/$POD
CAD=${CAD:-6}; WATCH=${WATCH:-200}
TAG=${TAG:-o34ret}
S=$W/$TAG.status; : > "$S"; C=$W/$TAG.csb; : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
sayN(){ local n=$1 cmd=$2 s=${3:-2} b; b=$(printf '%s\r' "$cmd" | base64 -w0);
  kubectl -n $NS exec "$POD" -- sh -c "echo $b | base64 -d > $L/vax$n.log.in" >/dev/null 2>&1; sleep "$s"; }
empty=0
shot(){ local cmd=$1 mark=$2 start bytes
  start=$(wc -c < "$HOSTL/vax$SURV.log")
  sayN "$SURV" "$cmd" "$CAD"
  { printf '\n########## %s ##########\n' "$mark"
    tail -c +$start "$HOSTL/vax$SURV.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'
  } >> "$C"
  bytes=$(( $(wc -c < "$HOSTL/vax$SURV.log") - start ))
  if [ "$bytes" -lt 30 ]; then empty=$((empty+1)); [ "$empty" -ge 5 ] && { log "console wedged"; return 1; }; else empty=0; fi
}

log "RUN $TAG pod=$POD -- reboot VAX$DEP, watch survivor VAX$SURV reclaim its CSB"
bash /data/training/vax/cluster/tools/lab2login.sh "$POD" $SURV | grep -q LOGGED-IN || { log "FATAL survivor not at DCL"; exit 2; }
sayN "$SURV" 'REPLY/ENABLE=(CLUSTER,CONFIG)' 2
sayN "$SURV" 'ANALYZE/SYSTEM' 8
sayN "$SURV" 'SET OUTPUT SYS$OUTPUT' 3
shot 'SHOW CLUSTER' 'PRE-RET FULL (VAX2 lingering CSB)'
shot "SHOW CLUSTER/NODE=VAX$DEP" 'PRE-RET VAX2'
log "baseline captured (VAX$DEP CSB should be long_break,removed @ its old address)"

# capture
kx timeout $((WATCH+120)) tcpdump -i br0 -w "$L/$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!; sleep 2

# --- reboot VAX2 (the RETURN) -- csbcycle.sh construct: kill halted SIMH + its
#     nodedrv, launch a fresh nodedrv that boots the SAME root (R5 selects SYS1)
V2=$(kx sh -c 'for p in $(pgrep -x vax); do case "$(readlink /proc/$p/cwd)" in *vax2) echo $p;; esac; done' | tr -d '\r' | head -1)
ND2=$(kx sh -c 'for p in $(pgrep -f nodedrv); do case "$(tr "\0" " " </proc/$p/cmdline)" in *vax2*) echo $p;; esac; done' | tr -d '\r' | head -1)
log "VAX$DEP simh=$V2 nodedrv=$ND2 -- killing both, relaunching with boot"
[ -n "$ND2" ] && kx kill -9 "$ND2" 2>/dev/null || true
[ -n "$V2" ] && kx kill -9 "$V2" 2>/dev/null || true
sleep 3
kubectl -n $NS exec "$POD" -- sh -c \
  "cd $LAB && python3 /usr/local/bin/nodedrv.py $LAB/vax2 $L/vax2.log --no-detach --date '11-AUG-2026 20:40' --boot 'B/R5:10000000 DUA0'" >/dev/null 2>&1 &
log "VAX$DEP reboot launched -- watching survivor CSB for ${WATCH}s"

T0=$(date +%s); RECLAIMED=""; READMITTED=""
while [ $(( $(date +%s) - T0 )) -lt "$WATCH" ]; do
  el=$(( $(date +%s) - T0 ))
  shot "SHOW CLUSTER/NODE=VAX$DEP" "R+${el}s" || break
  blk=$(awk '/########## R.'"$el"'s /{f=1} f' "$C" | tail -20)
  csid=$(echo "$blk" | grep -aoE 'CSID +[0-9A-F]{8}' | tail -1 | grep -aoE '[0-9A-F]{8}$')
  state=$(echo "$blk" | grep -aoiE 'State: +[0-9A-F]+ +[a-z_]+' | tail -1)
  memb=$(echo "$blk" | grep -aoiE 'member|long_break|removed|not found|no such' | sort -u | tr '\n' ',')
  echo "[R+${el}s] CSID=$csid state=$state flags=$memb" | tee -a "$S"
  if [ -n "$csid" ] && [ "$csid" != "00010002" ] && [ -z "$RECLAIMED" ]; then
    RECLAIMED=$el; log ">>> RECLAIM: old CSID 00010002 GONE; VAX$DEP now CSID=$csid (fresh) at R+${el}s"
  fi
  if echo "$memb" | grep -qi 'member' && ! echo "$memb" | grep -qi 'removed' && [ -z "$READMITTED" ]; then
    READMITTED=$el; log ">>> READMITTED: VAX$DEP back to member at R+${el}s"
  fi
done
shot 'SHOW CLUSTER' 'END-RET FULL (fresh CSB?)'
shot "SHOW CLUSTER/NODE=VAX$DEP" 'END-RET VAX2'
sayN "$SURV" 'EXIT' 3
kx pkill -9 -f tcpdump >/dev/null 2>&1 || true; wait $TCPD 2>/dev/null || true

log "==== O34RET RESULT ===="
log "old CSID 00010002 deallocated + fresh CSID assigned at: ${RECLAIMED:-NEVER}"
log "VAX$DEP readmitted to member at: ${READMITTED:-NEVER}"
echo "===$TAG-DONE===" >> "$S"
