#!/bin/sh
# build.sh — build the alpha-dec-vms cross toolchain image and extract the
# compiler + binutils to a local dir (default: ./out). Build/oracle tooling;
# see README.md and the Dockerfile header (Rule-9-clean, not a runtime target).
#
#   tools/cross-alpha-vms/build.sh [OUTDIR]
#
# Produces OUTDIR/{alpha-dec-vms-gcc, cc1, alpha-dec-vms-as, alpha-dec-vms-ld,
# alpha-dec-vms-objdump, ...}: a working alpha-dec-vms cross compiler that emits
# genuine EVAX objects for gap-probing OVMX LINK.EXE.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-"$HERE/out"}
IMG=ovmx-cross-alpha-vms
PREFIX=/opt/cross-alpha-vms

echo "== build $IMG (binutils + gcc cc1, alpha-dec-vms) =="
docker build -t "$IMG" "$HERE"

echo "== extract the toolchain to $OUT =="
mkdir -p "$OUT"
cid=$(docker create "$IMG")
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
# the whole prefix (bin/ + libexec/ carry the driver, as/ld, and cc1)
docker cp "$cid:$PREFIX/." "$OUT/"
echo "== done. try: $OUT/bin/alpha-dec-vms-gcc -S -mpointer-size=64 hello.c =="
"$OUT/bin/alpha-dec-vms-gcc" -dumpmachine 2>/dev/null || true
