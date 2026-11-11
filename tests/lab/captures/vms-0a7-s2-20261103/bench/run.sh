#!/bin/sh
# One bench run from pristine disks. usage: run.sh <outdir>   (env: passwords)
set -eu
W=${W:-/tmp/vms0a7s2-lab}
gunzip -c "$W/nodec.dsk.gz" > "$W/vaxw/data/d0.dsk"
cp "$W/ovmxa.pristine.img" "$W/ovmxa.img"
rm -f "$W/ovmxa.pcap"
mkdir -p "$1"
exec python3 "$(dirname "$0")/sethost_bench.py" "$1"
