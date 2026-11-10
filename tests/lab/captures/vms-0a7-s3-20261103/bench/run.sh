#!/bin/sh
# One S3 bench run from pristine disks (copied from the S1 run-5 bench).
# usage: run.sh <image> <outdir>
# env: W (work dir, default /tmp/s3r1-lab: nodec.dsk.gz, secret/paks3.json,
#      vaxw/{vax.ini (= bench/vaxc.ini), data/nvram.bin}), REPO, VAX_PW DN_PW OVMX_PW BAD_PW.
set -eu
IMG=$1 OUT=$2
W=${W:-/tmp/s3r1-lab}; REPO=${REPO:-$(cd "$(dirname "$0")/../../../../.." && pwd)}
C=s3r1-extract
docker rm -f $C >/dev/null 2>&1 || true
docker create --name $C "$IMG" >/dev/null
mkdir -p "$W/rel" "$OUT"
for f in vmlinuz initramfs-ovmx-slim.cpio.gz ovmx-distrib.img; do docker cp -L "$C:/boot/$f" "$W/rel/$f"; done
docker rm -f $C >/dev/null
"$REPO/tools/cluster-web-demo/inject-decnet-config.sh" "$W/rel/ovmx-distrib.img" "$W/ovmxa.img" --scsnode OVMXA
sha256sum "$W/rel/"* "$W/ovmxa.img" | sed "s|$W/||" > "$OUT/artifacts.sha256"
gunzip -c "$W/nodec.dsk.gz" | cp --sparse=always /dev/stdin "$W/vaxw/data/d0.dsk"
REL="$W/rel" W="$W" python3 "$(dirname "$0")/s3_bench.py" "$OUT" "$W/ovmxa.img"
