#!/bin/bash
# startU.sh <art> <tag> <durations...> : the run-e88 live-shaped stall matrix on this rig, under TCG
# (this pod is not privileged, so /dev/kvm is not openable).
cd /lab/run-af4rig
for p in $(ps -eo pid,args | grep -E 'tstail.py|qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i bre88r' | grep -v grep | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "/lab/run-af4rig/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 $p 2>/dev/null; done
export ACCEL="tcg,thread=multi" ACCEL_CPU="-cpu max"
ART=$1; TAG=$2; shift 2
setsid nohup bash /lab/run-af4rig/startL.sh "$ART" "$TAG" "$@" > /lab/run-af4rig/matrixout-$TAG.log 2>&1 < /dev/null &
echo started $TAG
