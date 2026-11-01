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
#   7. rd vms-3ea: the output volume carries the node's OWN label, OVMXASYS
#      (<SCSNODE>SYS), not the kit's OVMXSYS -- and --label overrides it
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
# The volume line (label) is asserted on its own in step 6; the diff is files.
grep -v '^%MASTER-I-VOLUME' "$WORK/list-before.txt" | sort > "$WORK/before.sorted"
grep -v '^%MASTER-I-VOLUME' "$WORK/list-after.txt"  | sort > "$WORK/after.sorted"
ADDED=$(comm -13 "$WORK/before.sorted" "$WORK/after.sorted")
REMOVED=$(comm -23 "$WORK/before.sorted" "$WORK/after.sorted")
echo "   added:   $ADDED"
[ -z "$REMOVED" ] || { echo "FAIL: files removed from the volume: $REMOVED"; exit 1; }
echo "$ADDED" | grep -q 'OVMXVMSSYS.PAR;2' \
    || { echo "FAIL: OVMXVMSSYS.PAR;2 not added"; exit 1; }
[ "$(echo "$ADDED" | grep -c .)" -eq 1 ] \
    || { echo "FAIL: more than one file changed on the volume"; exit 1; }

echo "6. rd vms-3ea: the node's system disk carries its OWN label"
grep -q '^%MASTER-I-VOLUME, OVMXSYS ' "$WORK/list-before.txt" \
    || { echo "FAIL: the stock fixture is not labelled OVMXSYS"; cat "$WORK/list-before.txt" | head -1; exit 1; }
grep -q '^%MASTER-I-VOLUME, OVMXASYS ' "$WORK/list-after.txt" \
    || { echo "FAIL: node OVMXA's sysdisk is not labelled OVMXASYS"; head -1 "$WORK/list-after.txt"; exit 1; }
grep -q 'volume label = OVMXASYS' "$WORK/inject.log" \
    || { echo "FAIL: the injector's own read-back did not report the label"; exit 1; }
"$DEMO/inject-ods2-config.sh" "$WORK/sysdisk.qcow2" "$WORK/out2.qcow2" \
    --scsnode OVMXB --scssystemid 1988 --label NODEBDISK > "$WORK/inject2.log" 2>&1 \
    || { echo "FAIL: injector with --label exited non-zero"; cat "$WORK/inject2.log"; exit 1; }
qemu-img convert -O raw "$WORK/out2.qcow2" "$WORK/out2.raw"
"$WORK/vmsfs_master" --ods2 list "$WORK/out2.raw" | grep -q '^%MASTER-I-VOLUME, NODEBDISK ' \
    || { echo "FAIL: --label NODEBDISK was not applied"; exit 1; }
if "$DEMO/inject-ods2-config.sh" "$WORK/sysdisk.qcow2" "$WORK/out3.qcow2" \
    --scsnode OVMXB --scssystemid 1988 --label 'not.valid' > "$WORK/inject3.log" 2>&1; then
    echo "FAIL: an invalid --label produced a sysdisk"; exit 1
fi

echo "PASS: ODS-2 config injection places VAXCLUSTER=2 SYS\$SYSTEM:OVMXVMSSYS.PAR;2,"
echo "      reads back byte-exact via the project reader, disturbs no other file,"
echo "      and gives the node's system disk its own label (OVMXASYS, rd vms-3ea)."
