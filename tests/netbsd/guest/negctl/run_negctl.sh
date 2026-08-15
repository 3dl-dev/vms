#!/bin/sh
# run_negctl.sh - CMake/ctest build-negative runner for the vax facility
# guest-tool width-safety teeth (vms-74b, epic vms-509 "unified
# cross-platform build" Rung D, docs/design-unified-cross-build.md §5/§8).
#
# Re-expresses the CROSSCOMPILE_NEGCTL=1 shell teeth formerly hand-rolled in
# each tools/cross-vax/build-{eflag,access,proctab,mbx}-tool-vax.sh: proves
# the vax--netbsdelf cross-compiler genuinely REJECTS a deliberately-broken
# TU that pretends `long' is 64-bit (an LP64 assumption false on ILP32 vax).
# ctest FAILS (nonzero exit) unless:
#   1. the compile of the negctl fixture ACTUALLY fails (a facility whose
#      width gate cannot fail is a LARP gate, INV-6), AND
#   2. the compiler's diagnostic actually names the static assertion / an
#      error (so a spurious, unrelated compile failure -- e.g. a missing
#      header -- cannot masquerade as the width rejection).
#
# Invoked by tests/netbsd/guest/CMakeLists.txt via add_test(), one instance
# per facility, with CMAKE_C_COMPILER/CMAKE_SYSROOT resolved at configure
# time from the active toolchain file (tools/cross-vax/toolchain-vax-netbsd.cmake).
#
# Clean-room (CLAUDE.md Rule 8): OVMX's own build glue over a stock gcc. No
# NetBSD or VSI source is copied.
#
# Usage: run_negctl.sh <CC> <sysroot-or-empty> <facility-label> <negctl.c> <outdir>

set -eu

CC="$1"
SYSROOT="$2"
FACILITY="$3"
SRC="$4"
OUTDIR="$5"

test -f "$SRC" || { echo "FAIL ($FACILITY negctl): fixture not found: $SRC"; exit 1; }

rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

SYSROOT_FLAG=""
if [ -n "$SYSROOT" ]; then
    SYSROOT_FLAG="--sysroot=$SYSROOT"
fi

echo "=== NEGATIVE CONTROL ($FACILITY): a deliberately-broken LP64 TU must FAIL the vax cross-compile ==="

# shellcheck disable=SC2086
if "$CC" $SYSROOT_FLAG -c "$SRC" -o "$OUTDIR/${FACILITY}_negctl.o" 2>"$OUTDIR/negctl.err"; then
    echo "FAIL ($FACILITY negctl): the LP64 width assertion compiled clean on vax -- no teeth"
    cat "$OUTDIR/negctl.err" 2>/dev/null || true
    exit 1
fi

echo "--- rejected as expected (compiler diagnostics) ---"
cat "$OUTDIR/negctl.err"

if ! grep -iE 'static.?assert|error' "$OUTDIR/negctl.err" >/dev/null 2>&1; then
    echo "FAIL ($FACILITY negctl): compile failed, but not with the expected static-assert/error diagnostic -- a different failure may be masquerading as the width rejection"
    exit 1
fi

echo "PASS (negctl): the LP64 width assumption was REJECTED by the vax--netbsdelf cross-compile ($FACILITY)"
