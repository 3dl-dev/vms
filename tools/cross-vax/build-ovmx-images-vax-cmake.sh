#!/bin/sh
# build-ovmx-images-vax-cmake.sh - Rung B of the unified cross-platform build
# (rd vms-64a, epic vms-509, docs/design-unified-cross-build.md §3-B/§8).
# Proves the TOP-LEVEL CMake project's `ovmx-images` aggregate target --
# the single authoritative "what ships" list -- configures and builds the
# FULL shipped userspace image set (not just STARTUP.EXE, rung A's scope)
# under tools/cross-vax/toolchain-vax-netbsd.cmake, and that every one of
# rd vms-e1d's 10 x86_64<->vax parity-drift images (HELP/AUTHORIZE/MAIL/
# MONITOR/INITIALIZE/INSTALL/SYSGEN/PRODUCT/PARTS -- SCSD excluded, see
# below) is now a real elf32-vax dynamic executable produced by
# `cmake --build --target ovmx-images`, closing the drift toward zero.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), same
# as every other tools/cross-vax/*.sh -- nothing installed on the build host
# (Rule 9). Built ALONGSIDE the rung A job and every existing per-image
# tools/cross-vax/build-*.sh (both/all green; retiring the hand scripts is
# rung E, not this job).
#
# SCSD.EXE (scsd_exe) is deliberately EXCLUDED from `ovmx-images` on the
# NetBSD substrate (CMakeLists.txt) -- it opens a Linux-only AF_PACKET raw
# socket with no NetBSD equivalent (NetBSD's analogue is bpf(4)), a genuine
# unclosed gap, not a script that was simply never written. This script does
# NOT assert SCSD.EXE exists; it is a named, reported exception (see this
# rung's PR body / docs/design-unified-cross-build.md follow-up), not a
# silent skip.
#
# LINK.EXE (vmslink) is also excluded on NetBSD -- it is the OVMX-native
# ELF64 linker that produces x86_64/aarch64 Mode-2 shareable images, a role
# with no meaning on vax (Decision-A images are ordinary NetBSD ld.elf_so
# dynamic executables, no LINK.EXE/IMGACT.EXE involved) -- the same
# "no VAX role by design" status IMGACT.EXE already carries in
# tools/parity/image-parity-allowlist.json. It was never in
# tools/cut-release.sh's VAX_ARTIFACT_ORDER either.
#
# Asserts the Decision-A activation contract (rd vms-42d) against every
# produced image:
#   * ELF32, Digital VAX, dynamically linked
#   * PT_INTERP == /usr/libexec/ld.elf_so (NOT an OVMX IMGACT.EXE path)
#   * DT_NEEDED subset of {libc, libpthread, libm, libatomic}
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

SRC="$(pwd)"
TARGET="${TARGET:-vax--netbsdelf}"
BUILD_DIR="${BUILD_DIR:-/tmp/build-vax-images-cmake}"
TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

rm -rf "$BUILD_DIR"

echo "=== configure: top-level project under the vax toolchain ==="
cmake -S "$SRC" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
echo

echo "=== build: cmake --build --target ovmx-images ==="
cmake --build "$BUILD_DIR" --target ovmx-images -- -j"$(nproc)"
echo

# The full shipped-image set ovmx-images builds on this substrate (SCSD.EXE
# and LINK.EXE are deliberately excluded on NetBSD -- see header). Boot set
# + LIBRARIAN.EXE are rung A/C's existing scope, carried here too so one job
# proves the whole aggregate; the ten names marked DRIFT are rd vms-e1d's
# parity-drift image set this rung closes.
IMAGES="STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE LIBRARIAN.EXE OVMXDUMP HELP.EXE AUTHORIZE.EXE MAIL.EXE MONITOR.EXE INITIALIZE.EXE INSTALL.EXE SYSGEN.EXE PRODUCT.EXE PARTS.EXE"
DRIFT_IMAGES="HELP.EXE AUTHORIZE.EXE MAIL.EXE MONITOR.EXE INITIALIZE.EXE INSTALL.EXE SYSGEN.EXE PRODUCT.EXE PARTS.EXE"

FAIL=0
for img in $IMAGES; do
    BIN="$BUILD_DIR/bin/$img"
    if [ ! -f "$BIN" ]; then
        echo "FAIL: $img was not produced at $BIN"
        FAIL=1
        continue
    fi

    HDR="$("$TARGET-readelf" -h "$BIN")"
    OK=1
    echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' || OK=0
    echo "$HDR" | grep -qiF 'Digital VAX' || OK=0
    echo "$HDR" | grep -qiE 'Type:[[:space:]]+EXEC' || OK=0

    INTERP="$("$TARGET-readelf" -p .interp "$BIN" 2>/dev/null || true)"
    echo "$INTERP" | grep -qF '/usr/libexec/ld.elf_so' || OK=0

    NEEDED="$("$TARGET-readelf" -d "$BIN" | grep -i NEEDED || true)"
    if echo "$NEEDED" | grep -viE 'libc\.so|libpthread\.so|libm\.so|libatomic\.so' | grep -q .; then
        OK=0
    fi

    if [ "$OK" -eq 1 ]; then
        echo "-> $img: ELF32 Digital VAX, dynamically linked, interp=/usr/libexec/ld.elf_so"
    else
        echo "FAIL: $img did not pass the Decision-A activation contract"
        echo "$HDR"
        echo "interp: $INTERP"
        echo "needed: $NEEDED"
        FAIL=1
    fi
done

if [ "$FAIL" -ne 0 ]; then
    echo "FAIL: one or more ovmx-images images failed the activation contract"
    exit 1
fi

echo
echo "=== drift closure (rd vms-e1d) ==="
for img in $DRIFT_IMAGES; do
    echo "DRIFT-CLOSED: $img"
done
echo "SCSD.EXE excluded (Linux-only AF_PACKET, no NetBSD BPF port -- reported gap, not silent skip)"

echo
echo "=== ALL PROOFS PASSED: ovmx-images builds the full shipped image set via 'cmake --build --target ovmx-images' for $TARGET and every image passes the Decision-A activation contract ==="
