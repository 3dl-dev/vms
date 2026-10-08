#!/bin/bash
# runP.sh <artdir> <tag> -- rd vms-9484 lab: an OVMX member with the HIGHEST SCSSYSTEMID coordinates an admission while a REAL VAX is the other member.
#   1. VAXC (real V7.3, SCSSYSTEMID 1989) founds alone.
#   2. OVMXA (SCSSYSTEMID 1995 -- now the highest member) joins.
#   3. OVMXB (1988) joins: VMS's coordinator is the highest-SCSSYSTEMID member,
#      so OVMXA coordinates its admission with the real VAX as the other member.
# No netem, no stall. Graded from the consoles and the wire.
set -u
ART="$1"; TAG="$2"; R=/lab/run-dlm2
for p in $(ps -eo pid,args | grep -E 'qemu-system-x86_64|nodedrv[.]py|tcpdum[p] -i brdl2r' | grep -v grep | grep -E "$R/|brdl2r" | awk '{print $1}'); do kill -9 "$p" 2>/dev/null; done
for p in $(ls -l /proc/*/cwd 2>/dev/null | grep "$R/nodeC" | sed 's|.*/proc/\([0-9]*\)/cwd.*|\1|'); do kill -9 "$p" 2>/dev/null; done
sleep 5
tc qdisc del dev tapB2 root 2>/dev/null; tc qdisc del dev tapA2 root 2>/dev/null
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/d0.dsk "$R/nodeC/data/d0.dsk"
cp --sparse=always /lab/cluster-demo-nodeC-v73/data/nvram.bin "$R/nodeC/data/nvram.bin"
rm -f "$R"/*.console.log "$R"/*.caps "$R"/s8.pcap "$R"/*.drv.out "$R"/fault.out
cd "$R"
bash b36start.sh cap; sleep 1
node() { # name tap mac sysid votes ev
  setsid nohup env ART=$ART OUT=$R/$1.console.log TAP=$2 MAC=$3 NAME=$1 DUR=600 \
     SCSNODE=$1 SCSSYSID=$4 VOTES=$5 EXPVOTES=$6 bash $R/b36node.sh > $R/$1.drv.out 2>&1 < /dev/null & }
waitfor() { for i in $(seq 1 $3); do grep -qa "$2" "$R/$1" 2>/dev/null && return 0; sleep 2; done; return 1; }
bash b36start.sh C
waitfor VAXC.console.log 'now a VAXcluster member -- system VAXC' 120 || { echo "[$TAG] FATAL VAXC never formed"; exit 1; }
echo "[$TAG] VAXC formed at $(date -u +%H:%M:%S)"
node OVMXA tapA2 52:54:00:00:df:0a 1995 1 2
if waitfor OVMXA.console.log 'this node is now a VAXcluster member' 150; then echo "[$TAG] OVMXA member at $(date -u +%H:%M:%S)"; else echo "[$TAG] FATAL OVMXA never joined"; exit 1; fi
sleep 20
node OVMXB tapB2 52:54:00:00:df:0b 1988 1 3
if waitfor OVMXB.console.log 'this node is now a VAXcluster member' 150; then echo "[$TAG] OVMXB member at $(date -u +%H:%M:%S)"; else echo "[$TAG] OVMXB NOT a member after 300 s"; fi
sleep 90
for p in $(ps -eo pid,args | grep 'tcpdum[p] -i brdl2r' | awk '{print $1}'); do kill "$p" 2>/dev/null; done
sleep 2
D="$R/runs/$TAG"; mkdir -p "$D"
cp "$R"/OVMXA.console.log "$R"/OVMXB.console.log "$R"/VAXC.console.log "$D"/ 2>/dev/null
gzip -c "$R"/s8.pcap > "$D"/s8.pcap.gz
bc=$(grep -ac "BUG CHECK\|BUGCHECK\|bugcheck" "$D"/VAXC.console.log)
echo "[$TAG] DONE bugchecks=$bc"
