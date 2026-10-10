#!/bin/bash
# test_vmsfs_master_headroom.sh - a mastered ODS-2 volume of a given size has
# file headers to spare for the system it boots (rd vms-59da).
#
# The header cap used to be the mastered tree plus a quarter: the booted
# distribution disk ran out of headers after ~25 new files, and every later
# file creation failed. A 16 MB volume of a one-file tree must allow at least
# one header per 64 blocks of volume (512) beyond the tree.
#
# Usage: test_vmsfs_master_headroom.sh <vmsfs_master-binary>
set -u
MASTER="${1:?usage: $0 <vmsfs_master-binary>}"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src/A"; echo hi > "$WORK/src/A/X.TXT"
"$MASTER" --ods2 master "$WORK/v.img" OVMXHDR "$WORK/src" 16 >/dev/null \
    || { echo "FAIL: master exited non-zero"; exit 1; }
mf=$(python3 -c "import struct,sys;d=open(sys.argv[1],'rb').read(1024);print(struct.unpack('<I',d[512+28:512+32])[0])" "$WORK/v.img")
lbl=$(python3 -c "import sys;d=open(sys.argv[1],'rb').read(1024);print(d[512+496:512+506].decode())" "$WORK/v.img")
[ "$lbl" = "DECFILE11B" ] || { echo "FAIL: no ODS-2 home block at LBN 1 ($lbl)"; exit 1; }
if [ "$mf" -lt 512 ]; then
    echo "FAIL: a 16 MB volume allows only $mf files (want >= 512)"
    exit 1
fi
echo "PASS: a 16 MB mastered volume allows $mf files"
