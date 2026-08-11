#!/bin/bash
# stallpoll.sh <tag> <store> <duration> <node#> [ENV=V ...]
#
# vms-2f3: one OVMX join against the lab as it stands, with SDA polled on a
# CHOSEN node *while the refusal is happening* -- not once at the end.
#
# WHY THIS EXISTS, AND WHY THE NODE IS AN ARGUMENT. oneshot.sh takes a single
# SDA dump after the run, which cannot show whether anything MOVED; session j's
# neg.sh added polling but only ever asked VAX1. In run r1B the coordinator was
# VAX3, so the one node whose state actually decides the outcome was never the
# one being asked. The coordinator is named in the CLUB as `Curr. coord. CSID`
# and is whoever OVMX addresses its op 0x02 to (SCSD-I-CMCONFIG2 logs the node
# number). Point this at THAT node.
#
# The polls are cheap and non-invasive: ANALYZE/SYSTEM -> SHOW CLUSTER -> EXIT
# on the console, which the lab has been doing all along without perturbing a
# transition.
#
# Same absolute-store rule as oneshot.sh (see the comment there): a relative
# path silently became the default node name "OVMX" and invalidated five runs.
set -u
TAG=$1; STORE=$2; DUR=$3; NODE=$4; shift 4
CL=/home/baron/vax/cluster; W=$CL/work; T=/tmp/clean-vax1-test

STORE_ARG=$STORE
STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
if [ ! -r "$STORE" ]; then
  echo "stallpoll: FATAL -- sysgen store '$STORE_ARG' does not exist or is unreadable." >&2
  echo "           Pass an ABSOLUTE path; this script cd's to the OVMX repo root." >&2
  exit 2
fi
if [ ! -p "$T/vax$NODE.log.in" ]; then
  echo "stallpoll: FATAL -- no console FIFO for node $NODE at $T/vax$NODE.log.in" >&2
  exit 2
fi

cd /home/baron/projects/vms
S=$W/$TAG.status; : > "$S"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
cleanN(){ tr -d '\000' < "$T/vax$NODE.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }
sayN(){ printf '%s\r' "$1" > "$T/vax$NODE.log.in"; sleep "${2:-2}"; }

# One SDA snapshot from node $NODE, tagged so it can be sliced out of the log.
snap(){
  local mark=$1
  local start
  start=$(wc -c < "$T/vax$NODE.log")
  sayN "WRITE SYS\$OUTPUT \"===$TAG-$mark===\"" 1
  sayN 'ANALYZE/SYSTEM' 8; sayN 'SET OUTPUT SYS$OUTPUT' 2
  sayN 'SHOW CLUSTER' 14; sayN 'EXIT' 4
  sayN "WRITE SYS\$OUTPUT \"===$TAG-$mark-END===\"" 2
  echo "----- $mark -----" >> "$S"
  tail -c +$start "$T/vax$NODE.log" | tr -d '\000' \
    | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' \
    | sed -n "/===$TAG-$mark===/,/===$TAG-$mark-END===/p" \
    | grep -aE '^[0-9A-F]{8}  |^Flags: |Last trans|Curr. coord|Index of next|Member State' >> "$S"
}

log "RUN $TAG store=$STORE dur=$DUR polling node $NODE env='$*'"
sudo timeout $((DUR+30)) tcpdump -i br0 -w "$W/d94-$TAG.pcap" 'ether proto 0x6007' \
     2>"$W/tcpdump-$TAG.err" &
sleep 2
sudo env OVMX_SYSGEN_PATH="$STORE" "$@" build-d94/bin/SCSD.EXE \
     --connect --duration "$DUR" --iface br0 > "$W/scsd-$TAG.log" 2>&1 &
P=$!

T0=$(date +%s)
for at in 15 40 70 105; do
  now=$(( $(date +%s) - T0 ))
  [ $now -lt $at ] && sleep $(( at - now ))
  snap "T+${at}s"
done

log "waitnodes:"
bash "$CL/tools/waitnodes.sh" 4 3 2>&1 | tee -a "$S"
log "XITDONE=$(grep -ac XITDONE "$W/scsd-$TAG.log")  XITABORT=$(grep -ac "SCSD-I-CMOP04" "$W/scsd-$TAG.log")"
snap "FINAL"
sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $P 2>/dev/null
log "CM census:"
grep -ao 'SCSD-T-CMIN, cat 0x[0-9a-f]* op 0x[0-9a-f]*' "$W/scsd-$TAG.log" \
  | sed 's/SCSD-T-CMIN, //' | sort | uniq -c | sort -rn | tee -a "$S"
log "CNXMAN on node $NODE:"
cleanN | tail -80 | grep -aiE 'CNXMAN|proposed|abort|removed' | sed 's/^ *//' \
  | sort -u | head -12 | tee -a "$S"
echo "===$TAG-DONE===" >> "$S"
