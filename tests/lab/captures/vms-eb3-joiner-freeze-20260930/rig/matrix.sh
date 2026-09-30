#!/bin/bash
# matrix.sh <art> <tag> <durations...> -- one arm per stall duration.
#
# THE STALL MATRIX (rd vms-8c54). RECNXINTERVAL is 20 s here, and the two sides
# of it are different faults:
#
#   stall < RECNXINTERVAL -- the node wakes while the real VAX STILL HOLDS it as
#                            a member, inside its own reconnect window. Whatever
#                            the node does on wake, it does to a peer that has
#                            not given it up.
#   stall > RECNXINTERVAL -- the VAX has already timed out and removed it, so
#                            the node wakes into a cluster it is no longer in.
#
# The live regression shows "lost connection to system OVMXA" with NO preceding
# "timed-out" line, which is the shape of a peer learning the loss from a LAST
# GASP rather than from its own clock -- i.e. the first case.
set -u
ART="$1"; TAG="$2"; shift 2
R=/lab/run-eb3
: > "$R/loop-$TAG.log"
i=0
for tok in "$@"; do
    i=$((i+1))
    # rd vms-e88: a token is ORDER:SECONDS (join order shuffled per arm) or SECONDS
    # rd vms-eb3: ...or ORDER:SECONDS:MODE, MODE = vax (the real VAX's own
    # "completing" line, rig arm P-3's trigger), member (OVMXB's own member
    # line, the vms-1f40 bar's), pkt:8109 or pkt:0a (armed on the wire in the
    # op-0a window, stallpkt.py).
    IFS=: read -r o d m1 m2 <<< "$tok"
    if [ -z "$d" ]; then d="$o"; o=""; fi
    if [ -n "$o" ]; then export JOIN_ORDER="$o"; fi
    unset MARK_LOG MARK TRIG_DELAY
    case "$m1" in
      pkt)    export STALL_MODE="pkt:$m2" ;;
      vax)    export STALL_MODE=console MARK_LOG=$R/VAXC.console.log TRIG_DELAY=0 \
                     MARK="%CNXMAN,  completing VAXcluster state transition" ;;
      member) export STALL_MODE=console MARK_LOG=$R/OVMXB.console.log TRIG_DELAY=2 \
                     MARK="this node is now a VAXcluster member" ;;
      *)      export STALL_MODE=console ;;
    esac
    echo "$tok" > "$R/arm-token"
    echo "===== $TAG-$i order=${JOIN_ORDER:-a-then-b} stall=${d}s mode=$STALL_MODE${MARK:+ mark=$MARK} start $(date -u +%H:%M:%S) =====" | tee -a "$R/loop-$TAG.log"
    STALL_S="$d" bash "$R/runarm.sh" "$ART" "$TAG-$i" >> "$R/loop-$TAG.log" 2>&1
    for _ in $(seq 1 ${JOIN_WAIT_BEATS:-170}); do
        grep -qa 'this node is now a VAXcluster member' "$R/OVMXB.console.log" 2>/dev/null && break
        sleep 2
    done
    sleep "${SETTLE_S:-60}"
    D="$R/runs/$TAG-$i"; mkdir -p "$D"
    cp "$R"/OVMXA.console.log "$R"/OVMXB.console.log "$R"/VAXC.console.log \
       "$R"/fault.out "$R"/join-order "$R"/arm-token "$D"/ 2>/dev/null
    echo "$d" > "$D/stall-seconds"
    for p in $(ps -eo pid,args | grep 'tcpdum[p] -i breb3r' | grep -v grep | awk '{print $1}'); do
        kill "$p" 2>/dev/null
    done
    sleep 2
    gzip -c "$R"/s8.pcap > "$D"/s8.pcap.gz 2>/dev/null
    bash "$R/grade3.sh" "$D" "$TAG-$i(${d}s)" | tee -a "$R/loop-$TAG.log"
done
echo "===== $TAG done: $(grep -c ' PASS ' "$R/loop-$TAG.log") PASS / $(grep -c ' FAIL ' "$R/loop-$TAG.log") FAIL =====" | tee -a "$R/loop-$TAG.log"
