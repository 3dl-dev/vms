#!/bin/bash
# lab2run.sh <pod> <tag> <store> <duration> [ENV=V ...]
#
# vms-2f3 / vms-a5c: run ONE OVMX join against a LAB-2 pod (k3s), which is a
# complete, isolated 2-node VMScluster of its own.
#
# WHY THIS IS NOT oneshot.sh. Lab-2's br0 lives inside the pod's network
# namespace, so SCSD cannot reach it from the host -- the daemon, the capture and
# the console FIFO writes all have to happen INSIDE the pod. Logs and captures
# land on the tank volume, which is readable from workshop at
# /data/training/vax/k8s-labs/<pod>/logs/, so only the writes need kubectl.
#
# A lab-2 cluster is TWO nodes (vax1+vax2), so a successful OVMX join is
# CLUSTER_NODES=3, not 4.
#
# ⚠ DO NOT MIX LAB-1 AND LAB-2 RUNS INSIDE ONE COMPARISON. Different SIMH
# binary (same source, different arch). Build a lab-2 comparison entirely out of
# lab-2 runs. See tests/lab/README.md and the lab-2 warning at the top of
# docs/HANDOFF-vms-2f3.md.
set -u
POD=$1; TAG=$2; STORE=$3; DUR=$4; shift 4
# vms-1da: NS/L/HOSTL/W overridable (defaults unchanged) so the staging +
# md5-verification logic can be exercised against a mocked kubectl/pod-fs in
# a test harness, without a live k3s cluster or the real tank mounts.
NS=${NS:-ovmx-lab}
L=${L:-/lab/k8s-labs/$POD/logs}
HOSTL=${HOSTL:-/data/training/vax/k8s-labs/$POD/logs}
W=${W:-/data/training/vax/cluster/work}
REPO=${REPO:-/home/baron/projects/vms}
# vms-578: the daemon under test is overridable, so a worktree can run its
# OWN build without clobbering the shared build-d94 artifact another agent
# may be mid-run with. Default unchanged.
SCSD_BIN=${SCSD_BIN:-$REPO/build-d94/bin/SCSD.EXE}

STORE_ARG=$STORE
STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
if [ ! -r "$STORE" ]; then
  echo "lab2run: FATAL -- unreadable store '$STORE_ARG'; pass an ABSOLUTE path" >&2
  exit 2
fi
kubectl -n $NS get pod "$POD" >/dev/null 2>&1 || { echo "lab2run: FATAL -- no pod $POD" >&2; exit 2; }

S=$W/$TAG.status; : > "$S"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
kx(){ kubectl -n $NS exec "$POD" -- "$@"; }
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax1.log.in"; sleep "${2:-2}"; }
clean(){ tr -d '\000' < "$HOSTL/vax1.log" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }

log "RUN $TAG pod=$POD store=$STORE dur=$DUR daemon=$SCSD_BIN env='$*'"

# vms-1da: /lab is the tank volume, mounted into EVERY pod and shared with
# lab-1. Staging the daemon at a fixed /lab/SCSD.EXE meant concurrent lab-2
# sessions (or lab-1) silently clobbered each other's binary mid-run -- one
# arm's captures got attributed to another arm's daemon. Stage per-run, under
# a directory namespaced by this run's TAG (already required to be unique --
# see the RUN log line above), never touch a shared path, and prove the copy
# landed byte-exact before AND after the run instead of trusting cp's exit code.
RDIR="/lab/run-$TAG"
kx mkdir -p "$RDIR"

# Stage the daemon and the identity store into the pod. Never discard cp's
# stderr -- a truncated/failed copy must be visible, not silently swallowed.
[ -x "$SCSD_BIN" ] || { echo "lab2run: FATAL -- no daemon at $SCSD_BIN" >&2; exit 2; }
LOCAL_MD5=$(md5sum "$SCSD_BIN" | awk '{print $1}')
if ! kubectl -n $NS cp "$SCSD_BIN" "$POD:$RDIR/SCSD.EXE"; then
  echo "lab2run: FATAL -- kubectl cp of $SCSD_BIN to $POD:$RDIR/SCSD.EXE failed" >&2
  exit 2
fi
POD_MD5=$(kx md5sum "$RDIR/SCSD.EXE" 2>/dev/null | awk '{print $1}')
if [ "$LOCAL_MD5" != "$POD_MD5" ]; then
  echo "lab2run: FATAL -- staged SCSD.EXE md5 mismatch: local=$LOCAL_MD5 pod=$POD_MD5" >&2
  exit 2
fi
log "staged SCSD.EXE at $RDIR/SCSD.EXE md5=$LOCAL_MD5 (verified in-pod)"
if ! kubectl -n $NS cp "$STORE" "$POD:$RDIR/$TAG.sysgen"; then
  echo "lab2run: FATAL -- kubectl cp of $STORE to $POD:$RDIR/$TAG.sysgen failed" >&2
  exit 2
fi
# The prior-admission sidecar (sec 4d.2) is part of the identity -- carry it if
# it exists, and make sure a stale one never leaks in if it does not.
if [ -r "$STORE.cluster" ]; then
  if ! kubectl -n $NS cp "$STORE.cluster" "$POD:$RDIR/$TAG.sysgen.cluster"; then
    echo "lab2run: FATAL -- kubectl cp of $STORE.cluster to $POD:$RDIR/$TAG.sysgen.cluster failed" >&2
    exit 2
  fi
  log "carried prior-admission sidecar ($(wc -c < "$STORE.cluster") bytes)"
else
  kx rm -f "$RDIR/$TAG.sysgen.cluster" 2>/dev/null
  log "no prior-admission sidecar -- this identity has never been admitted anywhere"
fi
kx chmod +x "$RDIR/SCSD.EXE"

# ⚠ A backgrounded process does NOT survive `kubectl exec` teardown -- a nohup'd
# tcpdump died at 26 s once and produced a misleadingly short capture. HOLD THE
# SESSION OPEN: run each in a FOREGROUND exec, backgrounded on the HOST side.
kubectl -n $NS exec "$POD" -- timeout $((DUR+40)) \
    tcpdump -i br0 -w "$L/d94-$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!
sleep 2
kubectl -n $NS exec "$POD" -- sh -c \
    "cd $RDIR && OVMX_SYSGEN_PATH=$RDIR/$TAG.sysgen $* ./SCSD.EXE --connect --duration $DUR --iface br0 > $L/scsd-$TAG.log 2>&1" &
SCSDP=$!
log "SCSD started in $POD (exec sessions held open: tcpdump=$TCPD scsd=$SCSDP)"

# Poll the cluster size from vax1's console. Lab-2 is 2 nodes, so 3 == joined.
joined=0
for i in $(seq 1 12); do
  sleep 10
  say "WRITE SYS\$OUTPUT \"CN_\"+F\$STRING(F\$GETSYI(\"CLUSTER_NODES\"))" 3
  n=$(clean | grep -aoE 'CN_[0-9]+' | tail -1)
  log "  t+$((i*13))s $n"
  if [ "$n" = "CN_3" ]; then joined=1; log "JOINED -- CLUSTER_NODES=3"; break; fi
done
[ $joined -eq 1 ] || log "NOT JOINED -- last $(clean | grep -aoE 'CN_[0-9]+' | tail -1)"

sleep 3
kx pkill -f SCSD.EXE 2>/dev/null
wait $SCSDP 2>/dev/null; wait $TCPD 2>/dev/null

# vms-1da: prove the binary that ran for the whole duration is still the one
# we staged -- a per-run dir stops a CONCURRENT session from clobbering it,
# but this is the check that would have caught it happening anyway instead of
# silently trusting the staging-time verification.
POST_MD5=$(kx md5sum "$RDIR/SCSD.EXE" 2>/dev/null | awk '{print $1}')
if [ "$POST_MD5" != "$LOCAL_MD5" ]; then
  log "FATAL -- SCSD.EXE at $RDIR/SCSD.EXE changed during the run: staged=$LOCAL_MD5 post-run=$POST_MD5 -- this run's results are UNATTRIBUTABLE"
else
  log "post-run md5 verified: $RDIR/SCSD.EXE unchanged ($POST_MD5)"
fi
log "XITDONE=$(kx grep -ac XITDONE $L/scsd-$TAG.log 2>/dev/null)  RETX=$(kx grep -ac 'RETRANSMIT 0x7b' $L/scsd-$TAG.log 2>/dev/null)  PSC-UNGATED=$(kx grep -aoE 'PSC-UNGATED=[0-9]+' $L/scsd-$TAG.log 2>/dev/null | tail -1)"
log "CM census:"
kx grep -ao 'SCSD-T-CMIN, cat 0x[0-9a-f]* op 0x[0-9a-f]*' "$L/scsd-$TAG.log" 2>/dev/null \
  | sed 's/SCSD-T-CMIN, //' | sort | uniq -c | sort -rn | head -8 | tee -a "$S"
# Guardrail 18: prove WHICH identity reached the wire. Read the capture from the
# shared volume on the host -- the in-pod version of this needs another level of
# shell quoting and silently returned empty.
log "identity on the wire: $(strings -a "$HOSTL/d94-$TAG.pcap" 2>/dev/null | grep -oE 'OVMX[A-Z0-9]{2}' | sort -u | tr '\n' ' ')"

# ⚠ PULL THE PRIOR-ADMISSION SIDECAR BACK TO THE HOST STORE. OVMX writes it
# beside the store INSIDE the pod, and the store is re-copied per run -- so
# without this the identity forgets it was ever admitted and every run is a
# first join. The host store file is the canonical record of this identity's
# history, which is what lets a refused identity be carried to another pod.
if kubectl -n $NS exec "$POD" -- test -r "/lab/$TAG.sysgen.cluster" 2>/dev/null; then
  kubectl -n $NS cp "$POD:/lab/$TAG.sysgen.cluster" "$STORE.cluster" >/dev/null 2>&1
  log "pulled prior-admission sidecar back to $STORE.cluster ($(wc -c < "$STORE.cluster" 2>/dev/null) bytes)"
fi
echo "===$TAG-DONE===" >> "$S"
