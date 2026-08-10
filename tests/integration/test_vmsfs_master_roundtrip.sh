#!/usr/bin/env bash
#
# test_vmsfs_master_roundtrip.sh - Prove the VMSFS mastering tool round-trips.
#
# The mastering tool (tools/vmsfs_master.c) writes a populated VMSFS image from
# a Linux source tree; vms-8ab will call it to build the distribution image. A
# tool that writes an image nothing can read is not done, so this test masters
# a tree and reads every file back BYTE-EXACT:
#
#   1. A synthetic tree exercising the hard cases:
#        - a subdirectory hierarchy several levels deep
#        - a directory with >5 entries (spills a second 88-byte dir block)
#        - a file larger than one 512-byte block (multi-block retrieval)
#        - a binary file (all 256 byte values)
#        - an empty (0-byte) file
#   2. The real distro system tree (distro/rootfs/vms), which vms-8ab masters.
#
# For each: master -> extract -> `diff -r` must report the trees identical.
#
# Usage: test_vmsfs_master_roundtrip.sh <vmsfs_master-binary> <repo-source-dir>
#
set -euo pipefail

MASTER="${1:?usage: $0 <vmsfs_master-binary> <repo-source-dir>}"
REPO="${2:?usage: $0 <vmsfs_master-binary> <repo-source-dir>}"

if [ ! -x "$MASTER" ]; then
    echo "FAIL: mastering tool not executable: $MASTER" >&2
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Case 1: synthetic tree covering the awkward layout paths.
# ---------------------------------------------------------------------------
SRC="$WORK/src"
mkdir -p "$SRC/SYS0/SYSCOMMON/SYSEXE" "$SRC/SYS0/SYSCOMMON/SYSMGR" "$SRC/DEEP/A/B/C"

# Small text files.
printf 'Hello from VMSFS mastering.\n' > "$SRC/SYS0/SYSCOMMON/SYSMGR/STARTUP.COM"
printf 'set noon\n' > "$SRC/SYS0/SYSCOMMON/SYSMGR/LOGIN.COM"
printf 'nested\n' > "$SRC/DEEP/A/B/C/LEAF.TXT"
# File with no type component.
printf 'README, no extension\n' > "$SRC/SYS0/README"

# Directory with >5 entries -> forces a 2nd directory data block.
for i in 1 2 3 4 5 6 7; do
    printf 'record %d\n' "$i" > "$SRC/SYS0/SYSCOMMON/SYSEXE/FILE${i}.DAT"
done

# Multi-block file (> 512 bytes): 4000 'A's + newline.
{ head -c 4000 /dev/zero | tr '\0' 'A'; printf '\n'; } > "$SRC/SYS0/SYSCOMMON/SYSEXE/BIG.DAT"

# Binary file: every byte value 0..255 (bash printf interprets \xHH directly,
# so this needs no python or other helper).
: > "$SRC/SYS0/SYSCOMMON/SYSEXE/BINARY.DAT"
for i in $(seq 0 255); do
    printf "\\x$(printf '%02x' "$i")" >> "$SRC/SYS0/SYSCOMMON/SYSEXE/BINARY.DAT"
done

# Empty file.
: > "$SRC/SYS0/SYSCOMMON/SYSEXE/EMPTY.DAT"

IMG="$WORK/synthetic.img"
OUT="$WORK/synthetic.out"

"$MASTER" master "$IMG" OVMXSYN "$SRC" >/dev/null \
    || fail "master (synthetic) exited non-zero"
"$MASTER" list "$IMG" >/dev/null \
    || fail "list (synthetic) exited non-zero"
"$MASTER" extract "$IMG" "$OUT" >/dev/null \
    || fail "extract (synthetic) exited non-zero"

if ! diff -r "$SRC" "$OUT" >"$WORK/diff.synthetic" 2>&1; then
    echo "----- synthetic round-trip diff -----" >&2
    cat "$WORK/diff.synthetic" >&2
    fail "synthetic tree did not round-trip byte-exact"
fi
echo "PASS: synthetic tree round-tripped byte-exact"

# ---------------------------------------------------------------------------
# Case 2: the real distribution system tree (what vms-8ab masters).
# ---------------------------------------------------------------------------
DISTRO="$REPO/distro/rootfs/vms"
if [ -d "$DISTRO" ]; then
    DIMG="$WORK/distro.img"
    DOUT="$WORK/distro.out"
    "$MASTER" master "$DIMG" OVMXSYS "$DISTRO" >/dev/null \
        || fail "master (distro) exited non-zero"
    "$MASTER" extract "$DIMG" "$DOUT" >/dev/null \
        || fail "extract (distro) exited non-zero"
    if ! diff -r "$DISTRO" "$DOUT" >"$WORK/diff.distro" 2>&1; then
        echo "----- distro round-trip diff -----" >&2
        cat "$WORK/diff.distro" >&2
        fail "distro tree (distro/rootfs/vms) did not round-trip byte-exact"
    fi
    echo "PASS: distro system tree round-tripped byte-exact"
else
    echo "SKIP: $DISTRO not present (distro tree round-trip)"
fi

echo "ALL PASS: vmsfs_master round-trip"
exit 0
