#!/bin/sh
# build-facility-tools-vax-cmake.sh - Rung D of the unified cross-platform
# build (rd vms-74b, epic vms-509, docs/design-unified-cross-build.md §5/§8).
#
# Proves the TOP-LEVEL CMake project's `ovmx-netbsd-guest-tools` aggregate
# target (tests/netbsd/guest/CMakeLists.txt) -- the CMake-native replacement
# for the four hand-maintained
# tools/cross-vax/build-{eflag,access,proctab,mbx}-tool-vax.sh scripts --
# cross-compiles + links all four facility guest test tools
# (vmseflag/vmsaccess/vmsproctab/vmsmbx) for netbsd-vax as real elf32-vax
# executables, AND that the width-safety negctl teeth those scripts used to
# hand-roll (CROSSCOMPILE_NEGCTL=1) still genuinely reject an LP64-width TU
# under the vax cross-compiler, now expressed as ctest build-negatives.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), same
# as every other tools/cross-vax/*.sh -- nothing installed on the build host
# (Rule 9). Uses its OWN build directory (separate from
# build-ovmx-images-vax-cmake.sh's), so it does not touch the ovmx-images /
# vax-cmake-images job's build tree or scope.
#
# Exit 0 = all proofs pass (4 positive builds, elf32-vax each, + 4 negctl
# teeth that genuinely fail to compile). Any failure is fatal (set -e).

set -eu

SRC="$(pwd)"
TARGET="${TARGET:-vax--netbsdelf}"
BUILD_DIR="${BUILD_DIR:-/tmp/build-vax-facility-tools-cmake}"
TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

rm -rf "$BUILD_DIR"

echo "=== configure: top-level project under the vax toolchain ==="
cmake -S "$SRC" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
echo

echo "=== build: cmake --build --target ovmx-netbsd-guest-tools ==="
cmake --build "$BUILD_DIR" --target ovmx-netbsd-guest-tools -- -j"$(nproc)"
echo

# --- positive proof: every guest tool is a real elf32-vax executable -------
TOOLS="vmseflag vmsaccess vmsproctab vmsmbx"
FAIL=0
for tool in $TOOLS; do
    # CMAKE_RUNTIME_OUTPUT_DIRECTORY (top-level CMakeLists.txt) collects every
    # executable target into <build>/bin, same as ovmx-images.
    BIN="$BUILD_DIR/bin/$tool"
    if [ ! -f "$BIN" ]; then
        echo "FAIL: $tool was not produced at $BIN"
        FAIL=1
        continue
    fi
    FMT="$("$TARGET-objdump" -f "$BIN" | grep -Ei 'file format' || true)"
    if echo "$FMT" | grep -qiF 'elf32-vax'; then
        echo "-> $tool: $FMT"
    else
        echo "FAIL: $tool was not built elf32-vax ($FMT)"
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo "FAIL: one or more facility guest tools failed the elf32-vax positive build"
    exit 1
fi

echo
echo "=== teeth: ctest build-negatives (width negctl, one per facility) ==="
# enable_testing()+add_test() are called directly in
# tests/netbsd/guest/CMakeLists.txt (independent of the top-level
# BUILD_TESTS flag, which stays OFF for this cross-configure -- see that
# file's header) -- so CTestTestfile.cmake is generated in THIS build
# subdirectory regardless. Run ctest from there directly rather than from
# the top build dir, which sidesteps any ambiguity about whether the
# top-level project also called enable_testing().
GUEST_BUILD_DIR="$BUILD_DIR/tests/netbsd/guest"
test -f "$GUEST_BUILD_DIR/CTestTestfile.cmake" \
    || { echo "FAIL: $GUEST_BUILD_DIR/CTestTestfile.cmake missing -- negctl tests were not registered"; exit 1; }
if ! (cd "$GUEST_BUILD_DIR" && ctest -R '_width_negctl_vax$' --output-on-failure); then
    echo "FAIL: one or more facility width negctl teeth did not fire"
    exit 1
fi

echo
echo "ALL PROOFS PASSED: ovmx-netbsd-guest-tools builds vmseflag/vmsaccess/vmsproctab/vmsmbx for $TARGET (real elf32-vax executables) and every facility's width negctl teeth genuinely reject the LP64 TU"
