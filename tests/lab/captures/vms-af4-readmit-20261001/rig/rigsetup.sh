#!/bin/bash
# rigsetup.sh : /lab/run-af4rig from the run-e88 rig scripts (own run root), a fourth OVMX
# node (D = OVMXC) for removal specimens, QEMU accel as a knob, and the private bridge.
set -eu
R=/lab/run-af4rig
mkdir -p $R/nodeC/data $R/art $R/runs
for f in b36start.sh b36node.sh matrix.sh runarm.sh stall.sh grade3.sh nodedrv.py startL.sh; do
  sed -e "s|/lab/run-e88|$R|g" /lab/run-e88/$f > $R/$f
done
cp /lab/run-e88/nodeC/vax.ini $R/nodeC/vax.ini
ln -sf /usr/local/bin/vax $R/nodeC/vax
sed -i 's|-accel kvm -cpu host|-accel ${ACCEL:-kvm} ${ACCEL_CPU:--cpu host}|' $R/b36node.sh
# Votes/expected are knobs too (default = the run-e88 values).
sed -i 's|VOTES=1 EXPVOTES=2|VOTES=${A_VOTES:-1} EXPVOTES=${A_EXP:-2}|; s|VOTES=1 EXPVOTES=3|VOTES=${B_VOTES:-1} EXPVOTES=${B_EXP:-3}|' $R/b36start.sh
sed -i 's@^  \*) echo "usage: b36start.sh cap|C|A|B" >&2; exit 2;;@  D)   setsid nohup env ART=$A OUT=$R/OVMXC.console.log TAP=tapDe \\\n          MAC=52:54:00:00:df:0c NAME=OVMXC DUR="${DUR:-900}" \\\n          SCSNODE=OVMXC SCSSYSID=1990 VOTES=${D_VOTES:-0} EXPVOTES=${D_EXP:-3} \\\n          bash $R/b36node.sh > $R/D.drv.out 2>\&1 < /dev/null \& ;;\n  *) echo "usage: b36start.sh cap|C|A|B|D" >\&2; exit 2;;@' $R/b36start.sh
ip link add bre88r type bridge 2>/dev/null || true
ip link set bre88r up
for t in tapAe tapBe tapCe tapDe; do
  ip tuntap add dev $t mode tap 2>/dev/null || true
  ip link set $t master bre88r; ip link set $t up
done
grep -n "D)\|VOTES" $R/b36start.sh
