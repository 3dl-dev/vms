#!/bin/bash
# runarm.sh <artdir> <tag> -- one A/B arm of the rd vms-dfe lab repro.
#
# VAXC (real OpenVMS VAX V7.3, SIMH) founds; OVMXA joins; OVMXB joins while a
# DETERMINISTIC transient loss of connectivity is injected on its tap during
# its admission (tc-netem 100% loss, nothing injected on the wire). The only
# difference between the two arms is <artdir>: two boot-artifact sets built by
# the SAME workflow from two commits that differ by exactly one patch.
set -u
ART="$1"; TAG="$2"
R=/lab/run-eb3
STALL_S="${STALL_S:-30}"
JIT="${JIT:-80ms 40ms}"
# WHERE THE FAULT LANDS.
#
# rd vms-dfe armed on the JOINER's own console line that its VMS$VAXcluster
# connection was open. That is too early for rd vms-b36: on this build the
# joiner often loses the pair's connection before its membership request ever
# reaches the member, so the member never opens a transition and the window the
# CNXMGRERR lives in is never entered (measured: CTL-1 and CTL-2, 2026-09-25).
#
# rd vms-b36 therefore arms on the REAL VAX's OWN console line that it has
# opened the transition -- "%CNXMAN,  proposing addition of system OVMXB" --
# which is the exact point the archived bugcheck run reached before its tap went
# dark. Same fault (tc netem drops every frame on the joiner's tap, nothing
# injected, no frame altered), landed where the exhibit landed it, so the window
# is entered every run instead of one run in five.
#
# MARK_LOG selects which console the trigger watches; MARK is the line.
# rd vms-8c54: the trigger is the REAL VAX's OWN commit of the joiner's
# addition -- the instant the joiner became a member. stall.sh takes its
# baseline when it starts (after OVMXA is already in), so the first INCREASE of
# "completing VAXcluster state transition" is OVMXB's commit and nothing else.
MARK_LOG="${MARK_LOG:-$R/VAXC.console.log}"
MARK="${MARK:-%CNXMAN,  completing VAXcluster state transition}"
DELAY="${TRIG_DELAY:-0}"

# Kill the previous arm's emulators BY PID. (pkill -f would also match this
# script's own command line, and SIMH ignores a plain SIGTERM in its console
# loop -- both were observed here.)
# SCOPED TO THIS RUN ROOT. An unscoped pattern would also match the real-VMS
# oracle lab running in the same pod (its own nodedrv.py + ./vax vax.ini), and
# killing another rig's emulators is how a lab eats itself.
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i breb3r' \
           | grep -v grep | grep -E "$R|breb3r" | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do
    kill -9 "$p" 2>/dev/null
done
sleep 5
tc qdisc del dev tapBx root 2>/dev/null
mkdir -p "$R/$TAG"
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk "$R/nodeC/data/d0.dsk"
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/nvram.bin "$R/nodeC/data/nvram.bin"
rm -f "$R"/*.console.log "$R"/*.caps "$R"/s8.pcap "$R"/*.drv.out "$R"/fault.out

cd "$R"
bash b36start.sh cap; sleep 1
bash b36start.sh C
echo "[$TAG] VAXC booting..."
for i in $(seq 1 120); do
    grep -qa 'now a VAXcluster member -- system VAXC' "$R/VAXC.console.log" 2>/dev/null && break
    sleep 2
done
grep -qa 'now a VAXcluster member -- system VAXC' "$R/VAXC.console.log" || { echo "[$TAG] FATAL VAXC never formed"; exit 1; }
echo "[$TAG] VAXC formed at $(date -u +%H:%M:%S)"

# rd vms-e88: JOIN ORDER is a variable of the matrix. a-then-b (the original
# rig: OVMXA admitted before OVMXB boots), together (both boot within a second,
# the browser all-at-once shape), b-then-a (OVMXB first, OVMXA 5 s later).
ORDER="${JOIN_ORDER:-a-then-b}"
echo "$ORDER" > "$R/join-order"
start_a() { ART_ROOT="$ART" DUR=400 bash b36start.sh A; }
wait_a() {
    for i in $(seq 1 150); do
        grep -qa 'this node is now a VAXcluster member' "$R/OVMXA.console.log" 2>/dev/null && break
        sleep 2
    done
    grep -qa 'this node is now a VAXcluster member' "$R/OVMXA.console.log" || { echo "[$TAG] FATAL OVMXA never joined"; exit 1; }
    echo "[$TAG] OVMXA MEMBER at $(date -u +%H:%M:%S)"
}
if [ "$ORDER" = "a-then-b" ]; then
    start_a; wait_a
elif [ "$ORDER" = "together" ]; then
    start_a; sleep 1
fi
tc qdisc replace dev tapBx root netem delay $JIT distribution normal
: > "$R/OVMXB.console.log"
# THE FAULT IS A STALLED GUEST, NOT A DARK WIRE (rd vms-8c54). See stall.sh's
# own header for why those are different faults; the live regression was a
# visitor's machine starving the emulator, and a netem blackout cannot produce
# it because a blackout DESTROYS the frames a stalled guest merely DEFERS.
if [ "${NOFAULT:-0}" = "1" ]; then
    echo "NOFAULT: no stall armed (jitter only)" > "$R/fault.out"
else
    case "${STALL_MODE:-console}" in
    pkt:*)  # rd vms-eb3: armed on the wire, in one window of the transition
        setsid nohup python3 "$R/stallpkt.py" tapBx 1988 "${STALL_MODE#pkt:}" "$STALL_S" \
            > "$R/fault.out" 2>&1 < /dev/null & ;;
    *)
        setsid nohup bash "$R/stall.sh" "${STALL_TAPS:-tapBx}" "$MARK_LOG" "$MARK" "$DELAY" "$STALL_S" \
            > "$R/fault.out" 2>&1 < /dev/null & ;;
    esac
fi
ART_ROOT="$ART" DUR=400 bash b36start.sh B
if [ "$ORDER" = "b-then-a" ]; then
    sleep 5; start_a
fi
echo "[$TAG] OVMXB booting with a ${STALL_S}s GUEST STALL armed at $(date -u +%H:%M:%S)"
