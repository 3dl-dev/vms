#!/bin/bash
# e88 oracle lab: three real V7.3 disk nodes on a private bridge in THIS pod's netns.
set -eu
D=/lab/k8s-labs/e88lab
mkdir -p $D/data $D/logs
for d in d0 d1; do [ -f $D/data/$d.dsk ] || cp --sparse=always /lab/cluster/data/$d.dsk.3node-golden.bak $D/data/$d.dsk; done
ip link add bre88 type bridge 2>/dev/null || true
ip link set bre88 up
for n in 1 2 3; do
  ip tuntap add dev tape$n mode tap 2>/dev/null || true
  ip link set tape$n master bre88; ip link set tape$n up
  nd=$D/vax$n; mkdir -p $nd/data
  [ -f $nd/data/nvram.bin ] || { if [ -f /lab/cluster/vax$n/data/nvram.bin ]; then cp /lab/cluster/vax$n/data/nvram.bin $nd/data/; else cp /lab/cluster/data/nvram.bin $nd/data/; fi; }
  ln -sf /usr/local/bin/vax $nd/vax
  cat > $nd/vax.ini <<EOT
attach nvr $nd/data/nvram.bin
set cpu conhalt
set cpu 128m
set idle=vms
set rl disable
set rq enable
set rqb disable
set rqc disable
set rqd disable
set rq0 ra92
set rq1 ra92
set rq2 cdrom
set rq3 cdrom
attach rq0 $D/data/d0.dsk
attach rq1 $D/data/d1.dsk
attach -r rq2 /lab/media/openvms073.iso
attach -r rq3 /lab/media/openvms-internet-product-suite-v11.iso
do local.ini
b
EOT
  printf 'set xq enable\nat xq tap:tape%s\natt dz %s,speed=*32\n' $n $((2100+n)) > $nd/local.ini
done
ip -brief link | grep -E "bre88|tape"
