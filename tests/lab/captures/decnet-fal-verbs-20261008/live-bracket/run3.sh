#!/bin/sh
# rd vms-b2f lab: three concurrent dapprobe serve instances (links 1..3 of one
# VMS command), each piping its link to faldrv2 (OVMX's compiled FAL server).
cd /lab/k8s-labs/dnlab-1/b2f
export ROUTER=1 DRV=/lab/k8s-labs/dnlab-1/b2f/faldrv2 SRVDIR=/lab/k8s-labs/dnlab-1/b2f/srv V=1
TAG=${TAG:-run}
for k in 0 1 2; do
  SKIP_CI=$k timeout ${TO:-120} python3 -u dapprobe_skipci.py dnp0 1.43 1.1 x x serve X > logs-$TAG-$k.log 2>&1 &
done
wait
