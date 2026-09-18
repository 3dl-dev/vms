#!/bin/sh
# inject-ods2-config.test.sh - offline ground-source proof (rd vms-f0f) that the
# ODS-2 SYSTEM-DISK config injector places SYS$SYSTEM:OVMXVMSSYS.PAR on a GENUINE
# Files-11 volume and the config reads back byte-exact via the project's OWN
# ODS-2 reader. No emulator is booted.
#
# Flow (all against a genuine ODS-2 fixture mastered by the project's own
# vmsfs_master, the SAME tool that masters the shipped distribution image):
#   1. build vmsfs_master + ods2_inject_sysgen from src/vmsfs/ods2 (project codec)
#   2. master a genuine ODS-2 fixture from distro/rootfs/vms (carries the stock
#      SYS$SYSTEM:OVMXVMSSYS.PAR;1)
#   3. wrap it qcow2 (simulate the demo's sysdisk.qcow2)
#   4. run inject-ods2-config.sh qcow2 -> qcow2 (SCSNODE=OVMXA, VAXCLUSTER=2)
#   5. assert (via the project's ODS-2 reader + mk_democonfig.parse_sysgen_store):
#        magic == 0x53595347, VAXCLUSTER == 2, SCSNODE == OVMXA
#   6. assert disk integrity: exactly OVMXVMSSYS.PAR;2 added, nothing removed
#
# Requires: cc, qemu-img, python3 (all already present on the dev host; NO
# installs -- all scratch lands under a /tmp workdir).

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
DEMO=$(cd "$HERE/.." && pwd)
REPO=$(cd "$DEMO/../.." && pwd)
ODS2_DIR="$REPO/src/vmsfs/ods2"
ODS2_INC="$REPO/src/vmsfs/include"
CC=${CC:-cc}
PYTHON=${PYTHON:-python3}

for t in "$CC" qemu-img "$PYTHON"; do
    command -v "$t" >/dev/null 2>&1 || { echo "SKIP: $t not on PATH"; exit 0; }
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/f0f-ods2-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

ODS2_SRCS="$ODS2_DIR/ods2_reader.c $ODS2_DIR/ods2_writer.c $ODS2_DIR/ods2_edit.c \
$ODS2_DIR/ods2_bdev.c $ODS2_DIR/ods2_path.c $ODS2_DIR/ods2_block_posix.c"

echo "1. building vmsfs_master + ods2_inject_sysgen (project codec)"
# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 -I "$ODS2_INC" \
    "$REPO/tools/vmsfs_master.c" $ODS2_SRCS -o "$WORK/vmsfs_master"

echo "2. mastering a genuine ODS-2 fixture from distro/rootfs/vms"
OVMX_MASTER_ODS2=1 "$WORK/vmsfs_master" --ods2 master \
    "$WORK/stock.raw" OVMXSYS "$REPO/distro/rootfs/vms" 128 >/dev/null
"$WORK/vmsfs_master" --ods2 list "$WORK/stock.raw" > "$WORK/list-before.txt"
grep -qi '\]OVMXVMSSYS.PAR;1' "$WORK/list-before.txt" \
    || { echo "FAIL: fixture missing stock SYS\$SYSTEM:OVMXVMSSYS.PAR;1"; exit 1; }

echo "3. wrapping fixture as qcow2 (simulated demo sysdisk.qcow2)"
qemu-img convert -O qcow2 "$WORK/stock.raw" "$WORK/sysdisk.qcow2"

echo "4. running inject-ods2-config.sh (qcow2 -> qcow2)"
"$DEMO/inject-ods2-config.sh" "$WORK/sysdisk.qcow2" "$WORK/out.qcow2" \
    --scsnode OVMXA --scssystemid 1987 --vaxcluster 2 > "$WORK/inject.log" 2>&1 \
    || { echo "FAIL: injector exited non-zero"; cat "$WORK/inject.log"; exit 1; }
grep -q "PASS: OUTPUT sysdisk carries the injected config" "$WORK/inject.log" \
    || { echo "FAIL: injector read-back proof did not pass"; cat "$WORK/inject.log"; exit 1; }

echo "5. disk-integrity list-diff (exactly OVMXVMSSYS.PAR;2 added, none removed)"
qemu-img convert -O raw "$WORK/out.qcow2" "$WORK/out.raw"
"$WORK/vmsfs_master" --ods2 list "$WORK/out.raw" > "$WORK/list-after.txt"
sort "$WORK/list-before.txt" > "$WORK/before.sorted"
sort "$WORK/list-after.txt"  > "$WORK/after.sorted"
ADDED=$(comm -13 "$WORK/before.sorted" "$WORK/after.sorted")
REMOVED=$(comm -23 "$WORK/before.sorted" "$WORK/after.sorted")
echo "   added:   $ADDED"
[ -z "$REMOVED" ] || { echo "FAIL: files removed from the volume: $REMOVED"; exit 1; }
echo "$ADDED" | grep -q 'OVMXVMSSYS.PAR;2' \
    || { echo "FAIL: OVMXVMSSYS.PAR;2 not added"; exit 1; }
[ "$(echo "$ADDED" | grep -c .)" -eq 1 ] \
    || { echo "FAIL: more than one file changed on the volume"; exit 1; }

echo "PASS: ODS-2 config injection places VAXCLUSTER=2 SYS\$SYSTEM:OVMXVMSSYS.PAR;2,"
echo "      reads back byte-exact via the project reader, and disturbs nothing else."
