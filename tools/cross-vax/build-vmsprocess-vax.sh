#!/bin/sh
# build-vmsprocess-vax.sh - cross-compile + link-prove vmsprocess for netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. vmsprocess is the layer above libvmssys in the OVMX library graph
# (CLAUDE.md "Library Build Order"): libvmssys -> vmsprocess (+pthread) -> ...
#
# Three proofs, mirroring build-libvmssys-vax.sh:
#
#   0. LIBVMSSYS DEPENDENCY: build libvmssys.a for netbsd-vax (the layer below),
#      so vmsprocess is proven to link ON TOP of the real elf32-vax libvmssys.
#   1. CMAKE WIRING: configure src/vmsprocess standalone with the VAX toolchain
#      file (VMSPROCESS_STANDALONE branch) and build the `vmsprocess` target ->
#      libvmsprocess.a of elf32-vax objects.
#   2. LINK PROOF: link a program that calls into all four vmsprocess translation
#      units (PCB, process identity, ASTs, access modes) AGAINST NetBSD libc +
#      libpthread and the elf32-vax libvmssys.a, producing a real vax--netbsdelf
#      ELF32 executable. This proves the link-libc backend end to end: NetBSD
#      supplies crt0/libc/libpthread/signal, libvmssys the syscall/kif surface,
#      vmsprocess the VMS process-control API. No Linux-only wait primitive is
#      pulled (AST delivery is POSIX signals; event flags live in the executive).
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
VMSPROCESS="$SRC/src/vmsprocess"
VMS_INCLUDE="$SRC/src/libvms/include"
OUT="${OUT:-/tmp/vax-vmsprocess-build}"
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

# --- proof 0: libvmssys.a (the layer below) ---------------------------------
echo "=== proof 0: build libvmssys.a for netbsd-vax (dependency layer) ==="
cmake -S "$LIBVMSSYS" -B "$OUT/libvmssys" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/libvmssys" --target vmssys -- -j"$(nproc)"
LIBVMSSYS_A="$(find "$OUT/libvmssys" -name 'libvmssys.a' | head -1)"
test -n "$LIBVMSSYS_A"
echo "built: $LIBVMSSYS_A"
echo

# --- proof 1: CMake standalone build of the vmsprocess library --------------
echo "=== proof 1: CMake netbsd-vax build of vmsprocess ==="
cmake -S "$VMSPROCESS" -B "$OUT/cmake" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVMS_INCLUDE_DIR="$VMS_INCLUDE"
cmake --build "$OUT/cmake" --target vmsprocess -- -j"$(nproc)"
LIB="$(find "$OUT/cmake" -name 'libvmsprocess.a' | head -1)"
test -n "$LIB"
echo "built: $LIB"
"$TARGET-ar" t "$LIB"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'vms_pcb*.o' | head -1)" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf executable -------------------------
echo "=== proof 2: link-libc executable (crt0/libc/libpthread from NetBSD) ==="
cat > "$OUT/smoke.c" <<'EOF'
/* References one symbol from each vmsprocess translation unit, forcing the whole
 * library to link against NetBSD libc + libpthread and the elf32-vax libvmssys.
 * Proves the link-libc backend: NetBSD provides crt0/libc/libpthread/signal,
 * libvmssys the syscall/kif surface, vmsprocess the VMS process-control API. */
#include <stdint.h>
#include <stddef.h>
#include "vms/pcb.h"      /* vms_pcb_init          (vms_pcb.c)      */
#include "vms/process.h"  /* vms_format_uic        (vms_process.c) */
#include "vms/ast.h"      /* ast_init/ast_pending  (ast.c)         */

/* access_modes.c has no public header; declare the two entry points used. */
uint8_t  access_mode_get(void);
int      priv_check(uint64_t priv);

int main(void)
{
    char buf[32];

    vms_pcb_init(0);                        /* vms_pcb.c     */
    vms_format_uic(0x00010002u, buf, sizeof buf); /* vms_process.c */
    ast_init();                             /* ast.c         */
    (void)ast_pending_count();
    (void)priv_check(0);                    /* access_modes.c */
    return (int)access_mode_get();
}
EOF
"$CC" --sysroot="$SYSROOT" \
    -I"$VMSPROCESS/include" -I"$VMS_INCLUDE" -I"$LIBVMSSYS" \
    "$OUT/smoke.c" "$LIB" "$LIBVMSSYS_A" -lpthread -o "$OUT/smoke"
echo "--- linked executable ---"
file "$OUT/smoke"
"$TARGET-readelf" -h "$OUT/smoke" | grep -Ei 'Class|Data|Machine|Type'

echo
echo "=== ALL PROOFS PASSED: vmsprocess builds and links for $TARGET ==="
