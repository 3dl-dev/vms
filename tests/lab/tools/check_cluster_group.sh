#!/bin/bash
# check_cluster_group.sh <initramfs.cpio.gz> [required-group]
#
# THE PRECONDITION A LAB FIRE DOES NOT SURVIVE WITHOUT (rd vms-daa): the
# initramfs about to be booted must carry a NON-ZERO cluster group.
#
# WHY. The cluster group number is what the LAVC HELLO multicast address is
# BUILT from -- AB-00-04-01-<LE16(group + 0x100)>, derived in
# src/kernel-core/vms_cluster_codec_hello.c (vms_cluster_hello_mcast_build) and
# nowhere else -- so two nodes with different groups are not on the same
# cluster's wire at all. It reaches the executive from
# /etc/ovmx/cluster_authorize.dat, and an ORDINARY image build ships an empty
# /etc/ovmx, i.e. GROUP 0: the node transmits HELLOs into a multicast group
# nobody listens to, receives none, and never joins. MEASURED on lab-2 vaxlab-4
# (2026-09-20, the note in stage_cluster_group.sh): a booted release node sent
# 225 frames to AB-00-04-01-00-00 while the real V7.3 cluster sent 180 to
# AB-00-04-01-01-01, and SHOW CLUSTER/LOCAL_PORTS read `channels 0, circuits 0,
# rx 0` on both sides.
#
# AND IT IS SILENT. Nothing fails, nothing crashes, no console line says why --
# the fire just produces a capture with no join in it. ovmx_fire.sh checks the
# build's provenance and the frame-conformance gate but never checked this, so
# a group-0 initramfs cost a whole lab run (rd vms-daa). This is that check.
#
# WHAT IT DOES. Unpacks ONLY the record out of the initramfs that will boot
# (never a sibling file, never the build tree's copy), reads the group with the
# PROJECT'S OWN reader -- tools/cluster/show_cluster_authorize.c calling the
# same cluster_authorize_read() the executive calls -- and refuses:
#
#   * no /etc/ovmx/cluster_authorize.dat at all   -> group 0
#   * a record the reader cannot read             -> the executive reads
#                                                    nothing there either
#   * group 0                                     -> the void
#   * group != <required-group>, when one is given (the lab VAX cluster is
#     group 1; VMS prints it itself -- SYSMAN> CONFIGURATION SHOW
#     CLUSTER_AUTHORIZATION)
#
# Prints the group it found on success, so a transcript records which cluster's
# wire the fire was on. Standard tools only (cpio, gzip, cc); no host installs,
# nothing written outside a temp dir.
set -u

IMG="${1:?usage: check_cluster_group.sh <initramfs.cpio.gz> [required-group]}"
WANT="${2:-}"
HERE=$(cd "$(dirname "$0")" && pwd)
# OVMX_ROOT lets a caller that is not sitting in the tree (the gate's own
# negative control runs a COPY of this script out of a temp dir) say where the
# tree is; otherwise it is three levels up, where this script lives.
ROOT="${OVMX_ROOT:-$(cd "$HERE/../../.." && pwd)}"
REC_PATH=etc/ovmx/cluster_authorize.dat
SRC="$ROOT/tools/cluster/show_cluster_authorize.c"
AUTHHDR="$ROOT/src/libvms/include"
CC="${CC:-cc}"

say()  { echo "check_cluster_group: $*"; }
die()  { echo "check_cluster_group: REFUSED -- $*" >&2; exit 1; }
fatal(){ echo "check_cluster_group: FATAL -- $*" >&2; exit 2; }

[ -f "$IMG" ] || fatal "no such initramfs: $IMG"
[ -f "$SRC" ] || fatal "missing the record reader $SRC"
command -v "$CC" >/dev/null 2>&1 || fatal "no C compiler ($CC) to build the record reader"
case "$WANT" in
	''|*[0-9]) : ;;
	*) fatal "required-group must be a number, got '$WANT'" ;;
esac

WORK=$(mktemp -d) || fatal "mktemp failed"
trap 'rm -rf "$WORK"' EXIT

"$CC" -I"$AUTHHDR" "$SRC" -o "$WORK/show_cluster_authorize" 2>"$WORK/cc.err" \
	|| { cat "$WORK/cc.err" >&2; fatal "could not build the record reader"; }

# Only the one record comes out of the image: a fire's initramfs is not a
# directory to go rummaging in.
mkdir -p "$WORK/root"
gzip -dc "$IMG" 2>/dev/null | ( cd "$WORK/root" && cpio -idm --quiet "$REC_PATH" ) 2>/dev/null
if [ ! -s "$WORK/root/$REC_PATH" ]; then
	die "$(basename "$IMG") carries no $REC_PATH, so the node boots with cluster group 0 and joins nothing (stage it with tests/lab/tools/stage_cluster_group.sh)"
fi

GOT=$("$WORK/show_cluster_authorize" "$WORK/root/$REC_PATH") \
	|| die "$(basename "$IMG")'s $REC_PATH is not a record the executive can read -- it would boot with cluster group 0"
case "$GOT" in
	''|*[!0-9]*) fatal "the reader printed '$GOT', which is not a group number" ;;
esac

[ "$GOT" -ne 0 ] || die "$(basename "$IMG") carries cluster group 0 -- the node would transmit HELLOs to AB-00-04-01-00-00 and join nothing"

if [ -n "$WANT" ] && [ "$GOT" -ne "$WANT" ]; then
	die "$(basename "$IMG") carries cluster group $GOT, but this fire requires group $WANT -- a different group is a different cluster's wire"
fi

say "cluster group $GOT (read out of the initramfs that will boot)"
exit 0
