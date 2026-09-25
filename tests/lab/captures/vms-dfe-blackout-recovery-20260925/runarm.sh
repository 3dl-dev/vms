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
R=/lab/run-dfe
BLACKOUT_S="${BLACKOUT_S:-45}"
JIT="${JIT:-80ms 40ms}"
MARK='%CNXMAN, (the cluster opened the VMS[$]VAXcluster connection to this node|a cluster member opened a VMS[$]VAXcluster connection to this node|the executive already holds this pair.s VMS[$]VAXcluster connection|adopting the VMS[$]VAXcluster connection)'
DELAY="${TRIG_DELAY:-0}"

# Kill the previous arm's emulators BY PID. (pkill -f would also match this
# script's own command line, and SIMH ignores a plain SIGTERM in its console
# loop -- both were observed here.)
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|[.]/vax vax[.]ini|tcpdum[p] -i brdfe' | grep -v grep | awk '{print $1}'); do
    kill -9 "$p" 2>/dev/null
done
sleep 5
tc qdisc del dev tapBdfe root 2>/dev/null
mkdir -p "$R/$TAG"
cp /lab/cluster-demo-nodeC-v73/data/d0.dsk   "$R/nodeC/data/d0.dsk"
cp /lab/cluster-demo-nodeC-v73/data/nvram.bin "$R/nodeC/data/nvram.bin"
rm -f "$R"/*.console.log "$R"/*.caps "$R"/dfe.pcap "$R"/*.drv.out "$R"/blackout.out

cd "$R"
bash dfestart.sh cap; sleep 1
bash dfestart.sh C
echo "[$TAG] VAXC booting..."
for i in $(seq 1 120); do
    grep -qa 'now a VAXcluster member -- system VAXC' "$R/VAXC.console.log" 2>/dev/null && break
    sleep 2
done
grep -qa 'now a VAXcluster member -- system VAXC' "$R/VAXC.console.log" || { echo "[$TAG] FATAL VAXC never formed"; exit 1; }
echo "[$TAG] VAXC formed at $(date -u +%H:%M:%S)"

ART_ROOT="$ART" DUR=400 bash dfestart.sh A
for i in $(seq 1 120); do
    grep -qa 'this node is now a VAXcluster member' "$R/OVMXA.console.log" 2>/dev/null && break
    sleep 2
done
grep -qa 'this node is now a VAXcluster member' "$R/OVMXA.console.log" || { echo "[$TAG] FATAL OVMXA never joined"; exit 1; }
echo "[$TAG] OVMXA MEMBER at $(date -u +%H:%M:%S)"

tc qdisc replace dev tapBdfe root netem delay $JIT distribution normal
: > "$R/OVMXB.console.log"
if [ "${NOFAULT:-0}" = "1" ]; then
    echo "NOFAULT: no blackout armed (jitter only)" > "$R/blackout.out"
else
    setsid nohup bash "$R/blackout.sh" tapBdfe "$R/OVMXB.console.log" "$MARK" "$DELAY" "$BLACKOUT_S" \
        > "$R/blackout.out" 2>&1 < /dev/null &
fi
ART_ROOT="$ART" DUR=400 bash dfestart.sh B
echo "[$TAG] OVMXB booting with blackout armed (${BLACKOUT_S}s) at $(date -u +%H:%M:%S)"
