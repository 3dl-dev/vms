#!/bin/sh
# build-ovmx-init-vax-cmake.sh - Rung A of the unified cross-platform build
# (rd vms-75f, epic vms-509, docs/design-unified-cross-build.md §8). Proves
# the TOP-LEVEL CMake project configures under the vax toolchain file and
# `cmake --build --target ovmx_init` alone produces a real netbsd-vax
# STARTUP.EXE -- the CMake-native replacement path for build-ovmx-init-vax.sh,
# built ALONGSIDE it (both green; the hand script is not touched or retired
# here, only rung E retires it).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), same
# as every other tools/cross-vax/*.sh -- nothing installed on the build host
# (Rule 9).
#
# Asserts the Decision-A activation contract (rd vms-42d) directly against
# the CMake-produced binary, the same contract build-ovmx-init-vax.sh's proof
# 3 asserts by hand:
#   * ELF32, Digital VAX, dynamically linked
#   * PT_INTERP == /usr/libexec/ld.elf_so (NOT an OVMX IMGACT.EXE path)
#   * DT_NEEDED subset of {libc, libpthread, libm, libatomic}
#   * no .vms$sv / .vms$imp symbol-vector sections (this is not an OVMX-
#     native LINK.EXE image; ld.elf_so activates it, not IMGACT.EXE)
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

SRC="$(pwd)"
TARGET="${TARGET:-vax--netbsdelf}"
BUILD_DIR="${BUILD_DIR:-/tmp/build-vax-cmake}"
TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

rm -rf "$BUILD_DIR"

echo "=== configure: top-level project under the vax toolchain ==="
cmake -S "$SRC" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
echo

echo "=== build: cmake --build --target ovmx_init ==="
cmake --build "$BUILD_DIR" --target ovmx_init -- -j"$(nproc)"
echo

BIN="$BUILD_DIR/bin/STARTUP.EXE"
test -f "$BIN" || { echo "FAIL: $BIN was not produced"; exit 1; }

echo "=== linked executable ==="
file "$BIN"
"$TARGET-readelf" -h "$BIN" | grep -Ei 'Class|Data|Type|Machine'
echo

HDR="$("$TARGET-readelf" -h "$BIN")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: STARTUP.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: STARTUP.EXE Machine is not Digital VAX"; exit 1; }
echo "$HDR" | grep -qiE 'Type:[[:space:]]+EXEC' \
    || { echo "FAIL: STARTUP.EXE Type is not EXEC (dynamically linked exe)"; exit 1; }
echo "OK: ELF32 / Digital VAX / EXEC"

INTERP="$("$TARGET-readelf" -p .interp "$BIN" 2>/dev/null || true)"
echo "$INTERP" | grep -qF '/usr/libexec/ld.elf_so' \
    || { echo "FAIL: STARTUP.EXE PT_INTERP is not /usr/libexec/ld.elf_so"; exit 1; }
echo "OK: PT_INTERP == /usr/libexec/ld.elf_so (Decision-A, not IMGACT.EXE)"

NEEDED="$("$TARGET-readelf" -d "$BIN" | grep -i NEEDED || true)"
echo "$NEEDED"
if echo "$NEEDED" | grep -viE 'libc\.so|libpthread\.so|libm\.so|libatomic\.so' | grep -q .; then
    echo "FAIL: STARTUP.EXE has a NEEDED entry outside {libc,libpthread,libm,libatomic}"
    exit 1
fi
echo "OK: NEEDED set is a subset of {libc,libpthread,libm,libatomic}"

if "$TARGET-readelf" -S "$BIN" | grep -qi 'vms\$sv\|vms\$imp'; then
    echo "FAIL: STARTUP.EXE carries a .vms\$sv/.vms\$imp symbol-vector section -- Decision-A images are plain ld.elf_so dynamic exes, not OVMX-native LINK.EXE shareables"
    exit 1
fi
echo "OK: no .vms\$sv / .vms\$imp symbol-vector section"

echo
echo "=== ALL PROOFS PASSED: ovmx_init (STARTUP.EXE) builds via 'cmake --build --target ovmx_init' for $TARGET and passes the Decision-A activation contract ==="
