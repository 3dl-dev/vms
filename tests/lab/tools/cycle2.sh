#!/bin/bash
# vms-2f3: JOIN/EXIT CYCLES with an SDA oracle and optional per-cycle identity.
#
#   cycle2.sh <tag> <NODE> <sysid> <cycles> <per-cycle-duration> [ENV=V ...]
#
# This is tools/cycle.sh with three changes, all made for vms-2f3:
#
#  1. SDA IS THE ORACLE, NOT DCL. `SHOW CLUSTER` in DCL renders an EMPTY TABLE
#     when the peers hold stale CSBs -- the oracle degrades before it errors.
#     ANALYZE/SYSTEM -> SHOW CLUSTER dumps the CSB list with State/Status, which
#     is how the long_break residue was found in the first place. This script
#     takes an SDA dump BEFORE the first join, after every cycle's exit, and at
#     the end, so the CSB for the dead incarnation is visible at every step.
#
#  2. PER-CYCLE IDENTITY (PER_CYCLE_SYSID=1). cycle.sh fixes one SCSSYSTEMID for
#     all cycles by generating the sysgen store once, before the loop. With
#     PER_CYCLE_SYSID=1 each cycle gets its own store at sysid+c-1, which
#     isolates a collision on IDENTITY from a collision on anything else about
#     the restart. Default 0 = same identity every cycle (the reproducer).
#
#  3. PER-CYCLE ENV (CYCLE<N>_ENV="A=1 B=2"). Lets cycle 2 run with a different
#     OVMX build-time knob than cycle 1 without a second lab reset -- e.g. give
#     only the rejoin a live incarnation timestamp and keep cycle 1 as the
#     in-run control.
#
# NOT AN EXPLICIT LEAVE: OVMX is killed abruptly, which is a node FAILURE from
# the cluster's point of view (class 0x03). That is honest about what OVMX can
# do today; vms-b8a is the graceful-leave work.
set -u
TAG=$1; NODE=$2; SYSID=$3; CYCLES=$4; DUR=$5; shift 5
REJOIN_GAP_S=${REJOIN_GAP_S:-10}
PER_CYCLE_SYSID=${PER_CYCLE_SYSID:-0}
SKIP_RESET=${SKIP_RESET:-0}
CL=/home/baron/vax/cluster
W=$CL/work
T=/tmp/clean-vax1-test
cd /home/baron/projects/vms
STAT=$W/$TAG.status; : > "$STAT"
log(){ echo "[$(date +%T)] $*" | tee -a "$STAT"; }
say(){ printf '%s\r' "$1" > "$T/vax1.log.in"; sleep "${2:-2}"; }
clean(){ tr -d '\000' < "$T/vax1.log" 2>/dev/null | sed 's/\x1b\[[0-9;]*[a-zA-Z]//g'; }
die(){ log "$*"; sudo pkill -f 'SCSD.EXE' 2>/dev/null; echo "===$TAG-DONE===" >> "$STAT"; exit 1; }
wn(){ local out rc; out=$(bash "$CL/tools/waitnodes.sh" "$@" 2>&1); rc=$?
      echo "$out" | tee -a "$STAT"; return $rc; }

# SDA CSB dump. The summary table is what we want: node, CSID, State, Status.
# Everything after the CLUB block is per-node detail and is left in the console
# log for later reading rather than echoed here.
sda(){
  local when=$1
  say "WRITE SYS\$OUTPUT \"===${TAG}-SDA-${when}-BEGIN===\"" 1
  say 'ANALYZE/SYSTEM' 8
  say 'SET OUTPUT SYS$OUTPUT' 2
  say 'SHOW CLUSTER' 14
  say 'EXIT' 4
  say "WRITE SYS\$OUTPUT \"===${TAG}-SDA-${when}-END===\"" 2
  log "SDA CSB list @ ${when}:"
  clean | sed -n "/===${TAG}-SDA-${when}-BEGIN===/,/===${TAG}-SDA-${when}-END===/p" \
        | grep -aE '^[0-9A-F]{8}  ' | tee -a "$STAT"
  clean | sed -n "/===${TAG}-SDA-${when}-BEGIN===/,/===${TAG}-SDA-${when}-END===/p" \
        | grep -aE '^Flags: [0-9A-F]{8} cluster' | tee -a "$STAT"
}

if [ "$SKIP_RESET" = 1 ]; then
  log "PHASE0 SKIP_RESET=1 -- reusing the lab as it stands (verify it yourself)"
else
  log "PHASE0 reset3 (all three VAXes) -- one reset, then $CYCLES cycles on top of it"
  bash "$CL/tools/reset3.sh" > "$W/reset3-$TAG.out" 2>&1
fi
wn 3 30 || die "ABORT: 3-node precondition not verified"
say 'REPLY/ENABLE=(CLUSTER)' 2
sda pre

# Identity stores. One per cycle when PER_CYCLE_SYSID=1, otherwise one shared.
for c in $(seq 1 "$CYCLES"); do
  if [ "$PER_CYCLE_SYSID" = 1 ]; then S=$((SYSID + c - 1)); else S=$SYSID; fi
  "$CL/tools/mk_sysgen" "$W/sysgen-$TAG-c$c.dat" "$NODE" "$S" 2>&1
  log "cycle $c identity: SCSNODE=$NODE SCSSYSTEMID=$S"
done

# One capture spanning ALL cycles: a per-cycle capture would cut every
# transition in half, and the rejoin is only meaningful against its own control.
TOTAL=$(( (DUR + 180) * CYCLES + 240 ))
sudo timeout "$TOTAL" tcpdump -i br0 -w "$W/d94-$TAG.pcap" \
     'ether proto 0x6007' 2>"$W/tcpdump-$TAG.err" &
sleep 2
say "WRITE SYS\$OUTPUT \"===${TAG}-RUN-BEGIN===\"" 1

OK=0; FAILED_AT=""
for c in $(seq 1 "$CYCLES"); do
  L=$W/scsd-$TAG-c$c.log
  eval "CENV=\${CYCLE${c}_ENV:-}"
  log "--- CYCLE $c/$CYCLES: $NODE joins (store sysgen-$TAG-c$c.dat) extra-env='${CENV}' ---"
  # shellcheck disable=SC2086
  sudo env OVMX_SYSGEN_PATH="$W/sysgen-$TAG-c$c.dat" "$@" $CENV build-d94/bin/SCSD.EXE \
       --connect --duration "$DUR" --iface br0 > "$L" 2>&1 &
  SCSD=$!

  if ! wn 4 24; then
    FAILED_AT="cycle $c: never reached four nodes"
    log "CYCLE $c FAILED: $FAILED_AT"
    sda "c${c}fail"
    sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $SCSD 2>/dev/null
    break
  fi
  for i in $(seq 1 30); do
    [ "$(grep -ac XITDONE "$L")" -ge 1 ] && break
    sleep 4
  done
  if [ "$(grep -ac XITDONE "$L")" -lt 1 ]; then
    FAILED_AT="cycle $c: counted at four nodes but its own barrier never completed"
    log "CYCLE $c FAILED: $FAILED_AT"
    sda "c${c}fail"
    sudo pkill -f 'SCSD.EXE' 2>/dev/null; wait $SCSD 2>/dev/null
    break
  fi
  EP=$(grep -a 'SCSD-I-XITION,' "$L" | tail -1 | sed 's/.*epoch=\(0x[0-9A-Fa-f]*\).*/\1/')
  log "CYCLE $c JOINED   epoch=$EP  transitions=$(grep -ac XITDONE "$L")"

  # --- exit: OVMX vanishes. A failure, not a leave. ---
  sudo pkill -f 'SCSD.EXE' 2>/dev/null
  wait $SCSD 2>/dev/null
  log "CYCLE $c OVMX process killed -- the cluster must now remove it as a failed node"
  if wn 3 24; then
    log "CYCLE $c cluster settled back to three nodes"
    OK=$((OK+1))
  else
    FAILED_AT="cycle $c: cluster did not settle back to three nodes after OVMX vanished"
    log "CYCLE $c FAILED: $FAILED_AT"
    sda "c${c}stuck"
    break
  fi
  sda "c${c}post"
  log "CYCLE $c waiting ${REJOIN_GAP_S}s before the next join"
  sleep "$REJOIN_GAP_S"
done

sda end
say "WRITE SYS\$OUTPUT \"===${TAG}-RUN-END===\"" 1
cp "$T/vax1.log" "$W/vax1-$TAG.log" 2>/dev/null

log "==== SUMMARY: $OK/$CYCLES cycles completed cleanly ===="
[ -n "$FAILED_AT" ] && log "FIRST FAILURE: $FAILED_AT"
log "per-cycle: XITDONE / XITGO / ungrounded-tags / role-warnings"
for c in $(seq 1 "$CYCLES"); do
  L=$W/scsd-$TAG-c$c.log
  [ -f "$L" ] || continue
  log "  cycle $c: $(grep -ac XITDONE "$L") / $(grep -ac XITGO "$L") / $(grep -ac XITGOUNGROUNDED "$L") / $(grep -ac XITROLE "$L")"
  printf '  cycle %s epochs: %s\n' "$c" \
    "$(grep -a 'SCSD-I-XITION,' "$L" | sed 's/.*\(epoch=0x[0-9A-Fa-f]*\).*\(class=0x[0-9a-f]*\).*/\1 \2/' | sort -u | tr '\n' ' ')" \
    | tee -a "$STAT"
done
log "aborts on VAX1: $(clean | grep -ac 'aborting VAXcluster')"
log "bugchecks on VAX1: $(clean | grep -acE 'BUG ?CHECK|INCONSTATE|INVEXCEPTN|LOCKMGRERR|CLUEXIT')"
log "CNXMAN OPCOM (deduped):"
clean | sed -n "/===${TAG}-RUN-BEGIN===/,/===${TAG}-RUN-END===/p" \
      | grep -aiE "CNXMAN|$NODE" | grep -avE '^\$|SHOW CLUSTER' \
      | sed 's/^ *//' | sort -u | head -30
echo "===$TAG-DONE===" >> "$STAT"
