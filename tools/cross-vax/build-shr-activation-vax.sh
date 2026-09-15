#!/bin/sh
# build-shr-activation-vax.sh - build the three artifacts the P4 runtime-
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
#   CONSUMER.EXE     a NEW elf32-vax executable (LINKVAX.EXE --executable) whose
#                    ONLY cross-image .vms$imp import is `purdy_s_hash` from the
#                    shipped LIBVMS$SHR.EXE -- a REAL cross-shareable universal-
#                    symbol call into the shipped RTL. Its own freestanding
#                    `_start` (start_vax.S) means LINKVAX synthesizes no crt0
#                    and force-binds no `exit` import (which would otherwise
#                    demand a producer the VAX Decision-A substrate has none
#                    of), and the NetBSD/vax libc.a is pulled STATICALLY for
#                    printf/write/_exit so those never become imports. This
#                    replaces the prior cut's gate-private P4BOOT$SHR.EXE
#                    producer (whose raw-write output never reached the SIMH
#                    console, and whose raw _exit recorded no condition), rd
#                    vms-d4a re-instrumentation.
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
VAXAR="${VAXAR:-$CROSS_PREFIX/bin/$TARGET-ar}"
BUILD_DIR="${BUILD_DIR:-/tmp/build-shr-activation-vax}"
HERE="$SRC/tools/cross-vax/shr-activation"

die() { echo "%%BUILD-SHR-ACTIVATION-VAX-F, $*" >&2; exit 1; }

[ -x "$VAXCC" ]      || die "vax cross gcc not found: $VAXCC"
[ -d "$SYSROOT" ]    || die "vax sysroot not found: $SYSROOT"
[ -f "$HERE/consumer_main.c" ]  || die "missing $HERE/consumer_main.c"
[ -f "$HERE/start_vax.S" ]      || die "missing $HERE/start_vax.S"

rm -rf "$BUILD_DIR"; mkdir -p "$BUILD_DIR" "$OUT"
# Purge any stale gate-private producer left by an earlier cut in a reused
# artifacts cache -- this gate no longer builds or stages P4BOOT$SHR.EXE.
rm -f "$OUT/P4BOOT\$SHR.EXE"

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

echo "=== 3. CONSUMER.EXE -- elf32-vax executable; ONLY .vms\$imp import = purdy_s_hash from LIBVMS\$SHR ==="
# Freestanding entry (start_vax.S) + the C body (consumer_main.c). consumer_main.c
# is NOT compiled -ffreestanding: it uses the C-RTL printf/fflush channel
# (rc3.c's console-surfacing shape) plus a bare write(2) fallback.
"$VAXCC" -fPIC -O2 -c -o "$BUILD_DIR/start_vax.o" "$HERE/start_vax.S"
"$VAXCC" -fPIC -O2 -c -o "$BUILD_DIR/consumer_main.o" "$HERE/consumer_main.c"
# Static NetBSD/vax libc, pulled selectively by LINKVAX (.OLB = selective, the
# same policy build-vax-shareable-graph.sh uses for LIBVMS$SHR's own libc deps).
cp "$SYSROOT/usr/lib/libc.a" "$BUILD_DIR/libc.OLB"
# emutls.o from libgcc: VAX gcc lowers __thread (libc's errno) to EMULATED TLS
# via __emutls_get_address; printf's first-call buffer setup reaches errno, so
# this MUST be defined or the C-RTL channel faults. libgcc.a as a whole
# whole-archives at the wrong grain (MULDEF vs libc's compiler-support members,
# build-vax-shareable-graph.sh §libgcc); pull ONLY emutls.o into its own .OLB.
LIBGCC="$("$VAXCC" -print-libgcc-file-name)"
[ -f "$LIBGCC" ] || die "libgcc.a not found: $LIBGCC"
mkdir -p "$BUILD_DIR/gccbits"
( cd "$BUILD_DIR/gccbits" && "$VAXAR" x "$LIBGCC" emutls.o )
[ -f "$BUILD_DIR/gccbits/emutls.o" ] || die "emutls.o not present in $LIBGCC"
"$VAXAR" rcs "$BUILD_DIR/emutls.OLB" "$BUILD_DIR/gccbits/emutls.o"

CONSUMER="$OUT/CONSUMER.EXE"
# --allow-undefined: the statically pulled libc leaves NetBSD process-startup
# globals (__progname/__ps_strings/environ/_end) undefined -- not on printf's
# path, deferred to 0 exactly as build-vax-shareable-graph.sh defers them for
# LIBVMS$SHR. --use LIBVMS$SHR is the ONE producer; NO gate-private producer.
"$LINKVAX" --executable --allow-undefined --use "$LIBVMS_SHR" \
    -o "$CONSUMER" \
    "$BUILD_DIR/start_vax.o" "$BUILD_DIR/consumer_main.o" \
    "$BUILD_DIR/libc.OLB" "$BUILD_DIR/emutls.OLB" \
    > "$BUILD_DIR/consumer-link.log" 2>&1 \
    || { cat "$BUILD_DIR/consumer-link.log"; die "LINKVAX.EXE --executable failed building CONSUMER.EXE"; }
cat "$BUILD_DIR/consumer-link.log"
grep -q '%LINK-S-CREATED' "$BUILD_DIR/consumer-link.log" \
    || die "no %LINK-S-CREATED banner -- CONSUMER.EXE was not produced"
[ -s "$CONSUMER" ] || die "$CONSUMER missing/empty after a reported success"
echo "OK: $CONSUMER ($(stat -c%s "$CONSUMER") bytes)"
echo

echo "=== 4. readelf-shape assertions (the vms-099 P3a/P3b done-condition bar, restated for CONSUMER.EXE) ==="
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
# The whole point of the re-instrumentation: NO gate-private producer. The
# only cross-image producer named in .vms$imp must be the SHIPPED LIBVMS$SHR.
if strings "$IMPBIN" | grep -qF 'P4BOOT$SHR'; then
    die "CONSUMER.EXE .vms\$imp still references P4BOOT\$SHR.EXE -- the gate-private producer was not dropped"
fi
if "$VAXREADELF" -r "$CONSUMER" 2>/dev/null | grep -q "R_VAX"; then
    die "CONSUMER.EXE carries UNAPPLIED R_VAX_* dynamic relocations"
fi
echo "   OK: elf32-vax ET_DYN, PT_INTERP=IMGACT.EXE, .vms\$imp -> LIBVMS\$SHR.EXE ONLY (no P4BOOT\$SHR), no unapplied R_VAX_*"
echo

echo "=== 5. IMGACT.EXE (elf32-vax, -DOVMX_IMGACT=ON + BIND_TRACE, rd vms-73b2/vms-33b/vms-d4a) ==="
IMGACT_BUILD="$BUILD_DIR/imgact-cmake"
# rd vms-d4a option (a): -DOVMX_IMGACT_BIND_TRACE=ON makes THIS gate-private
# IMGACT emit the CONSUMER import bind (prod/base/cell/val on /dev/console). The
# shipped IMGACT.EXE (built elsewhere, default OFF) never carries this.
cmake -S "$SRC" -B "$IMGACT_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake" \
    -DOVMX_IMGACT=ON -DOVMX_IMGACT_BIND_TRACE=ON \
    -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF >/dev/null
cmake --build "$IMGACT_BUILD" --target imgact >/dev/null
IMGACT=$(find "$IMGACT_BUILD" -name IMGACT.EXE | head -1)
[ -n "$IMGACT" ] && [ -s "$IMGACT" ] || die "IMGACT.EXE(vax) did not build"
cp "$IMGACT" "$OUT/IMGACT.EXE"
echo "OK: $OUT/IMGACT.EXE ($(stat -c%s "$OUT/IMGACT.EXE") bytes)"
echo

echo "ALL SHR-ACTIVATION ARTIFACTS BUILT: $OUT"
ls -l "$OUT" | sed 's/^/   /'
