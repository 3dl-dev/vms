#!/bin/sh
# pack_initramfs.sh <staged-root> <budget-bytes> -- pack the QEMU harness's two
# boot archives from ONE staged tree (rd vms-802f).
#
# The harness boots a 512 MB guest whose rootfs is the unpacked initramfs, in two
# modes that never run in the same boot (tests/qemu/init.sh):
#   * the executive suites (/tests/test_kmod_* test_syssvc_* test_imgact_*
#     test_corpus_*), the kernel-executive shards and the negative controls;
#   * the corpus runtime scoreboard (ovmx.corpus=1, /tests/corpus_rt/*), which runs
#     INSTEAD of the suites.
# One archive carrying both sets measured 183 MB of a 176 MiB budget, with no room
# for the next program. So the staged tree is packed twice, each archive leaving out
# only the set its mode never runs:
#   /initramfs.cpio.gz         everything except /tests/corpus_rt
#   /initramfs-corpus.cpio.gz  everything except the four executive-suite globs
# Every boot still runs every suite or program it ran before: init.sh's suite loop
# globs exactly those four patterns, and its corpus loop globs /tests/corpus_rt.
# run_tests.sh boots the corpus archive when OVMX_CORPUS_RT=1.
#
# Each archive must fit the budget, and each must carry its set whole: the suite
# count in the executive archive and the corpus program count in the corpus archive
# equal the staged tree's, or the pack fails by name.
#
# Usage: pack_initramfs.sh <staged-root> <budget-bytes>
#   writes /initramfs.cpio.gz and /initramfs-corpus.cpio.gz (OUT_DIR overrides /).
set -eu
ROOT=${1:?usage: pack_initramfs.sh <staged-root> <budget-bytes>}
BUDGET=${2:?usage: pack_initramfs.sh <staged-root> <budget-bytes>}
OUT=${OUT_DIR:-}
cd "$ROOT"

n_suites=$(find ./tests -maxdepth 1 \( -name 'test_kmod_*' -o -name 'test_syssvc_*' \
    -o -name 'test_imgact_*' -o -name 'test_corpus_*' \) -print | wc -l)
n_corpus=$(find ./tests/corpus_rt -type f 2>/dev/null | wc -l)

find . -path ./tests/corpus_rt -prune -o -print \
    | cpio -o -H newc 2>/dev/null | gzip > "$OUT/initramfs.cpio.gz"
find . \( -path './tests/test_kmod_*' -o -path './tests/test_syssvc_*' \
    -o -path './tests/test_imgact_*' -o -path './tests/test_corpus_*' \) -prune -o -print \
    | cpio -o -H newc 2>/dev/null | gzip > "$OUT/initramfs-corpus.cpio.gz"

check() { # <archive> <what> <expected-count> <pattern>
    bytes=$(gzip -dc "$1" | wc -c)
    pct=$(( (BUDGET - bytes) * 100 / BUDGET ))
    echo "Initramfs $1: unpacked $bytes bytes (budget $BUDGET, ${pct}% headroom), packed $(ls -l "$1" | awk '{print $5}')"
    if [ "$bytes" -gt "$BUDGET" ]; then
        echo "FATAL: $1 unpacks to $bytes bytes, over the $BUDGET-byte budget -- the 512 MB guest would run out of RAM-backed rootfs mid-run (or fail to unpack it at all). Shrink what the initramfs carries; do not raise run_tests.sh's -m to fit it."
        exit 1
    fi
    have=$(gzip -dc "$1" | cpio -t 2>/dev/null | grep -cE "$4" || true)
    if [ "$have" -ne "$3" ]; then
        echo "FATAL: $1 carries $have $2, the staged tree has $3 -- a boot from it would silently run fewer"
        exit 1
    fi
}
check "$OUT/initramfs.cpio.gz" "executive suites" "$n_suites" '^(\./)?tests/test_(kmod|syssvc|imgact|corpus)_[^/]*$'
check "$OUT/initramfs-corpus.cpio.gz" "corpus programs" "$n_corpus" '^(\./)?tests/corpus_rt/[^/]+$'
# And neither carries the other's set (that is the whole point of the split).
if gzip -dc "$OUT/initramfs.cpio.gz" | cpio -t 2>/dev/null | grep -qE '^(\./)?tests/corpus_rt/'; then
    echo "FATAL: the executive archive still carries /tests/corpus_rt"; exit 1
fi
if gzip -dc "$OUT/initramfs-corpus.cpio.gz" | cpio -t 2>/dev/null | grep -qE '^(\./)?tests/test_(kmod|syssvc|imgact|corpus)_'; then
    echo "FATAL: the corpus archive still carries executive suites"; exit 1
fi
[ "$n_suites" -gt 0 ] || { echo "FATAL: no executive suites staged under $ROOT/tests"; exit 1; }
