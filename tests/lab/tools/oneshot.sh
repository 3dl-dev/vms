#!/bin/bash
# oneshot.sh <tag> <store> <duration> [ENV=V ...] -- one OVMX join against the lab AS IT STANDS.
set -u
TAG=$1; STORE=$2; DUR=$3; shift 3
CL=/home/baron/vax/cluster; W=$CL/work; T=/tmp/clean-vax1-test
# vms-2f3 2026-08-01: resolve STORE to an ABSOLUTE path BEFORE the cd below, and
# refuse to run without it. This script cd's to the repo root, so a relative
# store path silently resolved to nothing, SCSD fell back to the default node
# name "OVMX", and five runs meant to be five distinct fresh identities all went
# on the wire as ONE node. Every "fresh identity" positive control among them
# was really a rejoin. Fail loudly instead.
STORE_ARG=$STORE
STORE=$(readlink -f -- "$STORE" 2>/dev/null || echo "$STORE")
if [ ! -r "$STORE" ]; then
  echo "oneshot: FATAL -- sysgen store '$STORE_ARG' does not exist or is unreadable." >&2
  echo "         Pass an ABSOLUTE path; this script cd's to the OVMX repo root." >&2
  exit 2
fi
cd /home/baron/projects/vms
S=$W/$TAG.status; : > "$S"
log(){ echo "[$(date +%T)] $*" | tee -a "$S"; }
clean(){ tr -d '\000' < "$T/vax1.log" | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }
say(){ printf '%s\r' "$1" > "$T/vax1.log.in"; sleep "${2:-2}"; }
log "RUN $TAG store=$STORE dur=$DUR env='$*'"
sudo timeout $((DUR+30)) tcpdump -i br0 -w "$W/d94-$TAG.pcap" 'ether proto 0x6007' 2>"$W/tcpdump-$TAG.err" &
sleep 2
sudo env OVMX_SYSGEN_PATH="$STORE" "$@" build-d94/bin/SCSD.EXE \
     --connect --duration "$DUR" --iface br0 > "$W/scsd-$TAG.log" 2>&1 &
P=$!
out=$(bash "$CL/tools/waitnodes.sh" 4 $((DUR/10)) 2>&1); rc=$?
echo "$out" | tee -a "$S"
log "waitnodes rc=$rc"
sleep 5
log "XITDONE=$(grep -ac XITDONE "$W/scsd-$TAG.log")  XITGO=$(grep -ac XITGO "$W/scsd-$TAG.log")"
say "WRITE SYS\$OUTPUT \"===$TAG-SDA-BEGIN===\"" 1
say 'ANALYZE/SYSTEM' 8; say 'SET OUTPUT SYS$OUTPUT' 2; say 'SHOW CLUSTER' 14; say 'EXIT' 4
say "WRITE SYS\$OUTPUT \"===$TAG-SDA-END===\"" 2
log "SDA CSB list:"
clean | sed -n "/===$TAG-SDA-BEGIN===/,/===$TAG-SDA-END===/p" | grep -aE '^[0-9A-F]{8}  |^Flags: [0-9A-F]{8} cluster' | tee -a "$S"
log "OVMX CSB detail:"
clean | sed -n "/===$TAG-SDA-BEGIN===/,/===$TAG-SDA-END===/p" | awk '/OVMX.*Cluster System Block/,/Cache Protocol/' | head -20 | tee -a "$S"
sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $P 2>/dev/null
log "CNXMAN since run start:"
clean | tail -120 | grep -aiE 'CNXMAN|OVMX' | sed 's/^ *//' | sort -u | head -20 | tee -a "$S"
echo "===$TAG-DONE===" >> "$S"
