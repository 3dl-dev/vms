#!/bin/sh
# Extract the S2 bootable release set from the runner image and inject DECnet
# (OVMXA, 1.963). usage: prep_ovmx.sh <image> <workdir> <repo>
set -eu
IMG=$1 W=$2 REPO=$3
C=vms0a7s2-extract
docker rm -f $C >/dev/null 2>&1 || true
docker create --name $C "$IMG" >/dev/null
mkdir -p "$W/rel"
for f in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img; do
  docker cp -L "$C:/boot/$f" "$W/rel/$f"
done
docker rm -f $C >/dev/null
"$REPO/tools/cluster-web-demo/inject-decnet-config.sh" "$W/rel/ovmx-distrib.img" "$W/ovmxa.img" --scsnode OVMXA
sha256sum "$W/rel/"* "$W/ovmxa.img"
