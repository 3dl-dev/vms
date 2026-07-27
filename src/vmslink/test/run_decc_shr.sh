#!/bin/sh
# run_decc_shr.sh — producer proof for DECC$SHR.EXE (bead vms-61f.1, pillar
# vms-ade). Builds LINK.EXE + OVMXDUMP, runs the mk_decc_shr.sh recipe to link
# the WHOLE musl libc.a + libgcc.a into an OVMX C-RTL shareable, and asserts the
# done-condition:
#
#   1. LINK.EXE links it CLEAN (no --allow-undefined, zero deferred externals) —
#      libgcc.a satisfies the soft-float/long-double builtins libc.a defers, and
#      the weak linker-defined boundary symbols (__init_array_start/end,
#      _DYNAMIC, ...) resolve to 0 (musl carries no static constructors).
#   2. The image is a valid ELF ET_DYN carrying .vms$sv and .vms$rel.
#   3. OVMXDUMP lists the C-RTL universals (malloc/free/memcpy/memset/strlen/
#      snprintf/printf/...) as PROCEDURE universals at nonzero image-relative
#      addresses.
#
# This item PRODUCES the shareable only; runtime init (__libc_start_main, TCB/
# thread-pointer setup, running constructors) is vms-61f.2. Runs in the arm64
# musl container (the CI decc-shr job / CLAUDE.md test loop). Exit 0 = success.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)     # src/vmslink/test
SRC=$(cd "$HERE/.." && pwd)             # src/vmslink
WORK=${WORK:-/tmp/decc-shr-test}
rm -rf "$WORK"; mkdir -p "$WORK"

echo "== build LINK.EXE + OVMXDUMP =="
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/LINK.EXE"   "$SRC/link.c"
$CC -std=gnu11 -O2 -Wall -Wextra -I"$SRC/include" -o "$WORK/OVMXDUMP"   "$SRC/dump_image.c"

if [ ! -f /usr/lib/libc.a ]; then
    echo "FAIL: /usr/lib/libc.a not found — this harness must run in the arm64 musl"
    echo "      container (the CI decc-shr job; see CLAUDE.md test loop)."
    exit 1
fi

echo
echo "== recipe: whole musl libc.a + libgcc.a -> DECC\$SHR.EXE (strict, no ld) =="
# Strict on purpose: mk_decc_shr.sh does NOT pass --allow-undefined, so any
# unresolved external that a C-RTL consumer would need aborts the link.
sh "$SRC/mk_decc_shr.sh" "$WORK/LINK.EXE" "$WORK/DECC\$SHR.EXE"

echo
echo "== readelf: must be ET_DYN with .vms\$sv + .vms\$rel =="
readelf -hSW "$WORK/DECC\$SHR.EXE" | grep -E "Type:|\.vms\\\$sv|\.vms\\\$rel|\.got|\.data" || true
readelf -h "$WORK/DECC\$SHR.EXE" | grep -q "DYN"        || { echo "FAIL: not ET_DYN"; exit 1; }
readelf -S "$WORK/DECC\$SHR.EXE" | grep -q '\.vms\$sv'  || { echo "FAIL: no .vms\$sv"; exit 1; }
readelf -S "$WORK/DECC\$SHR.EXE" | grep -q '\.vms\$rel' || { echo "FAIL: no .vms\$rel"; exit 1; }

echo
echo "== OVMXDUMP: C-RTL universals =="
OUT=$("$WORK/OVMXDUMP" "$WORK/DECC\$SHR.EXE")
echo "$OUT" | head -20
echo "  ..."

echo
echo "== assertions: core libc universals are PROCEDURE at nonzero addresses =="
for s in malloc free calloc realloc memcpy memmove memset memcmp \
         strlen strcmp strncpy strchr strstr snprintf vsnprintf printf \
         fprintf puts fwrite fread fopen fclose qsort getenv exit; do
    echo "$OUT" | grep -qE "PROCEDURE .* $s\$" \
        || { echo "FAIL: $s missing / not a PROCEDURE universal"; exit 1; }
done
# No universal may have a zero image-relative value (would mean unresolved).
echo "$OUT" | grep -E 'PROCEDURE +value=0x0{16}' \
    && { echo "FAIL: a universal resolved to address 0"; exit 1; } || true

echo
echo "== unneeded compiler-runtime builtins stay INTERNAL, not exported =="
# Most of libgcc.a's builtins are linked in but never appear in the symbol
# vector — a C-RTL consumer never calls them directly. EXCEPTION (vms-4ba.4):
# __addtf3/__trunctfdf2 and 16 other IEEE-quad ("tf", 128-bit long double)
# helpers WERE promoted to real universals for tcc-as-an-OVMX-image (TCC.EXE
# is a genuine cross-image CONSUMER of them — its own long-double constant
# folding calls them, unlike every prior consumer) — see mk_decc_shr.sh's
# vms-4ba.4 comment block for the full list. __multc3 (complex multiply) and
# __fixtfsi (another tf conversion, unused by tcc) are NOT in that list and
# must stay internal-only, so they remain the regression check here.
for b in __multc3 __fixtfsi; do
    echo "$OUT" | grep -qE "PROCEDURE .* $b\$" \
        && { echo "FAIL: compiler-runtime builtin $b leaked into .vms\$sv"; exit 1; } \
        || true
done

echo
echo "ALL DECC\$SHR PRODUCER CHECKS PASSED"
