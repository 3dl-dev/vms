#!/bin/sh
# build-shr-activation-vax.sh - build the four artifacts the P4 runtime-
# activation gate (rd vms-d4a, epic vms-404 P4) stages onto the SIMH VAX
# rail's mastered ODS-2 system volume:
#
#   IMGACT.EXE      the elf32-vax OVMX image activator (rd vms-73b2/vms-33b,
#                    P2), staged at SYS$SYSEXE (the default PT_INTERP path
#                    LINKVAX.EXE bakes into every VAX executable it emits).
#   LIBVMS$SHR.EXE   the REAL, SHIPPED elf32-vax .vms$sv shareable graph (rd
#                    vms-c7f7/P3b) -- built by the UNMODIFIED
#                    tools/cross-vax/build-vax-shareable-graph.sh, never
#                    touched by this script.
#   P4BOOT$SHR.EXE   a tiny NEW producer, purpose-built for this gate, that
#                    supplies the crt0's REQUIRED `exit` import plus a
#                    console-write primitive (p4boot_rt.c) -- LIBVMS$SHR.EXE
#                    is the real RTL and rightly exports neither.
#   CONSUMER.EXE     a NEW elf32-vax executable (LINKVAX.EXE --executable,
#                    consumer_main.c) that imports `purdy_s_hash` from
#                    LIBVMS$SHR.EXE -- a REAL cross-shareable universal-symbol
#                    call into the shipped RTL -- and `exit`/`p4boot_puts`
#                    from P4BOOT$SHR.EXE.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), same
# substrate as build-vax-shareable-graph.sh / run_test_vax.sh / run_test_vax_
# exec.sh. Nothing here touches the host (Rule 9). Fails loudly (set -eu) on
# any missing input or a build step that does not produce its artifact.
#
# Usage: build-shr-activation-vax.sh <repo-root> <out-dir>
set -eu

SRC="${1:?usage: $0 <repo-root> <out-dir>}"
OUT="${2:?usage: $0 <repo-root> <out-dir>}"
TARGET="${TARGET:-vax--netbsdelf}"
CROSS_PREFIX="${CROSS_PREFIX:-/opt/cross}"
SYSROOT="${SYSROOT:-$CROSS_PREFIX/sysroot}"
CC="${CC:-gcc}"
VAXCC="${VAXCC:-$CROSS_PREFIX/bin/$TARGET-gcc}"
VAXREADELF="${VAXREADELF:-$CROSS_PREFIX/bin/$TARGET-readelf}"
BUILD_DIR="${BUILD_DIR:-/tmp/build-shr-activation-vax}"
HERE="$SRC/tools/cross-vax/shr-activation"

die() { echo "%%BUILD-SHR-ACTIVATION-VAX-F, $*" >&2; exit 1; }

[ -x "$VAXCC" ]      || die "vax cross gcc not found: $VAXCC"
[ -d "$SYSROOT" ]    || die "vax sysroot not found: $SYSROOT"
[ -f "$HERE/p4boot_rt.c" ]      || die "missing $HERE/p4boot_rt.c"
[ -f "$HERE/consumer_main.c" ]  || die "missing $HERE/consumer_main.c"

rm -rf "$BUILD_DIR"; mkdir -p "$BUILD_DIR" "$OUT"

echo "=== 1. LIBVMS\$SHR.EXE -- the SHIPPED elf32-vax .vms\$sv graph (rd vms-c7f7, UNMODIFIED script) ==="
LIBVMS_GRAPH_BUILD="$BUILD_DIR/libvms-graph"
( cd "$SRC" && BUILD_DIR="$LIBVMS_GRAPH_BUILD" OUT_DIR="$LIBVMS_GRAPH_BUILD/out" \
    sh tools/cross-vax/build-vax-shareable-graph.sh )
LIBVMS_SHR="$LIBVMS_GRAPH_BUILD/out/LIBVMS\$SHR.EXE"
[ -s "$LIBVMS_SHR" ] || die "LIBVMS\$SHR.EXE did not build: $LIBVMS_SHR"
cp "$LIBVMS_SHR" "$OUT/LIBVMS\$SHR.EXE"
echo "OK: $OUT/LIBVMS\$SHR.EXE ($(stat -c%s "$OUT/LIBVMS\$SHR.EXE") bytes)"
echo

echo "=== 2. LINKVAX.EXE (host tool, -DOVMX_LINK_ELF32, src/vmslink/link.c UNCHANGED) ==="
LINKVAX="$BUILD_DIR/LINKVAX.EXE"
"$CC" -std=gnu11 -O2 -Wall -Wextra -DOVMX_LINK_ELF32 -I"$SRC/src/vmslink/include" \
    -o "$LINKVAX" "$SRC/src/vmslink/link.c"
[ -x "$LINKVAX" ] || die "LINKVAX.EXE did not build"
echo "OK: $LINKVAX"
echo

echo "=== 3. P4BOOT\$SHR.EXE -- the gate's tiny exit/write runtime shim (elf32-vax .vms\$sv) ==="
"$VAXCC" -fPIC -O2 -c -o "$BUILD_DIR/p4boot_rt.o" "$HERE/p4boot_rt.c"
cp "$SYSROOT/usr/lib/libc.a" "$BUILD_DIR/libc.OLB"
P4BOOT_SHR="$OUT/P4BOOT\$SHR.EXE"
"$LINKVAX" --shareable --allow-undefined \
    --symbol-vector "exit=PROCEDURE,p4boot_puts=PROCEDURE" \
    --gsmatch EQUAL,1,0 -o "$P4BOOT_SHR" \
    "$BUILD_DIR/p4boot_rt.o" "$BUILD_DIR/libc.OLB" \
    > "$BUILD_DIR/p4boot-link.log" 2>&1 \
    || { cat "$BUILD_DIR/p4boot-link.log"; die "LINKVAX.EXE --shareable failed building P4BOOT\$SHR.EXE"; }
cat "$BUILD_DIR/p4boot-link.log"
grep -q '%LINK-S-CREATED' "$BUILD_DIR/p4boot-link.log" \
    || die "no %LINK-S-CREATED banner -- P4BOOT\$SHR.EXE was not produced"
[ -s "$P4BOOT_SHR" ] || die "$P4BOOT_SHR missing/empty after a reported success"
"$VAXREADELF" -SW "$P4BOOT_SHR" | grep -qE '\.vms\$sv' \
    || die "P4BOOT\$SHR.EXE missing .vms\$sv section"
echo "OK: $P4BOOT_SHR ($(stat -c%s "$P4BOOT_SHR") bytes)"
echo

echo "=== 4. CONSUMER.EXE -- elf32-vax executable importing purdy_s_hash from LIBVMS\$SHR ==="
"$VAXCC" -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector \
    -c -o "$BUILD_DIR/consumer_main.o" "$HERE/consumer_main.c"
CONSUMER="$OUT/CONSUMER.EXE"
"$LINKVAX" --executable --use "$LIBVMS_SHR" --use "$P4BOOT_SHR" \
    -o "$CONSUMER" "$BUILD_DIR/consumer_main.o" \
    > "$BUILD_DIR/consumer-link.log" 2>&1 \
    || { cat "$BUILD_DIR/consumer-link.log"; die "LINKVAX.EXE --executable failed building CONSUMER.EXE"; }
cat "$BUILD_DIR/consumer-link.log"
grep -q '%LINK-S-CREATED' "$BUILD_DIR/consumer-link.log" \
    || die "no %LINK-S-CREATED banner -- CONSUMER.EXE was not produced"
[ -s "$CONSUMER" ] || die "$CONSUMER missing/empty after a reported success"
echo "OK: $CONSUMER ($(stat -c%s "$CONSUMER") bytes)"
echo

echo "=== 5. readelf-shape assertions (the vms-099 P3a/P3b done-condition bar, restated for CONSUMER.EXE) ==="
"$VAXREADELF" -h "$CONSUMER" | grep -qE "Class:.*ELF32" || die "CONSUMER.EXE is not ELF32"
"$VAXREADELF" -h "$CONSUMER" | grep -qi "Digital VAX"   || die "CONSUMER.EXE is not EM_VAX"
"$VAXREADELF" -h "$CONSUMER" | grep -qE "Type:.*DYN"    || die "CONSUMER.EXE is not ET_DYN"
INTERP=$("$VAXREADELF" -lW "$CONSUMER" | sed -n 's/.*Requesting program interpreter: \(.*\)\]/\1/p')
echo "   PT_INTERP = $INTERP"
echo "$INTERP" | grep -q "IMGACT.EXE" || die "CONSUMER.EXE PT_INTERP is not IMGACT.EXE (got '$INTERP')"
echo "$INTERP" | grep -q "ld.elf_so" && die "CONSUMER.EXE PT_INTERP is ld.elf_so -- the rejected LARP"
# objcopy the raw section bytes rather than grepping readelf -x's hex dump --
# a name can straddle that dump's fixed 16-byte-per-line boundary and be
# missed by a naive line-grep even though it is genuinely present (same
# discipline as build-vax-shareable-graph.sh's own producer spot-check).
IMPBIN="$BUILD_DIR/vms_imp.bin"
"$TARGET-objcopy" -O binary --only-section='.vms$imp' "$CONSUMER" "$IMPBIN"
strings "$IMPBIN" | grep -qF 'LIBVMS$SHR' \
    || die "CONSUMER.EXE .vms\$imp does not reference LIBVMS\$SHR.EXE"
strings "$IMPBIN" | grep -qF 'P4BOOT$SHR' \
    || die "CONSUMER.EXE .vms\$imp does not reference P4BOOT\$SHR.EXE"
if "$VAXREADELF" -r "$CONSUMER" 2>/dev/null | grep -q "R_VAX"; then
    die "CONSUMER.EXE carries UNAPPLIED R_VAX_* dynamic relocations"
fi
echo "   OK: elf32-vax ET_DYN, PT_INTERP=IMGACT.EXE, .vms\$imp -> LIBVMS\$SHR.EXE + P4BOOT\$SHR.EXE, no unapplied R_VAX_*"
echo

echo "=== 6. IMGACT.EXE (elf32-vax, -DOVMX_IMGACT=ON, rd vms-73b2/vms-33b) ==="
IMGACT_BUILD="$BUILD_DIR/imgact-cmake"
cmake -S "$SRC" -B "$IMGACT_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake" \
    -DOVMX_IMGACT=ON -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF >/dev/null
cmake --build "$IMGACT_BUILD" --target imgact >/dev/null
IMGACT=$(find "$IMGACT_BUILD" -name IMGACT.EXE | head -1)
[ -n "$IMGACT" ] && [ -s "$IMGACT" ] || die "IMGACT.EXE(vax) did not build"
cp "$IMGACT" "$OUT/IMGACT.EXE"
echo "OK: $OUT/IMGACT.EXE ($(stat -c%s "$OUT/IMGACT.EXE") bytes)"
echo

echo "ALL SHR-ACTIVATION ARTIFACTS BUILT: $OUT"
ls -l "$OUT" | sed 's/^/   /'
