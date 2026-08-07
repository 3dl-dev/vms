#!/bin/sh
#
# rip-media.sh -- make an ISO from OpenVMS Alpha installation media you hold
# (rd vms-e2c).
#
# Run this on ANY machine with an optical drive -- it does not need to be
# workshop, and it does not need anything from this repo but this file. Copy the
# resulting .iso to $LAB/media/ on workshop afterwards.
#
# OpenVMS distribution CDs are ODS-2/ISO-9660 hybrids. Do NOT rip them with a
# tool that "understands" filesystems and rewrites metadata -- mkisofs, genisoimage
# and friends will produce something that boots differently or not at all. A
# raw block copy of the whole device is the only faithful rip, which is what
# this does.
#
# Usage: rip-media.sh [DEVICE] [OUTPUT.ISO]

set -eu

DEV="${1:-/dev/sr0}"
OUT="${2:-openvms-alpha.iso}"

[ -b "$DEV" ] || { echo "FAIL: $DEV is not a block device. Try: lsblk -o NAME,TYPE,LABEL" >&2; exit 1; }

echo "device: $DEV"
blockdev --getsize64 "$DEV" 2>/dev/null | awk '{printf "size:   %.1f MB\n", $1/1048576}' || true
# The volume label is worth recording: OpenVMS media labels carry the version,
# so the rip is self-identifying later when you have three ISOs and no memory.
blkid -o value -s LABEL "$DEV" 2>/dev/null | sed 's/^/label:  /' || true

echo
echo "ripping (raw block copy, no filesystem interpretation)..."
# noerror,sync so a marginal 20-year-old disc yields a usable image with the
# bad sectors zero-filled rather than aborting at the first read error.
dd if="$DEV" of="$OUT" bs=2048 conv=noerror,sync status=progress

echo
sync
ls -la "$OUT"
md5sum "$OUT"

cat <<EOF

=== done.

Copy it to the lab and install:

    scp $OUT workshop:/data/training/vax/alpha/media/
    # then on workshop: point the marked line in
    #   tests/lab-alpha/cfg/alpha1-install.cfg
    # at the ISO, and
    LAB=/data/training/vax/alpha
    CFG=\$LAB/cfg/alpha1-install.cfg \$LAB/node.sh start
    \$LAB/node.sh send 'show device'     # expect DQA1 = the CD
    \$LAB/node.sh send 'boot dqa1'
EOF
