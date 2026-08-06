#!/bin/bash
# conflictbracket.sh <pod> <tag> <SCSNODE> <SCSSYSTEMID> [poll-iterations]
#
# vms-1ae: one arm of the "%PEA0, Remote System Conflicts with Known System"
# bracket, run against a LAB-2 pod (k3s, one pod == one isolated 2-node
# VMScluster).  Derived from tools/lab2run.sh; the differences are the point:
#
#   1. IDENTITY IS AN ARGUMENT, NOT A STORE.  The refusal under test is keyed on
#      SCSNODE / SCSSYSTEMID, so the arm mints its own store from the known-good
#      template rather than reusing a file whose identity you have to go and
#      look up.  Two arms that differ in exactly one of the two fields is the
#      whole experiment.
#
#   2. THE CONSOLE IS WINDOWED.  vax1.log / vax2.log are append-only for the
#      life of the pod and already contain every earlier run's OPCOM traffic.
#      A bare `grep Conflicts vax1.log` therefore reports a message some other
#      agent's run produced hours ago.  Byte offsets are snapshotted before the
#      daemon starts and only the appended region is searched.
#
#   3. BOTH CONSOLES ARE READ (guardrail 7).  The port driver logs the conflict
#      on every node that polls the offender, and vax2's console is free.
#
#   4. SHOW CLUSTER IS SAMPLED BEFORE AND AFTER.  The precondition for the
#      refusal is the peer already knowing a system with the same SCSSYSTEMID
#      or SCSNODE -- that set is exactly what SHOW CLUSTER prints, so an arm
#      that does not record it cannot say what it collided with.
#
# Result line is machine-readable:  RESULT <tag> CONFLICT=yes|no NODE=<name> ...
set -u
POD=$1; TAG=$2; NODE=$3; SYSID=$4; ITERS=${5:-5}
NS=${NS:-ovmx-lab}
L=${L:-/lab/k8s-labs/$POD/logs}
HOSTL=${HOSTL:-/data/training/vax/k8s-labs/$POD/logs}
W=${W:-/data/training/vax/cluster/work}
TOOLS=${TOOLS:-/data/training/vax/cluster/tools}
TEMPLATE=${TEMPLATE:-$W/sysgen-s8.dat}
SCSD_BIN=${SCSD_BIN:?set SCSD_BIN to the daemon under test}
DUR=${DUR:-90}

S=$W/$TAG.status; : > "$S"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax1.log.in"; sleep "${2:-2}"; }
clean(){ tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }
# Only the bytes this arm appended.
win(){ tail -c "+$2" "$HOSTL/$1.log" 2>/dev/null | clean; }

kubectl -n $NS get pod "$POD" >/dev/null 2>&1 || { echo "conflictbracket: FATAL -- no pod $POD" >&2; exit 2; }
[ -x "$SCSD_BIN" ] || { echo "conflictbracket: FATAL -- no daemon at $SCSD_BIN" >&2; exit 2; }

STORE=$W/sysgen-$TAG.dat
# ALLOW_COLLISION=1 is for the arms where the collision IS the experiment.
MKFLAGS=""; [ "${ALLOW_COLLISION:-0}" = "1" ] && MKFLAGS="--force"
python3 "$TOOLS/mk_sysgen.py" $MKFLAGS "$STORE" "$NODE" "$SYSID" "$TEMPLATE" \
  || { echo "conflictbracket: FATAL -- mk_sysgen failed" >&2; exit 2; }
rm -f "$STORE.cluster"
log "ARM $TAG pod=$POD SCSNODE=$NODE SCSSYSTEMID=$SYSID store=$STORE daemon=$SCSD_BIN"

# Precondition sample: which systems does the peer already know?
say "SET TERMINAL/DEVICE=VT100/BROADCAST/PAGE=200/WIDTH=132" 2
PRE1=$(( $(wc -c < "$HOSTL/vax1.log") + 1 ))
say "SHOW CLUSTER" 5
PRECLUSTER=$(win vax1 "$PRE1" | grep -aoE 'OVMX[A-Z0-9]{2}|VAX[0-9]' | sort -u | tr '\n' ' ')
log "known systems BEFORE: $PRECLUSTER"

# PREFLIGHT (vms-1ae). mk_sysgen's registry only knows stores minted on this
# host. SHOW CLUSTER is the cheapest peer-side view of what else is out there,
# so check our SCSNODE against it before burning two minutes on a run that is
# pre-refused and reads as a stall.
#
# It OVER-APPROXIMATES, deliberately, and this check does NOT depend on knowing
# why. Measured fact (sec 4(w), arm 1aeR): a name can still be listed by SHOW
# CLUSTER as BRK_NEW while that same collision draws no conflict at all -- so a
# name listed here does not GUARANTEE a conflict. Whether the poller's record
# ages out is one HYPOTHESIS for that gap and is explicitly UNPROVEN in 4(w)
# (two timepoints, one confounded pod, interval never measured); do not repeat
# it as a behaviour. The preflight only needs the observed direction -- SHOW
# CLUSTER's set is a superset of the poller's -- so it may abort a run that
# would in fact have been admitted, and that false abort costs nothing but a
# re-mint. It also under-covers the other half: SHOW CLUSTER prints node names,
# never SCSSYSTEMIDs, so the SCSSYSTEMID collision (arms 1aeC/1aeT2/1aeU2) is
# invisible here and is mk_sysgen's registry to catch.
if [ "${ALLOW_COLLISION:-0}" != "1" ] && echo " $PRECLUSTER" | grep -q " ${NODE:0:6} "; then
  log "ABORT -- SCSNODE ${NODE:0:6} is ALREADY a system this peer knows."
  log "  This peer MAY answer with '%PEA0, Remote System Conflicts with Known"
  log "  System', in which case the join can never complete and the run reads"
  log "  as a stall. The check over-approximates (see above) -- re-minting is"
  log "  cheaper than finding out. Mint a free identity:"
  log "  python3 $TOOLS/mk_sysgen.py --alloc <prefix> $W"
  exit 3
fi

RDIR="/lab/run-$TAG"
kx mkdir -p "$RDIR" >/dev/null
LOCAL_MD5=$(md5sum "$SCSD_BIN" | awk '{print $1}')
kubectl -n $NS cp "$SCSD_BIN" "$POD:$RDIR/SCSD.EXE" || exit 2
POD_MD5=$(kx md5sum "$RDIR/SCSD.EXE" | awk '{print $1}')
[ "$LOCAL_MD5" = "$POD_MD5" ] || { log "FATAL -- staged md5 mismatch"; exit 2; }
kubectl -n $NS cp "$STORE" "$POD:$RDIR/$TAG.sysgen" || exit 2
kx rm -f "$RDIR/$TAG.sysgen.cluster" >/dev/null 2>&1
kx chmod +x "$RDIR/SCSD.EXE"
log "staged SCSD.EXE md5=$LOCAL_MD5 (verified in-pod)"

# The console window this arm owns starts HERE -- after staging, before the
# daemon emits a single frame.
OFF1=$(( $(wc -c < "$HOSTL/vax1.log") + 1 ))
OFF2=$(( $(wc -c < "$HOSTL/vax2.log") + 1 ))

kubectl -n $NS exec "$POD" -- timeout $((DUR+40)) \
    tcpdump -i br0 -w "$L/1ae-$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!
sleep 2
kubectl -n $NS exec "$POD" -- sh -c \
    "cd $RDIR && OVMX_SYSGEN_PATH=$RDIR/$TAG.sysgen ./SCSD.EXE --connect --duration $DUR --iface br0 > $L/scsd-$TAG.log 2>&1" &
SCSDP=$!
log "SCSD started (tcpdump=$TCPD scsd=$SCSDP)"

joined=0
for i in $(seq 1 "$ITERS"); do
  sleep 10
  say "WRITE SYS\$OUTPUT \"CN_\"+F\$STRING(F\$GETSYI(\"CLUSTER_NODES\"))" 3
  n=$(win vax1 "$OFF1" | grep -aoE 'CN_[0-9]+' | tail -1)
  log "  t+$((i*13))s $n"
  [ "$n" = "CN_3" ] && { joined=1; log "JOINED -- CLUSTER_NODES=3"; break; }
done
[ $joined -eq 1 ] || log "NOT JOINED"

sleep 3
kx pkill -f SCSD.EXE >/dev/null 2>&1
wait $SCSDP 2>/dev/null; wait $TCPD 2>/dev/null

POST_MD5=$(kx md5sum "$RDIR/SCSD.EXE" 2>/dev/null | awk '{print $1}')
[ "$POST_MD5" = "$LOCAL_MD5" ] || log "FATAL -- SCSD.EXE changed mid-run; this arm is UNATTRIBUTABLE"

C1=$(win vax1 "$OFF1" | grep -ac 'Remote System Conflicts')
C2=$(win vax2 "$OFF2" | grep -ac 'Remote System Conflicts')
CLINE=$(win vax1 "$OFF1" | grep -a 'Remote System Conflicts' | head -1 | tr -s ' ')
[ -z "$CLINE" ] && CLINE=$(win vax2 "$OFF2" | grep -a 'Remote System Conflicts' | head -1 | tr -s ' ')
win vax1 "$OFF1" > "$W/$TAG.vax1.txt"
win vax2 "$OFF2" > "$W/$TAG.vax2.txt"

say "SHOW CLUSTER" 5
POSTCLUSTER=$(win vax1 "$OFF1" | grep -aoE 'OVMX[A-Z0-9]{2}|VAX[0-9]' | sort -u | tr '\n' ' ')
log "known systems AFTER: $POSTCLUSTER"
log "identity on the wire: $(strings -a "$HOSTL/1ae-$TAG.pcap" 2>/dev/null | grep -oE 'OVMX[A-Z0-9]{2}' | sort -u | tr '\n' ' ')"
CONF=no; [ "$C1" -gt 0 ] || [ "$C2" -gt 0 ] && CONF=yes
log "RESULT $TAG CONFLICT=$CONF NODE=$NODE SYSID=$SYSID vax1_hits=$C1 vax2_hits=$C2 joined=$joined"
[ -n "$CLINE" ] && log "  conflict line: $CLINE"
