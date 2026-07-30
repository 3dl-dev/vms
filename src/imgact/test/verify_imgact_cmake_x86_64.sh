#!/bin/sh
# verify_imgact_cmake_x86_64.sh — x86_64 build-integration proof for the
# OVMX_IMGACT CMake build mode (bead vms-be5; x86_64 analogue of
# verify_imgact_cmake.sh, which covers aarch64).
#
# Runs INSIDE a NATIVE x86_64 musl Alpine container (GitHub runners are x86_64,
# so this needs NO emulation — unlike the aarch64 jobs). Configures + builds the
# whole tree with -DOVMX_IMGACT=ON using the native x86_64 musl gcc, then
# asserts, via readelf, that the produced binaries are laid out for the x86_64
# IMGACT.EXE backend (src/imgact/arch/x86_64, bead vms-913.11):
#   * IMGACT.EXE  is a static-PIE ET_DYN with RELATIVE-only relocs, no PT_INTERP
#   * STARTUP.EXE is static: no PT_INTERP, no DT_NEEDED
#   * executables carry PT_INTERP = IMGACT.EXE and use DT_HASH (SysV)
#   * shareable images carry a VMS SONAME and use DT_HASH (SysV)
#   * NO object anywhere uses GNU_HASH (IMGACT implements SysV hash only)
#   * TLS uses the TLSDESC dialect (-mtls-dialect=gnu2, bead vms-be5): at least
#     one R_X86_64_TLSDESC appears and NO R_X86_64_DTPMOD64/DTPOFF64 (the
#     general-dynamic __tls_get_addr model) appears anywhere. The x86_64 IMGACT
#     backend resolves ONLY TLSDESC; a GD reloc would fault at activation.
#
# Exit 0 only if every assertion holds. CI gate (rule 10): the x86_64
# OVMX_IMGACT build mode must stay green and correctly formed.

set -e

CC=${CC:-gcc}
READELF=${READELF:-readelf}
SRC=$(cd "$(dirname "$0")/../../.." && pwd)   # repo root
BUILD=${BUILD:-/tmp/imgact-build-x86_64}
IMGACT_PATH="/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== toolchain (must be x86_64 musl) =="
$CC -dumpmachine
echo

echo "== configure + build (OVMX_IMGACT=ON, x86_64 musl) =="
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug \
      -DOVMX_IMGACT=ON -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON >/dev/null
cmake --build "$BUILD" -j"$(nproc)" >/dev/null
echo "build OK"
echo

BIN="$BUILD/bin"
LIB="$BUILD/lib"

echo "== IMGACT.EXE: static-PIE ET_DYN, RELATIVE-only, no PT_INTERP =="
$READELF -hW "$BIN/IMGACT.EXE" | grep -qE "Type:\s+DYN" || fail "IMGACT.EXE not ET_DYN"
$READELF -lW "$BIN/IMGACT.EXE" | grep -qi interpreter && fail "IMGACT.EXE has PT_INTERP"
NONREL=$($READELF -rW "$BIN/IMGACT.EXE" | awk '/R_X86_64/ && $3 !~ /RELATIVE/' | wc -l)
[ "$NONREL" -eq 0 ] || fail "IMGACT.EXE has non-RELATIVE relocations"
echo "  ok"

echo "== STARTUP.EXE: static (no PT_INTERP, no DT_NEEDED) =="
$READELF -lW "$BIN/STARTUP.EXE" | grep -qi interpreter && fail "STARTUP.EXE has PT_INTERP"
$READELF -dW "$BIN/STARTUP.EXE" 2>/dev/null | grep -q NEEDED && fail "STARTUP.EXE has DT_NEEDED"
echo "  ok"

echo "== executables: PT_INTERP=IMGACT.EXE, DT_HASH =="
for exe in DCL.EXE LOGINOUT.EXE HELP.EXE; do
    [ -f "$BIN/$exe" ] || fail "$exe not built"
    $READELF -lW "$BIN/$exe" | grep -q "$IMGACT_PATH" \
        || fail "$exe PT_INTERP != $IMGACT_PATH"
    $READELF -dW "$BIN/$exe" | grep -qE '\(HASH\)' || fail "$exe missing DT_HASH"
    echo "  $exe ok"
done

echo "== shareable images: VMS SONAME, DT_HASH =="
for shr in 'LIBVMS$SHR.EXE' 'LIBVMSFS$SHR.EXE' 'LIBVMSPROCESS$SHR.EXE'; do
    [ -f "$LIB/$shr" ] || fail "$shr not built"
    $READELF -dW "$LIB/$shr" | grep -q "SONAME.*$shr" || fail "$shr missing/wrong SONAME"
    $READELF -dW "$LIB/$shr" | grep -qE '\(HASH\)' || fail "$shr missing DT_HASH"
    echo "  $shr ok"
done

echo "== no GNU_HASH anywhere =="
for f in "$BIN"/*.EXE "$LIB"/*.EXE; do
    $READELF -dW "$f" 2>/dev/null | grep -q GNU_HASH && fail "GNU_HASH in $(basename "$f")"
done
echo "  ok"

echo "== TLS is TLSDESC (gnu2), NOT the general-dynamic __tls_get_addr model =="
# The x86_64 IMGACT backend (imgact_arch.h) resolves R_X86_64_TLSDESC only. A
# general-dynamic reloc (R_X86_64_DTPMOD64 / R_X86_64_DTPOFF64) would prove the
# -mtls-dialect=gnu2 flag did NOT take effect and would fault at activation.
GD=0; TLSDESC=0
for f in "$BIN"/*.EXE "$LIB"/*.EXE; do
    if $READELF -rW "$f" 2>/dev/null | grep -qE 'R_X86_64_DTPMOD64|R_X86_64_DTPOFF64'; then
        echo "  general-dynamic TLS reloc in $(basename "$f")"; GD=1
    fi
    $READELF -rW "$f" 2>/dev/null | grep -q 'R_X86_64_TLSDESC' && TLSDESC=1
done
[ "$GD" -eq 0 ] || fail "general-dynamic TLS relocs present — -mtls-dialect=gnu2 not applied"
[ "$TLSDESC" -eq 1 ] || fail "no R_X86_64_TLSDESC anywhere — expected TLS-using shareables to emit TLSDESC"
echo "  ok (TLSDESC present, no general-dynamic relocs)"

echo
echo "ALL OVMX_IMGACT x86_64 BUILD CHECKS PASSED"
