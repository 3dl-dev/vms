#!/bin/bash
# rejoin_arm_lab2.sh <pod> <tag> <store-basename> <duration> [ENV=V ...]
#
# vms-71d: run ONE OVMX join arm against a lab-2 pod, driving the daemon and
# capture INSIDE the pod (br0 lives in the pod netns). Unlike lab2run.sh this
# uses a FIXED in-pod sysgen path (/lab/<store>) so the prior-admission sidecar
# (<store>.cluster) persists in place between arms of a rejoin triple -- the
# rejoin arm must read the sidecar the first-join arm wrote. The daemon
# (/lab/71d-SCSD.EXE) and the stores are staged once by the caller.
#
# A lab-2 cluster is 2 nodes, so a completed join is CLUSTER_NODES=3.
set -u
POD=$1; TAG=$2; STORE=$3; DUR=$4; shift 4
NS=ovmx-lab
L=/lab/k8s-labs/$POD/logs
HOSTL=/data/training/vax/k8s-labs/$POD/logs
K="kubectl -n $NS exec $POD --"
say(){ kubectl -n $NS exec "$POD" -- sh -c "printf '%s\\r' '$1' > $L/vax1.log.in"; sleep "${2:-2}"; }
clean(){ $K sh -c "tr -d '\000' < $L/vax1.log | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'"; }

echo "=== ARM $TAG store=/lab/$STORE dur=${DUR}s env='$*' ==="
$K sh -c "test -r /lab/$STORE.cluster && echo 'sidecar PRESENT (rejoin):' && cat /lab/$STORE.cluster || echo 'no sidecar (first join)'"

# capture inside the pod; held open on the host side
kubectl -n $NS exec "$POD" -- timeout $((DUR+30)) \
    tcpdump -i br0 -w "$L/d94-$TAG.pcap" -U -s 0 'ether proto 0x6007' >/dev/null 2>&1 &
TCPD=$!
sleep 2
kubectl -n $NS exec "$POD" -- sh -c \
    "cd /lab && OVMX_SYSGEN_PATH=/lab/$STORE $* ./71d-SCSD.EXE --connect --duration $DUR --iface br0 > $L/scsd-$TAG.log 2>&1" &
SCSDP=$!

joined=0
for i in $(seq 1 $(( (DUR/13)+1 )) ); do
  sleep 13
  say "WRITE SYS\$OUTPUT \"CN_\"+F\$STRING(F\$GETSYI(\"CLUSTER_NODES\"))" 3
  n=$(clean | grep -aoE 'CN_[0-9]+' | tail -1)
  echo "  t+$((i*13))s $n"
  if [ "$n" = "CN_3" ]; then joined=1; echo "  JOINED CLUSTER_NODES=3"; break; fi
done

sleep 2
$K pkill -f 71d-SCSD.EXE 2>/dev/null
wait $SCSDP 2>/dev/null; wait $TCPD 2>/dev/null
sleep 2
XIT=$($K grep -ac XITDONE "$L/scsd-$TAG.log" 2>/dev/null)
echo "  RESULT $TAG: joined=$joined XITDONE=$XIT"
echo "  identity on wire: $(strings -a "$HOSTL/d94-$TAG.pcap" 2>/dev/null | grep -oaE 'OVMX[A-Z0-9]{2}' | sort -u | tr '\n' ' ')"
