#!/bin/sh
# pack_initramfs_check.sh <repo-root> -- host check of tests/qemu/pack_initramfs.sh
# (rd vms-802f). Packs a synthetic staged tree shaped like the harness's and asserts:
#   1. the executive archive carries every suite and no /tests/corpus_rt entry;
#   2. the corpus archive carries every corpus program and no executive suite;
#   3. everything else (bin/, vms/, the non-suite /tests files) is in BOTH;
#   4. an archive over the budget fails the pack, by name.
# No QEMU, no container, no /dev/vms needed.
set -u
REPO=${1:?usage: pack_initramfs_check.sh <repo-root>}
PACK=$REPO/tests/qemu/pack_initramfs.sh
W=$(mktemp -d "${TMPDIR:-/tmp}/pack-initramfs-check.XXXXXX")
trap 'rm -rf "$W"' EXIT
fail=0
no() { echo "FAIL: $*"; fail=1; }

R=$W/root
mkdir -p "$R/bin" "$R/vms/SYS0/SYSCOMMON/SYSEXE" "$R/tests/corpus_rt"
for f in bin/busybox vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE tests/ovmx_boot_task \
         tests/corpus_seed_sysvol tests/DCL.EXE tests/test_arith_hparith init \
         tests/test_kmod_lock tests/test_syssvc_rms_acp tests/test_imgact_bind \
         tests/test_corpus_zlib tests/corpus_rt/sys_enqw tests/corpus_rt/lib_spawn \
         tests/corpus_rt/lib_spawn.args; do
    head -c 4096 /dev/urandom > "$R/$f"
done

list() { gzip -dc "$1" | cpio -t 2>/dev/null | sed 's|^\./||'; }

OUT_DIR=$W sh "$PACK" "$R" 1000000 > "$W/pack.log" 2>&1 \
    || { cat "$W/pack.log"; no "pack of a tree well under the budget failed"; }
list "$W/initramfs.cpio.gz" > "$W/exec.lst"
list "$W/initramfs-corpus.cpio.gz" > "$W/corpus.lst"
for f in tests/test_kmod_lock tests/test_syssvc_rms_acp tests/test_imgact_bind tests/test_corpus_zlib; do
    grep -qx "$f" "$W/exec.lst" || no "executive archive lacks suite $f"
    grep -qx "$f" "$W/corpus.lst" && no "corpus archive carries suite $f"
done
for f in tests/corpus_rt/sys_enqw tests/corpus_rt/lib_spawn tests/corpus_rt/lib_spawn.args; do
    grep -qx "$f" "$W/corpus.lst" || no "corpus archive lacks program $f"
    grep -qx "$f" "$W/exec.lst" && no "executive archive carries program $f"
done
for f in init bin/busybox vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE tests/ovmx_boot_task \
         tests/corpus_seed_sysvol tests/DCL.EXE tests/test_arith_hparith; do
    grep -qx "$f" "$W/exec.lst" || no "executive archive lacks shared file $f"
    grep -qx "$f" "$W/corpus.lst" || no "corpus archive lacks shared file $f"
done

# Over budget: the corpus archive alone is ~ 11 x 4 KiB + headers; 30000 bytes is under it.
if OUT_DIR=$W sh "$PACK" "$R" 30000 > "$W/over.log" 2>&1; then
    no "a pack over the budget succeeded"
else
    grep -q "over the 30000-byte budget" "$W/over.log" || { cat "$W/over.log"; no "over-budget failure did not name the budget"; }
fi

[ "$fail" = 0 ] && echo "PASS: pack_initramfs.sh splits the staged tree into an executive and a corpus archive, each whole and in budget"
exit "$fail"
