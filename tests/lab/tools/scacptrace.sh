#!/bin/bash
# scacptrace.sh <tag> <store> <duration> <node#> [ENV=V ...]
#
# vms-2f3: HIGH-CADENCE SCACP trace of a join, with a packet capture, so the
# ORDER of events at the PEDRIVER layer can be established.
#
# WHY THIS EXISTS RATHER THAN scacppoll.sh. scacppoll drives `MC SCACP` /
# SHOW VC / SHOW CHANNEL / EXIT for every sample, which costs ~23 s of console
# round-trips. That is fine for "is this VC collapsed at T+20" and useless for
# "did it collapse BEFORE or AFTER our op 0x02 went unanswered", which is the
# question that decides whether §4d.9's congestion collapse is a cause or a
# consequence.
#
# TWO TRICKS MAKE THE CADENCE POSSIBLE:
#  1. Enter SCACP ONCE and stay at its prompt, firing bare `SHOW VC` at
#     intervals. No process startup per sample.
#  2. SCACP's own output header carries a timestamp --
#     "VAX1 PEA0 VC Summary  1-AUG-2026 18:09:53.86:" -- so every sample dates
#     itself and no console markers are needed. VAX1's clock tracks the host
#     clock in this lab (its OPCOM lines match host time to the second), so
#     these timestamps compare directly against tcpdump and the SCSD log.
#
# Read-only: SHOW VC only.
set -u
TAG=$1; STORE=$2; DUR=$3; NODE=$4; shift 4
CL=/home/baron/vax/cluster; W=$CL/work; T=/tmp/clean-vax1-test
CADENCE=${CADENCE:-4}

STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
[ -r "$STORE" ] || { echo "scacptrace: FATAL -- unreadable store '$STORE'" >&2; exit 2; }
[ -p "$T/vax$NODE.log.in" ] || { echo "scacptrace: FATAL -- no console for node $NODE" >&2; exit 2; }

cd /home/baron/projects/vms
OUT=$W/$TAG.scacptrace
say(){ printf '%s\r' "$1" > "$T/vax$NODE.log.in"; sleep "${2:-1}"; }

START_BYTE=$(wc -c < "$T/vax$NODE.log")
echo "[$(date +%T.%N)] RUN $TAG store=$STORE dur=$DUR node=$NODE cadence=${CADENCE}s env='$*'" | tee "$OUT"

sudo timeout $((DUR+30)) tcpdump -i br0 -w "$W/d94-$TAG.pcap" 'ether proto 0x6007' \
     2>"$W/tcpdump-$TAG.err" &
sleep 2

# Park the console inside SCACP BEFORE the run starts, so sample 1 is cheap.
say 'MC SCACP' 5

echo "[$(date +%T.%N)] SCSD start" | tee -a "$OUT"
sudo env OVMX_SYSGEN_PATH="$STORE" "$@" build-d94/bin/SCSD.EXE \
     --connect --duration "$DUR" --iface br0 > "$W/scsd-$TAG.log" 2>&1 &
P=$!

T0=$(date +%s)
while [ $(( $(date +%s) - T0 )) -lt "$DUR" ]; do
  say 'SHOW VC' "$CADENCE"
done
say 'EXIT' 3

echo "[$(date +%T.%N)] XITDONE=$(grep -ac XITDONE "$W/scsd-$TAG.log")" | tee -a "$OUT"
sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $P 2>/dev/null

tail -c +$START_BYTE "$T/vax$NODE.log" | tr -d '\000' \
  | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' >> "$OUT"
echo "wrote $OUT and $W/d94-$TAG.pcap"
