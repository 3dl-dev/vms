#!/bin/bash
# scacppoll.sh <tag> <store> <duration> <node#> [ENV=V ...]
#
# vms-2f3: one OVMX join, with SCACP (SCA Control Program) polled on a chosen
# node WHILE the join is in progress.
#
# WHY. Every peer-side oracle this investigation has used -- OPCOM, SDA
# SHOW CLUSTER, DCL SHOW CLUSTER -- reports connection-manager STATE. SCACP
# reports PEDRIVER's own view of the virtual circuit and its channels: VC state,
# how many channels are open, whether the channel is in the ECS (Equivalent
# Channel Set, i.e. usable), the transmit window, the retransmit timeout and the
# transmit-timeout count. That is a LOWER layer than CNXMAN, and a returning
# node whose op 0x02 is silently discarded may be being discarded there.
#
# Present on VAX/VMS 7.3 in this lab (verified 2026-08-01). Commands used are
# read-only: SHOW VC and SHOW CHANNEL.
set -u
TAG=$1; STORE=$2; DUR=$3; NODE=$4; shift 4
CL=/home/baron/vax/cluster; W=$CL/work; T=/tmp/clean-vax1-test

STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
[ -r "$STORE" ] || { echo "scacppoll: FATAL -- unreadable store '$STORE'" >&2; exit 2; }
[ -p "$T/vax$NODE.log.in" ] || { echo "scacppoll: FATAL -- no console for node $NODE" >&2; exit 2; }

cd /home/baron/projects/vms
S=$W/$TAG.scacp; : > "$S"
say(){ printf '%s\r' "$1" > "$T/vax$NODE.log.in"; sleep "${2:-2}"; }

snap(){
  local mark=$1 start
  start=$(wc -c < "$T/vax$NODE.log")
  say "WRITE SYS\$OUTPUT \"===$TAG-$mark===\"" 1
  say 'MC SCACP' 4; say 'SHOW VC' 8; say 'SHOW CHANNEL' 8; say 'EXIT' 3
  say "WRITE SYS\$OUTPUT \"===$TAG-$mark-END===\"" 2
  echo "========== $mark ==========" >> "$S"
  tail -c +$start "$T/vax$NODE.log" | tr -d '\000' \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
    | sed -n "/===$TAG-$mark===/,/===$TAG-$mark-END===/p" >> "$S"
}

echo "[$(date +%T)] RUN $TAG store=$STORE dur=$DUR scacp-poll node $NODE env='$*'" | tee -a "$S"
sudo env OVMX_SYSGEN_PATH="$STORE" "$@" build-d94/bin/SCSD.EXE \
     --connect --duration "$DUR" --iface br0 > "$W/scsd-$TAG.log" 2>&1 &
P=$!
T0=$(date +%s)
for at in 20 60; do
  now=$(( $(date +%s) - T0 )); [ $now -lt $at ] && sleep $(( at - now ))
  snap "T+${at}s"
done
echo "[$(date +%T)] XITDONE=$(grep -ac XITDONE "$W/scsd-$TAG.log")" | tee -a "$S"
sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $P 2>/dev/null
echo "===$TAG-DONE===" >> "$S"
