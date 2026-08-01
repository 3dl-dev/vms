#!/bin/sh
# verify_imgact_cmake.sh — proof that the OVMX_IMGACT CMake build mode
# (bead vms-913.3) produces correctly-laid-out image-activation binaries.
#
# Runs INSIDE a native-musl Alpine (aarch64) container. Configures + builds
# the whole tree with -DOVMX_IMGACT=ON, then asserts, via readelf, that:
#   * IMGACT.EXE   is a static-PIE ET_DYN with RELATIVE-only relocs, no INTERP
#   * STARTUP.EXE  is static: no PT_INTERP, no DT_NEEDED
#   * executables  carry PT_INTERP = IMGACT.EXE and use DT_HASH (SysV)
#   * shareable images carry a VMS SONAME and use DT_HASH (SysV)
#   * NO object anywhere uses GNU_HASH (IMGACT implements SysV hash only)
#
# Exit 0 only if every assertion holds. This is a CI gate (rule 10): the
# OVMX_IMGACT build mode must stay green and correctly formed.

set -e

CC=${CC:-gcc}
SRC=$(cd "$(dirname "$0")/../../.." && pwd)   # repo root
BUILD=${BUILD:-/tmp/imgact-build}
IMGACT_PATH="/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE"

fail() { echo "FAIL: $*" >&2; exit 1; }

echo "== configure + build (OVMX_IMGACT=ON, musl) =="
cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Debug \
      -DOVMX_IMGACT=ON -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON >/dev/null
cmake --build "$BUILD" -j"$(nproc)" >/dev/null
echo "build OK"
echo

BIN="$BUILD/bin"
LIB="$BUILD/lib"

echo "== IMGACT.EXE: static-PIE ET_DYN, RELATIVE-only, no PT_INTERP =="
readelf -hW "$BIN/IMGACT.EXE" | grep -qE "Type:\s+DYN" || fail "IMGACT.EXE not ET_DYN"
readelf -lW "$BIN/IMGACT.EXE" | grep -qi interpreter && fail "IMGACT.EXE has PT_INTERP"
NONREL=$(readelf -rW "$BIN/IMGACT.EXE" | awk '/R_AARCH64/ && $3 !~ /RELATIVE/' | wc -l)
[ "$NONREL" -eq 0 ] || fail "IMGACT.EXE has non-RELATIVE relocations"
echo "  ok"

echo "== STARTUP.EXE: static (no PT_INTERP, no DT_NEEDED) =="
readelf -lW "$BIN/STARTUP.EXE" | grep -qi interpreter && fail "STARTUP.EXE has PT_INTERP"
readelf -dW "$BIN/STARTUP.EXE" 2>/dev/null | grep -q NEEDED && fail "STARTUP.EXE has DT_NEEDED"
echo "  ok"

echo "== executables: PT_INTERP=IMGACT.EXE, DT_HASH =="
for exe in DCL.EXE LOGINOUT.EXE VMSLNMD.EXE; do
    [ -f "$BIN/$exe" ] || fail "$exe not built"
    readelf -lW "$BIN/$exe" | grep -q "$IMGACT_PATH" \
        || fail "$exe PT_INTERP != $IMGACT_PATH"
    readelf -dW "$BIN/$exe" | grep -qE '\(HASH\)' || fail "$exe missing DT_HASH"
    echo "  $exe ok"
done

echo "== shareable images: VMS SONAME, DT_HASH =="
for shr in 'LIBVMS$SHR.EXE' 'LIBVMSFS$SHR.EXE' 'LIBVMSPROCESS$SHR.EXE'; do
    [ -f "$LIB/$shr" ] || fail "$shr not built"
    readelf -dW "$LIB/$shr" | grep -q "SONAME.*$shr" || fail "$shr missing/wrong SONAME"
    readelf -dW "$LIB/$shr" | grep -qE '\(HASH\)' || fail "$shr missing DT_HASH"
    echo "  $shr ok"
done

echo "== no GNU_HASH anywhere =="
for f in "$BIN"/*.EXE "$LIB"/*.EXE; do
    readelf -dW "$f" 2>/dev/null | grep -q GNU_HASH && fail "GNU_HASH in $(basename "$f")"
done
echo "  ok"

echo
echo "ALL OVMX_IMGACT BUILD CHECKS PASSED"
