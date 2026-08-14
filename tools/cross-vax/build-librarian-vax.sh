#!/bin/sh
# build-librarian-vax.sh - cross-compile + link-prove LIBRARIAN.EXE
# (src/vmslink/librarian.c) for netbsd-vax (rd vms-9172, epic vms-f10; VAX as a
# first-class release platform -- build parity R1).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host.
#
# LIBRARIAN.EXE is the OVMX object-library archiver -- the self-host toolchain
# image (rd vms-ca9, #429) that mints and maintains .OLB object libraries which
# LINK.EXE consumes. It is a SYS$SYSTEM: image installed alongside LINK.EXE and
# OVMXDUMP (src/vmslink/CMakeLists.txt: OUTPUT_NAME "LIBRARIAN"), so for VAX to
# be a first-class platform in the ONE build/release stream the archiver must
# cross-compile for elf32-vax exactly as DCL.EXE / LOGINOUT / STARTUP already do
# (build-vmsdcl-vax.sh, build-loginout-vax.sh, build-ovmx-init-vax.sh). Without
# this gate a width- or portability-break in the librarian (or in the shared
# ovmx_olb.h on-disk .OLB layout it depends on) would only surface downstream;
# here it reds the PR.
#
# Unlike the DCL/LOGINOUT images, LIBRARIAN links NO OVMX runtime library
# (src/vmslink/CMakeLists.txt gives vmslibrarian no target_link_libraries): it
# is a standalone tool over ordinary POSIX (stdio/stdlib/string/ctype) plus two
# OVMX headers -- ovmx_olb.h (the .OLB on-disk record layout, src/vmslink/
# include) and ssdef.h (VMS status #defines, src/libvms/include). The proof is
# therefore a single-TU compile + a clean vax--netbsdelf link against NetBSD
# libc, mirroring the compile discipline the other image gates use.
#
# Proofs:
#   1. cross-compile librarian.c for elf32-vax (ILP32 width proof of the .OLB
#      record structs in ovmx_olb.h); assert the object is elf32-vax.
#   2. link a real vax--netbsdelf ELF32 LIBRARIAN.EXE against NetBSD libc;
#      assert ELFCLASS32 + Digital VAX.
#
# Teeth (CROSSCOMPILE_NEGCTL=1): compile a deliberately-broken copy of
# librarian.c and assert the cross-compile REJECTS it -- proving proof 1 has
# teeth (a build break really does red this gate), mirroring the negctl the
# tests/netbsd/crosscompile.sh and build-vms-module-vax.sh gates use.
#
# DELIBERATELY NOT emulation (no qemu-system-vax); build/test tooling, never a
# runtime (Rule 9). Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
VMSLINK="$SRC/src/vmslink"
VMSLINK_INCLUDE="$SRC/src/vmslink/include"
VMS_INCLUDE="$SRC/src/libvms/include"
OUT="${OUT:-/tmp/vax-librarian-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

# The same portability defines src/vmslink/CMakeLists.txt sets for vmslibrarian.
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I$VMSLINK_INCLUDE -I$VMS_INCLUDE"

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo "sysroot: $SYSROOT"
echo

# --- teeth check ------------------------------------------------------------
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OUT/librarian_bad.c"
    { cat "$VMSLINK/librarian.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
    # shellcheck disable=SC2086
    if "$CC" $CFLAGS_COMMON -c "$bad" -o /dev/null 2>/dev/null; then
        echo "FAIL (negctl): a deliberately-broken LIBRARIAN TU COMPILED -- the cross-compile check has NO TEETH"
        exit 1
    fi
    echo "PASS (negctl): a deliberately-broken LIBRARIAN TU fails the elf32-vax cross-compile, as it must"
    exit 0
fi

# --- proof 1: cross-compile librarian.c for elf32-vax -----------------------
echo "=== proof 1: cross-compile librarian.c for netbsd-vax (elf32-vax) ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$VMSLINK/librarian.c" -o "$OUT/librarian.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/librarian.o" | grep -Ei 'file format|architecture'
"$TARGET-objdump" -f "$OUT/librarian.o" | grep -qiF 'file format elf32-vax' \
    || { echo "FAIL: librarian.o was not elf32-vax -- wrong target"; exit 1; }
echo

# --- proof 2: link a real vax--netbsdelf LIBRARIAN.EXE ----------------------
echo "=== proof 2: link a real vax--netbsdelf LIBRARIAN.EXE ==="
# Dynamic (no -static): the same Decision A activation path (vms-42d) every
# other netbsd-vax OVMX image takes.
"$CC" --sysroot="$SYSROOT" "$OUT/librarian.o" -o "$OUT/LIBRARIAN.EXE"
echo "--- linked executable ---"
file "$OUT/LIBRARIAN.EXE"
"$TARGET-readelf" -h "$OUT/LIBRARIAN.EXE" | grep -Ei 'Class|Data|Machine|Type'

HDR="$("$TARGET-readelf" -h "$OUT/LIBRARIAN.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: LIBRARIAN.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: LIBRARIAN.EXE Machine is not Digital VAX"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: LIBRARIAN.EXE builds and links for $TARGET ==="
