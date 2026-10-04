#!/bin/bash
# af4rig.sh : rd vms-af4 on the rig. VAXC (real V7.3) + OVMXA members; OVMXB joins;
# then OVMXB's QEMU is SIGKILLed (an unclean teardown: no departure, no last gasp)
# and the SAME node (SCSNODE/SCSSYSTEMID) is booted again at once, inside the
# survivors' RECNXINTERVAL. What do the survivors do with the old incarnation, and
# is the new one admitted, refused, or held off -- against the real-VMS oracle?
set -u
R=/lab/run-b98rig
export ACCEL="${ACCEL:-tcg,thread=multi}" ACCEL_CPU="${ACCEL_CPU:--cpu max}"
cd $R
ART_ROOT=$R/art/b1 DUR=900 bash b36start.sh B
for i in $(seq 1 90); do grep -qa 'this node is now a VAXcluster member' OVMXB.console.log && break; sleep 2; done
echo "B MEMBER $(date +%s.%N)"
sleep 45
P=$(ps -eo pid,args | grep qemu-system-x86_64 | grep 'ifname=tapBe' | grep -v grep | awk '{print $1}')
echo "KILL B qemu $P at $(date +%s.%N)"
kill -9 $P
sleep 3
cp OVMXB.console.log OVMXB-incarnation1.console.log
echo "REBOOT B at $(date +%s.%N)"
ART_ROOT=$R/art/b1 DUR=600 bash b36start.sh B
for i in $(seq 1 150); do grep -qa 'this node is now a VAXcluster member' OVMXB.console.log && break; sleep 2; done
echo "B2 MEMBER-or-timeout $(date +%s.%N)"
sleep 60
echo "END $(date +%s.%N)"
