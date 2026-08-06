#!/bin/bash
# portrun.sh <pod> <tag> <store> <dur> [ENV=V ...]   -- vms-0fe
#
# lab2run.sh with BOTH VAX consoles bracketed by BYTE OFFSET across the run.
#
# WHY. vms-0fe had to decide whether a specific port-driver console line
# ("%PEA0, Inappropriate SCA Control Message") was produced BY THIS RUN. The
# consoles are append-only logs shared by every run that ever touched the pod,
# so `grep PEA0 vax1.log` answers a different question -- it finds scrollback
# from somebody else's experiment and reads as a reproduction. Record wc -c on
# both consoles before the run and diff from there, or do not make a claim
# about what the peer said.
#
# Reading VAX2 as well as VAX1 is not optional either: which node prints the
# line is itself evidence (vms-0fe run 0feA1 sent its surviving DISCONNECT_REQ
# to VAX2, not VAX1).
#
# Everything else -- staging, md5 before AND after, identity-on-the-wire from
# the capture rather than the daemon's own log -- is lab2run.sh's and is
# inherited unchanged.
set -u
POD=$1; TAG=$2; STORE=$3; DUR=$4; shift 4
HOSTL=/data/training/vax/k8s-labs/$POD/logs
W=/data/training/vax/cluster/work
# Like every script in this directory this is a SNAPSHOT carrying absolute
# paths to the lab host; override REPO/SCSD_BIN to run it from a worktree.
REPO=${REPO:-/home/baron/projects/vms}
SCSD_BIN=${SCSD_BIN:-$REPO/build-d94/bin/SCSD.EXE}

V1=$(wc -c < "$HOSTL/vax1.log")
V2=$(wc -c < "$HOSTL/vax2.log" 2>/dev/null || echo 0)
echo "=== $TAG console baseline vax1=$V1 vax2=$V2"

SCSD_BIN=$SCSD_BIN bash "$REPO/tests/lab/tools/lab2run.sh" \
    "$POD" "$TAG" "$STORE" "$DUR" "$@"

sleep 8
echo "=== $TAG VAX1 console delta ==="
tail -c +$((V1+1)) "$HOSTL/vax1.log" | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
  | tee "$W/0fe-$TAG-vax1.txt" | grep -aiE 'PEA0|PEDRIVER|CNXMAN|OPCOM|virtual circuit|SCA' | sort -u
echo "=== $TAG VAX2 console delta ==="
tail -c +$((V2+1)) "$HOSTL/vax2.log" 2>/dev/null | tr -d '\000' | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
  | tee "$W/0fe-$TAG-vax2.txt" | grep -aiE 'PEA0|PEDRIVER|CNXMAN|OPCOM|virtual circuit|SCA' | sort -u
echo "=== $TAG done ==="
