#!/bin/bash
# cn3start.sh - start capture / VAXC / node A|B, fully detached. $2 = art root.
set -u
R=/lab/run-1ac
A="${ART_ROOT:-$R/art}"
case "${1:-}" in
  cap) setsid nohup tcpdump -i br1ac -s0 -w "$R/cn3.pcap" 'ether proto 0x6007' > "$R/tcpdump.out" 2>&1 < /dev/null & ;;
  C)   cd $R && setsid nohup python3 $R/nodedrv.py $R/nodeC $R/VAXC.console.log --boot "B DUA0" > $R/nodeC.drv.out 2>&1 < /dev/null & ;;
  A)   setsid nohup env ART=$A/A OUT=$R/OVMXA.console.log TAP=tapA1ac \
          MAC=52:54:00:00:4f:0a NAME=OVMXA DUR="${DUR:-1200}" \
          SCSNODE=OVMXA SCSSYSID=1987 VOTES=1 EXPVOTES=2 \
          bash $R/cn3node.sh > $R/A.drv.out 2>&1 < /dev/null & ;;
  B)   setsid nohup env ART=$A/B OUT=$R/OVMXB.console.log TAP=tapB1ac \
          MAC=52:54:00:00:4f:0b NAME=OVMXB DUR="${DUR:-900}" \
          SCSNODE=OVMXB SCSSYSID=1988 VOTES=1 EXPVOTES=3 \
          bash $R/cn3node.sh > $R/B.drv.out 2>&1 < /dev/null & ;;
  *) echo "usage: cn3start.sh cap|C|A|B" >&2; exit 2;;
esac
sleep 1; echo "started $1 (art=$A)"
