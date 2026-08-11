#!/bin/bash
# connpoll.sh <tag> <store> <duration> <node#> <SCSNODE> [ENV=V ...]
#
# vms-2f3: one OVMX join with SDA *SHOW CONNECTIONS/NODE=<SCSNODE>* sampled on a
# chosen peer THROUGHOUT the run -- the CDT the peer holds for our identity, and
# its `Rej/Disconn Reason` field.
#
# WHY THIS EXISTS. Every peer-side oracle used on this item so far (OPCOM, SDA
# SHOW CLUSTER, DCL SHOW CLUSTER, SCACP) reports connection-manager or PEDRIVER
# *state*. Section 4d.9 identified SHOW CONNECTIONS as carrying an actual reason
# code and then never read it. This is the only oracle in the lab that names a
# REJECTION instead of describing a silence.
#
# THREE HARNESS LESSONS ARE BAKED IN -- the first cost run w2A outright.
#
#  1. STAY INSIDE SDA. The first version drove ANALYZE/SYSTEM ... EXIT per
#     sample. During a join the peer console is flooded with OPCOM broadcasts,
#     and typing a fresh command sequence into that flood overran the terminal
#     input buffer: '%RMS-F-RER, file read error / -SYSTEM-W-DATAOVERUN'. The
#     T+55s and FINAL markers were swallowed and three of four snapshots came
#     back EMPTY. scacptrace.sh already solved this for SCACP (sec 4d.10) --
#     enter the utility once, then fire a bare command on a timer.
#  2. NARROW THE QUERY. Bare SHOW CONNECTIONS prints every CDT in full (~230
#     lines/sample). SDA VAX 7.3 supports /NODE=name, /SYSAP=name and
#     /ADDRESS=n (verified via HELP on the lab). /NODE= makes a sample a few
#     lines, which is what makes repeated sampling safe.
#  3. NO CONSOLE MARKERS. Markers are what got truncated. Samples are separated
#     by counting 'Connection Descriptor Table' / 'SHOW CONNECTIONS' headers in
#     the slice, and the send times are recorded host-side instead.
#
# Read-only: SHOW CONNECTIONS only. Same absolute-store rule as oneshot.sh.
set -u
TAG=$1; STORE=$2; DUR=$3; NODE=$4; SCSNODE=$5; shift 5
CL=/home/baron/vax/cluster; W=$CL/work; T=/tmp/clean-vax1-test
CADENCE=${CADENCE:-12}

STORE_ARG=$STORE
STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
[ -r "$STORE" ] || { echo "connpoll: FATAL -- unreadable store '$STORE_ARG'; pass an ABSOLUTE path" >&2; exit 2; }
[ -p "$T/vax$NODE.log.in" ] || { echo "connpoll: FATAL -- no console FIFO for node $NODE" >&2; exit 2; }
# A logged-OUT console silently yields empty samples -- run s5A lost an entire
# experiment to exactly that (sec 4d.8). Prove DCL before spending the run.
bash "$CL/tools/loginN.sh" "$NODE" | grep -q LOGGED-IN \
  || { echo "connpoll: FATAL -- node $NODE is not at DCL; samples would be empty." >&2; exit 2; }

cd /home/baron/projects/vms
S=$W/$TAG.status; : > "$S"
C=$W/$TAG.conn;   : > "$C"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
say(){ printf '%s\r' "$1" > "$T/vax$NODE.log.in"; sleep "${2:-2}"; }

log "RUN $TAG store=$STORE dur=$DUR peer=VAX$NODE subject=$SCSNODE cadence=${CADENCE}s env='$*'"
START_BYTE=$(wc -c < "$T/vax$NODE.log")

sudo timeout $((DUR+40)) tcpdump -i br0 -w "$W/d94-$TAG.pcap" 'ether proto 0x6007' \
     2>"$W/tcpdump-$TAG.err" &
sleep 2

# Park inside SDA BEFORE the run so the first sample is cheap.
say 'ANALYZE/SYSTEM' 8
say 'SET OUTPUT SYS$OUTPUT' 3
say "SHOW CONNECTIONS/NODE=$SCSNODE" 6      # pre-run baseline
echo "[$(date +%T)] sample 1 (pre-run)" >> "$S"

log "SCSD start"
sudo env OVMX_SYSGEN_PATH="$STORE" "$@" build-d94/bin/SCSD.EXE \
     --connect --duration "$DUR" --iface br0 > "$W/scsd-$TAG.log" 2>&1 &
P=$!

T0=$(date +%s); n=1
while [ $(( $(date +%s) - T0 )) -lt "$DUR" ]; do
  n=$((n+1))
  echo "[$(date +%T)] sample $n  (T+$(( $(date +%s) - T0 ))s)" >> "$S"
  say "SHOW CONNECTIONS/NODE=$SCSNODE" "$CADENCE"
done
say 'EXIT' 4

log "waitnodes:"
bash "$CL/tools/waitnodes.sh" 4 3 2>&1 | tee -a "$S"
log "XITDONE=$(grep -ac XITDONE "$W/scsd-$TAG.log")  XITABORT=$(grep -ac SCSD-I-CMOP04 "$W/scsd-$TAG.log")  RETX=$(grep -ac 'RETRANSMIT 0x7b' "$W/scsd-$TAG.log")"
sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $P 2>/dev/null

tail -c +$START_BYTE "$T/vax$NODE.log" | tr -d '\000' \
  | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g' > "$C"

{
  echo "----- samples of SHOW CONNECTIONS/NODE=$SCSNODE on VAX$NODE -----"
  grep -aE 'SHOW CONNECTIONS|Connection Descriptor Table|^State:|Local Process|Rej/Disconn Reason|Remote Con. ID|Rem. Sta|no connections|%SDA' "$C"
  echo "----- overrun check (empty is good) -----"
  grep -aE 'DATAOVERUN|RMS-F-RER' "$C" | head -3
} >> "$S"

log "CM census:"
grep -ao 'SCSD-T-CMIN, cat 0x[0-9a-f]* op 0x[0-9a-f]*' "$W/scsd-$TAG.log" \
  | sed 's/SCSD-T-CMIN, //' | sort | uniq -c | sort -rn | tee -a "$S"
log "identity on the wire: $(strings -a "$W/d94-$TAG.pcap" | grep -oE 'OVMX[A-Z0-9]{2}' | sort -u | tr '\n' ' ')"
echo "===$TAG-DONE===" >> "$S"
