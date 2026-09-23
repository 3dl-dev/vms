#!/bin/bash
# stage_cluster_group.sh <in-initramfs.cpio.gz> <out-initramfs.cpio.gz> <group#> [password]
#
# Put a CLUSTER_AUTHORIZE.DAT for a NAMED cluster group into an already-built
# OVMX boot initramfs (rd vms-b34).
#
# WHY THIS EXISTS. The cluster group number is what the LAVC HELLO multicast
# address is BUILT from -- AB-00-04-01-<LE16(group + 0x100)>
# (src/kernel-core/vms_cluster_codec_hello.c vms_cluster_hello_mcast_build; the
# +0x100 is VMS's, grounded on three real-VMS oracles, rd vms-147) --
# so two nodes with different group numbers are on different multicast groups
# and are simply not on the same cluster's wire. It reaches the executive from
# /etc/ovmx/cluster_authorize.dat (cluster_authorize_read() -> SYSGEN_LOAD
# args.auth_group), and distro/Dockerfile.bootable stages that file only when
# built with --build-arg CLUSTER_AUTH_GROUP=<n>; an ordinary build (and every
# published release artifact) ships an EMPTY /etc/ovmx and therefore group 0.
#
# WHICH NUMBER TO PASS (rd vms-147). The lab VAX cluster is group 1 -- VMS
# prints that itself: SYSMAN> CONFIGURATION SHOW CLUSTER_AUTHORIZATION on VAX1
# reads "Cluster group number: 1" / "Multicast address: AB-00-04-01-01-01".
# Earlier lab runs passed 257 here and joined anyway: OVMX's derivation was
# LE16(group) with no bias, so 257 produced group 1's address -- two errors
# that cancelled. With the derivation corrected, 257 now puts a node on
# AB-00-04-01-01-02 (a different cluster's address) and the lab join would go
# silent. Pass the cluster's REAL group number, read from VMS, not one
# back-derived from a multicast address.
#
# Measured on lab-2 vaxlab-4 (2026-09-20): a booted V0.7 release node ran the
# whole join window transmitting 225 frames to AB-00-04-01-00-00 while the real
# VMS V7.3 cluster transmitted 180 to AB-00-04-01-01-01 (group 1). Neither
# received one frame of the other's: `SHOW CLUSTER/LOCAL_PORTS` read
# `channels 0, circuits 0, rx 0`. Requiring a 30-minute image rebuild to put a
# lab run on the lab cluster's own group is what made that cost a whole run, so
# the group becomes a run parameter here instead.
#
# It authors NO bytes of its own: the record is written by the project's own
# tools/cluster/mk_cluster_authorize.c, which calls the SAME
# cluster_authorize_write() the runtime reader parses. This script only unpacks,
# drops that file in, and repacks.
#
# Leaves the input untouched; writes a NEW initramfs. Standard tools only
# (cpio, gzip, cc); no host installs.
set -eu

IN="${1:?usage: stage_cluster_group.sh <in.cpio.gz> <out.cpio.gz> <group#> [password]}"
OUT="${2:?output initramfs path required}"
GROUP="${3:?cluster group number required}"
PASSWORD="${4:-}"

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${REPO:-$(cd "$HERE/../../.." && pwd)}"
MKSRC="$REPO/tools/cluster/mk_cluster_authorize.c"
AUTHHDR="$REPO/src/libvms/include"

[ -f "$IN" ]    || { echo "stage_cluster_group: FATAL -- no such initramfs: $IN" >&2; exit 2; }
[ -f "$MKSRC" ] || { echo "stage_cluster_group: FATAL -- missing $MKSRC" >&2; exit 2; }
case "$GROUP" in ''|*[!0-9]*) echo "stage_cluster_group: FATAL -- group must be a number, got '$GROUP'" >&2; exit 2 ;; esac
[ "$GROUP" -ge 1 ] && [ "$GROUP" -le 65535 ] || {
    echo "stage_cluster_group: FATAL -- group $GROUP out of range (1..65535); 0 is 'no group configured', which is what this tool exists to avoid" >&2
    exit 2
}

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || { echo "stage_cluster_group: FATAL -- no C compiler ($CC) to build the record writer" >&2; exit 2; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# 1. Build the project's own record writer and author the record with it.
"$CC" -I"$AUTHHDR" "$MKSRC" -o "$WORK/mk_cluster_authorize" \
    || { echo "stage_cluster_group: FATAL -- could not build mk_cluster_authorize" >&2; exit 2; }

# 2. Unpack, drop the record at the path cluster_authorize_path() reads, repack.
mkdir -p "$WORK/root"
gzip -dc "$IN" | ( cd "$WORK/root" && cpio -idm --quiet )
mkdir -p "$WORK/root/etc/ovmx"
"$WORK/mk_cluster_authorize" "$WORK/root/etc/ovmx/cluster_authorize.dat" "$GROUP" "$PASSWORD" \
    || { echo "stage_cluster_group: FATAL -- mk_cluster_authorize failed" >&2; exit 2; }
[ -s "$WORK/root/etc/ovmx/cluster_authorize.dat" ] \
    || { echo "stage_cluster_group: FATAL -- the record was not written" >&2; exit 2; }

( cd "$WORK/root" && find . | cpio -o -H newc --quiet ) | gzip -9 > "$OUT"
[ -s "$OUT" ] || { echo "stage_cluster_group: FATAL -- repack produced nothing" >&2; exit 2; }

# 3. READ IT BACK OUT OF THE ARTIFACT THAT WILL ACTUALLY BOOT. A staging step
#    that is not verified against its own output is how a silent group-0 boot
#    happens in the first place.
VER="$(mktemp -d)"
gzip -dc "$OUT" | ( cd "$VER" && cpio -idm --quiet ) 2>/dev/null
if ! cmp -s "$VER/etc/ovmx/cluster_authorize.dat" "$WORK/root/etc/ovmx/cluster_authorize.dat"; then
    rm -rf "$VER"
    echo "stage_cluster_group: FATAL -- read-back of $OUT does not match the authored record" >&2
    exit 2
fi
rm -rf "$VER"
echo "stage_cluster_group: group $GROUP staged into $(basename "$OUT") (read-back verified)"
