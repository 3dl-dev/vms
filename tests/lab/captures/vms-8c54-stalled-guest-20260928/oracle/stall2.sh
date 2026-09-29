#!/bin/bash
# THE ORACLE for rd vms-8c54: a REAL OpenVMS VAX V7.3 MEMBER stalled INSIDE the
# survivor's reconnect window.
#
# vax1 and vax2 are two real V7.3 nodes in one VAXcluster on br0. This SIGSTOPs
# vax2's emulator for <secs> seconds -- the same fault a visitor's starved
# machine applies to an OVMX guest -- and SIGCONTs it. Nothing on the wire is
# touched. RECNXINTERVAL is 20 s, the PE listen timeout ~8 s, so a 10 s stall
# has the survivor close the virtual circuit and KEEP the CSB: vax2 wakes while
# vax1 has NOT given it up. What vax2 puts on the wire on wake is the answer to
# "what should OVMX do here", and what vax1 does with it is whether real VMS
# survives it.
set -u
S="${1:-10}"
P2=$(ps -eo pid,args | grep "[v]ax vax.ini" | while read p r; do \
        [ "$(readlink /proc/$p/cwd)" = "/lab/k8s-labs/s8lab/vax2" ] && echo "$p"; done)
[ -z "$P2" ] && { echo "vax2 not running"; exit 1; }
echo "ORACLE STALL vax2 pid $P2 for ${S}s"
echo "STOP  epoch=$(date +%s.%N) utc=$(date -u +%H:%M:%S.%3N)"
kill -STOP "$P2"; sleep "$S"; kill -CONT "$P2"
echo "CONT  epoch=$(date +%s.%N) utc=$(date -u +%H:%M:%S.%3N)"
