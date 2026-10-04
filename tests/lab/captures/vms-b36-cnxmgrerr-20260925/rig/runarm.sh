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
R=/lab/run-b36
BLACKOUT_S="${BLACKOUT_S:-45}"
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
MARK_LOG="${MARK_LOG:-$R/OVMXB.console.log}"
MARK="${MARK:-%CNXMAN, (the cluster opened the VMS[$]VAXcluster connection to this node|a cluster member opened a VMS[$]VAXcluster connection to this node|the executive already holds this pair.s VMS[$]VAXcluster connection|adopting the VMS[$]VAXcluster connection)}"
DELAY="${TRIG_DELAY:-0}"

# Kill the previous arm's emulators BY PID. (pkill -f would also match this
# script's own command line, and SIMH ignores a plain SIGTERM in its console
# loop -- both were observed here.)
# SCOPED TO THIS RUN ROOT. An unscoped pattern would also match the real-VMS
# oracle lab running in the same pod (its own nodedrv.py + ./vax vax.ini), and
# killing another rig's emulators is how a lab eats itself.
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i brb36' \
           | grep -v grep | grep -E "$R|brb36" | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do
    kill -9 "$p" 2>/dev/null
done
sleep 5
tc qdisc del dev tapBb36 root 2>/dev/null
mkdir -p "$R/$TAG"
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk "$R/nodeC/data/d0.dsk"
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/nvram.bin "$R/nodeC/data/nvram.bin"
rm -f "$R"/*.console.log "$R"/*.caps "$R"/b36.pcap "$R"/*.drv.out "$R"/blackout.out

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

ART_ROOT="$ART" DUR=400 bash b36start.sh A
for i in $(seq 1 120); do
    grep -qa 'this node is now a VAXcluster member' "$R/OVMXA.console.log" 2>/dev/null && break
    sleep 2
done
grep -qa 'this node is now a VAXcluster member' "$R/OVMXA.console.log" || { echo "[$TAG] FATAL OVMXA never joined"; exit 1; }
echo "[$TAG] OVMXA MEMBER at $(date -u +%H:%M:%S)"

tc qdisc replace dev tapBb36 root netem delay $JIT distribution normal
: > "$R/OVMXB.console.log"
if [ "${NOFAULT:-0}" = "1" ]; then
    echo "NOFAULT: no blackout armed (jitter only)" > "$R/blackout.out"
else
    setsid nohup bash "$R/blackout.sh" tapBb36 "$MARK_LOG" "$MARK" "$DELAY" "$BLACKOUT_S" \
        > "$R/blackout.out" 2>&1 < /dev/null &
fi
ART_ROOT="$ART" DUR=400 bash b36start.sh B
echo "[$TAG] OVMXB booting with blackout armed (${BLACKOUT_S}s) at $(date -u +%H:%M:%S)"
