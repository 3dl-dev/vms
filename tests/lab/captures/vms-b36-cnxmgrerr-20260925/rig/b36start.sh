#!/bin/bash
# b36start.sh - start capture / VAXC / node A|B on the rd vms-dfe bridge.
set -u
R=/lab/run-b36
A="${ART_ROOT:-$R/art}"
case "${1:-}" in
  cap) setsid nohup tcpdump -i brb36 -s0 -w "$R/b36.pcap" 'ether proto 0x6007' > "$R/tcpdump.out" 2>&1 < /dev/null & ;;
  C)   cd $R && setsid nohup python3 $R/nodedrv.py $R/nodeC $R/VAXC.console.log --boot "B DUA0" > $R/nodeC.drv.out 2>&1 < /dev/null & ;;
  A)   setsid nohup env ART=$A OUT=$R/OVMXA.console.log TAP=tapAb36 \
          MAC=52:54:00:00:df:0a NAME=OVMXA DUR="${DUR:-1200}" \
          SCSNODE=OVMXA SCSSYSID=1987 VOTES=1 EXPVOTES=2 \
          bash $R/b36node.sh > $R/A.drv.out 2>&1 < /dev/null & ;;
  B)   setsid nohup env ART=$A OUT=$R/OVMXB.console.log TAP=tapBb36 \
          MAC=52:54:00:00:df:0b NAME=OVMXB DUR="${DUR:-900}" \
          SCSNODE=OVMXB SCSSYSID=1988 VOTES=1 EXPVOTES=3 \
          bash $R/b36node.sh > $R/B.drv.out 2>&1 < /dev/null & ;;
  *) echo "usage: b36start.sh cap|C|A|B" >&2; exit 2;;
esac
sleep 1; echo "started $1 (art=$A)"
