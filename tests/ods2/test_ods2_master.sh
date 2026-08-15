#!/bin/sh
# tests/ods2/test_ods2_master.sh
#
# vms-5eb R6-build: prove the vmsfs_master BINARY masters a host source tree
# onto a GENUINE ODS-2 (Files-11 L2, "DECFILE11B") volume via --ods2 and reads
# it back over a REAL block device (ods2_bdev_*, never POSIX opendir), the same
# path the ODS-2 runtime flip (RMS/DCL/MOUNT) consumes.
#
# This exercises the actual shipped tool end-to-end, not just the library
# (test_ods2_path.c covers the library primitives + verbatim byte-identity):
#   1. master --ods2 lays SYS0/SYSCOMMON/{SYSEXE,SYSLIB} + a text + a binary
#      file onto a fresh image;
#   2. the on-disk home block at LBN 1 is a genuine "DECFILE11B" home block;
#   3. list --ods2 walks the whole tree back through the real MFD->component
#      FID chain and prints every file at the right VMS path;
#   4. FAIL-HONEST (Rule 9 / INV-6): the DEFAULT (VMFS) master is NOT a genuine
#      ODS-2 volume, and list --ods2 on it reports that honestly (NOTODS2)
#      rather than faking a listing.
#
# VMSFS_MASTER is injected by CMake ($<TARGET_FILE:vmsfs_master>).

set -eu

MASTER="${VMSFS_MASTER:?VMSFS_MASTER (path to vmsfs_master) must be set}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ods2master.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT INT TERM

TREE="$WORK/tree"
IMG="$WORK/ods2.img"
VMFS_IMG="$WORK/vmfs.img"

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- 1. Build a small system-disk-shaped host tree. ----
mkdir -p "$TREE/SYS0/SYSCOMMON/SYSEXE" "$TREE/SYS0/SYSCOMMON/SYSLIB"
printf '$ SET NOON\n$ WRITE SYS$OUTPUT "hello"\n' > "$TREE/SYS0/SYSCOMMON/SYSEXE/LOGIN.COM"
printf 'documentation record\n'                    > "$TREE/SYS0/SYSCOMMON/SYSLIB/README.TXT"
# A binary image: MUST be stored verbatim (create_file_raw), not VAR-reframed.
head -c 4096 /dev/urandom                          > "$TREE/SYS0/SYSCOMMON/SYSEXE/DCL.EXE"

# ---- 2. master --ods2 ----
MOUT="$WORK/master.out"
"$MASTER" --ods2 master "$IMG" OVMXSYS "$TREE" 4 > "$MOUT" 2>&1 \
    || { cat "$MOUT" >&2; fail "master --ods2 exited non-zero"; }
grep -q 'MASTER-I-DONE' "$MOUT" || { cat "$MOUT" >&2; fail "no MASTER-I-DONE"; }
grep -q 'DECFILE11B'    "$MOUT" || { cat "$MOUT" >&2; fail "master did not announce DECFILE11B"; }
echo "PASS: master --ods2 produced $IMG"

# ---- 3. the on-disk home block (LBN 1) is a genuine DECFILE11B home block. ----
dd if="$IMG" bs=512 skip=1 count=1 2>/dev/null | grep -q 'DECFILE11B' \
    || fail "home block at LBN 1 is not a DECFILE11B ODS-2 home block"
echo "PASS: on-disk home block is DECFILE11B"

# ---- 4. list --ods2 walks the tree back over the block device. ----
LOUT="$WORK/list.out"
"$MASTER" --ods2 list "$IMG" > "$LOUT" 2>&1 \
    || { cat "$LOUT" >&2; fail "list --ods2 exited non-zero"; }

for want in \
    'OVMXSYS' \
    '\[000000\]INDEXF.SYS;1' \
    '\[000000\]BITMAP.SYS;1' \
    '\[000000\]SYS0.DIR;1' \
    '\[SYS0\]SYSCOMMON.DIR;1' \
    '\[SYS0.SYSCOMMON\]SYSEXE.DIR;1' \
    '\[SYS0.SYSCOMMON\]SYSLIB.DIR;1' \
    '\[SYS0.SYSCOMMON.SYSEXE\]LOGIN.COM;1' \
    '\[SYS0.SYSCOMMON.SYSEXE\]DCL.EXE;1' \
    '\[SYS0.SYSCOMMON.SYSLIB\]README.TXT;1'
do
    grep -Eq "$want" "$LOUT" || { cat "$LOUT" >&2; fail "list missing: $want"; }
done
echo "PASS: list --ods2 walked the full tree over the real FID chain"

# ---- 5. FAIL-HONEST: the legacy VMFS master is not a genuine ODS-2 volume. ----
"$MASTER" --vmfs master "$VMFS_IMG" OVMXSYS "$TREE" 4 > "$WORK/vmfs.master.out" 2>&1 \
    || { cat "$WORK/vmfs.master.out" >&2; fail "VMFS master exited non-zero"; }
# list --ods2 on a VMFS image must FAIL honestly, never fake a listing.
if "$MASTER" --ods2 list "$VMFS_IMG" > "$WORK/vmfs.list.out" 2>&1; then
    cat "$WORK/vmfs.list.out" >&2
    fail "list --ods2 on a VMFS image should have failed (fail-honest INV-6)"
fi
grep -q 'NOTODS2' "$WORK/vmfs.list.out" \
    || { cat "$WORK/vmfs.list.out" >&2; fail "VMFS image rejected without the honest NOTODS2 reason"; }
echo "PASS: list --ods2 rejects a non-ODS-2 (VMFS) volume, fail-honest"

echo "ALL PASS: vmsfs_master --ods2 masters + reads a genuine ODS-2 SYS\$DISK tree"
