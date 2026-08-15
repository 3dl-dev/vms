#!/bin/sh
# build-product-vax.sh - cross-compile + link-prove PRODUCT.EXE
# (src/product/product.c) for netbsd-vax (rd vms-69a, rung A of
# docs/design-vax-installer.md §9, epic vms-f10 R4).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host.
#
# PRODUCT.EXE is the OVMX PCSI-equivalent installer (vms-df9): OVMX$INSTALL.COM's
# menu RUNs it to lay a kit's payload onto the target device (PRODUCT INSTALL
# /DESTINATION=<dev>). It is portable C already used on x86_64/aarch64
# (src/product/CMakeLists.txt: target_link_libraries(vms_product PRIVATE vmsfs
# ovmx_kit_reader)) -- no new design, just the same static-link/Decision-A
# cross-build pattern the existing vax image scripts (build-vmsdcl-vax.sh,
# build-loginout-vax.sh, build-librarian-vax.sh) already established.
#
# ovmx_kit_reader.c (the shared kit open/validate/read module) is a
# dependency-free TU (only <stdint.h>/<stddef.h> + ovmx_kit_format.h -- see
# src/product/ovmx_kit_reader.h) -- compiled directly alongside product.c,
# same as the top-level CMakeLists.txt's own `ovmx_kit_reader` STATIC library
# does in-tree, not treated as part of the elf32-vax dependency stack below.
#
# Proofs, mirroring build-loginout-vax.sh:
#
#   0..0f. the elf32-vax dependency stack:
#          libvmssys.a, vmsprocess.a, libvms.a, vmslnm.a, vmsfs.a, vmsrms.a,
#          vmsqueue.a
#   1.  compile product.c + ovmx_kit_reader.c for elf32-vax.
#   2.  PRODUCT.EXE LINK PROOF: link a real vax--netbsdelf ELF32 executable
#       against the whole elf32-vax stack + NetBSD libc + libpthread + libm +
#       libatomic. --start-group resolves the libvms<->vmsfs archive cycle.
#       Never executed (VAX cannot run on the amd64 host); the proof is a
#       clean vax--netbsdelf link of PRODUCT.EXE.
#
# Teeth (CROSSCOMPILE_NEGCTL=1): compile a deliberately-broken copy of
# product.c and assert the cross-compile REJECTS it, mirroring
# build-librarian-vax.sh's negctl.
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
VMSPROCESS="$SRC/src/vmsprocess"
LIBVMS="$SRC/src/libvms"
VMSLNM="$SRC/src/vmslnm"
VMSFS="$SRC/src/vmsfs"
VMSRMS="$SRC/src/vmsrms"
VMSQUEUE="$SRC/src/vmsqueue"
PRODUCT="$SRC/src/product"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
OUT="${OUT:-/tmp/vax-product-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

MAKE_PROG="$(command -v make)"
TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake"
test -f "$TOOLCHAIN_FILE" || { echo "FAIL: toolchain file missing: $TOOLCHAIN_FILE"; exit 1; }

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo "sysroot: $SYSROOT"
cmake --version | head -1
echo

# --- common compile flags for product.c + ovmx_kit_reader.c ----------------
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I$PRODUCT \
    -I$VMS_INCLUDE -I$VMSFS_INCLUDE"

# --- teeth check -------------------------------------------------------------
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OUT/product_bad.c"
    { cat "$PRODUCT/product.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
    # shellcheck disable=SC2086
    if "$CC" $CFLAGS_COMMON -c "$bad" -o /dev/null 2>/dev/null; then
        echo "FAIL (negctl): a deliberately-broken PRODUCT.EXE TU COMPILED -- the cross-compile check has NO TEETH"
        exit 1
    fi
    echo "PASS (negctl): a deliberately-broken PRODUCT.EXE TU fails the elf32-vax cross-compile, as it must"
    exit 0
fi

build_dep() {  # <name> <src-dir> <cmake-target> <archive-basename> -> echoes .a path
    _name="$1"; _srcdir="$2"; _tgt="$3"; _ar="$4"
    echo "=== dependency: build $_ar for netbsd-vax ($_name) ===" 1>&2
    cmake -S "$_srcdir" -B "$OUT/$_name" -G "Unix Makefiles" \
        -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE=Release 1>&2
    cmake --build "$OUT/$_name" --target "$_tgt" -- -j"$(nproc)" 1>&2
    _path="$(find "$OUT/$_name" -name "$_ar" | head -1)"
    test -n "$_path" || { echo "FAIL: $_ar not built" 1>&2; exit 1; }
    echo "$_path"
}

# --- proofs 0..0f: the elf32-vax dependency stack ---------------------------
LIBVMSSYS_A="$(build_dep libvmssys "$LIBVMSSYS" vmssys libvmssys.a)"
LIBVMSPROCESS_A="$(build_dep vmsprocess "$VMSPROCESS" vmsprocess libvmsprocess.a)"
LIBVMS_A="$(build_dep libvms "$LIBVMS" vms libvms.a)"
LIBVMSLNM_A="$(build_dep vmslnm "$VMSLNM" vmslnm libvmslnm.a)"
LIBVMSFS_A="$(build_dep vmsfs "$VMSFS" vmsfs libvmsfs.a)"
LIBVMSRMS_A="$(build_dep vmsrms "$VMSRMS" vmsrms libvmsrms.a)"
LIBVMSQUEUE_A="$(build_dep vmsqueue "$VMSQUEUE" vmsqueue libvmsqueue.a)"
echo "dependency stack built:"
echo "  $LIBVMSSYS_A"
echo "  $LIBVMSPROCESS_A"
echo "  $LIBVMS_A"
echo "  $LIBVMSLNM_A"
echo "  $LIBVMSFS_A"
echo "  $LIBVMSRMS_A"
echo "  $LIBVMSQUEUE_A"
echo

# --- proof 1: cross-compile product.c + ovmx_kit_reader.c ------------------
echo "=== proof 1: cross-compile product.c + ovmx_kit_reader.c ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$PRODUCT/product.c"          -o "$OUT/product.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$PRODUCT/ovmx_kit_reader.c"  -o "$OUT/ovmx_kit_reader.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/product.o" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf PRODUCT.EXE over the whole stack --
echo "=== proof 2: link a real vax--netbsdelf PRODUCT.EXE ==="
# Dynamic (no -static): same Decision A path (vms-42d) every other netbsd-vax
# OVMX image on this substrate takes.
"$CC" --sysroot="$SYSROOT" \
    "$OUT/product.o" "$OUT/ovmx_kit_reader.o" \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/PRODUCT.EXE"
echo "--- linked executable ---"
file "$OUT/PRODUCT.EXE"
"$TARGET-readelf" -h "$OUT/PRODUCT.EXE" | grep -Ei 'Class|Data|Machine|Type'

HDR="$("$TARGET-readelf" -h "$OUT/PRODUCT.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: PRODUCT.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: PRODUCT.EXE Machine is not Digital VAX"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: PRODUCT.EXE builds and links for $TARGET ==="
