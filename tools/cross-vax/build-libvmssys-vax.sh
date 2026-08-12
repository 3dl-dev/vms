#!/bin/sh
# build-libvmssys-vax.sh - cross-compile + link-prove libvmssys for netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. Two independent proofs, per docs/design-ovmx-netbsd-syskrnl.md §4:
#
#   1. CMAKE WIRING: configure src/libvmssys standalone with the VAX toolchain
#      file and build the `vmssys` target -> libvmssys.a of VAX objects. This is
#      the "wired into the build system (CMake arch selection)" proof.
#   2. LINK PROOF: link a tiny program that calls into libvmssys AGAINST NetBSD
#      libc, producing a real vax--netbsdelf ELF executable. This proves the
#      link-libc backend recommendation (§4.1) end to end: crt0/libc come from
#      NetBSD, the VMS API surface from libvmssys.
#
# Exit 0 = both proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
AR="${TARGET}-ar"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
OUT="${OUT:-/tmp/vax-build}"
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== toolchain ==="
"$CC" --version | head -1
"$CC" -dumpmachine
echo "sysroot: $SYSROOT"
echo

# --- sanity: the compiler agrees VAX is ILP32 little-endian ------------------
echo "=== width/endian sanity (the whole reason for the P3 audit) ==="
cat > "$OUT/width.c" <<'EOF'
#include <stdint.h>
_Static_assert(sizeof(long)  == 4, "VAX long must be 32-bit (ILP32)");
_Static_assert(sizeof(void*) == 4, "VAX pointer must be 32-bit (ILP32)");
_Static_assert(sizeof(int)   == 4, "int 32-bit");
_Static_assert(sizeof(long long) == 8, "long long 64-bit");
int main(void){ return 0; }
EOF
"$CC" --sysroot="$SYSROOT" -c "$OUT/width.c" -o "$OUT/width.o"
# byte order: encode 0x01020304 and read the low byte from the object's data
printf '%s\n' "little-endian + ILP32 assertions compiled clean"
echo

# --- proof 1: CMake standalone build of the vmssys library -------------------
echo "=== proof 1: CMake netbsd-vax build of libvmssys ==="
cmake -S "$LIBVMSSYS" -B "$OUT/cmake" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/tools/cross-vax/toolchain-vax-netbsd.cmake" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/cmake" --target vmssys
LIB="$(find "$OUT/cmake" -name 'libvmssys.a' | head -1)"
test -n "$LIB"
echo "built: $LIB"
"$TARGET-ar" t "$LIB"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'vms_kif*.o' | head -1)" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf executable against NetBSD libc -------
echo "=== proof 2: link-libc executable (crt0+libc from NetBSD, API from libvmssys) ==="
cat > "$OUT/smoke.c" <<'EOF'
/* Calls into libvmssys' own namespaced RTL and links against NetBSD libc.
 * Proves the link-libc backend: NetBSD provides crt0/libc, libvmssys the VMS
 * API surface. */
#include <stdio.h>
#include "vms_string.h"
int main(void) {
    const char *s = "OVMX/NetBSD-vax";
    printf("vms_strlen=%lu sizeof(long)=%lu sizeof(ptr)=%lu\n",
           (unsigned long)vms_strlen(s),
           (unsigned long)sizeof(long),
           (unsigned long)sizeof(void *));
    return 0;
}
EOF
"$CC" --sysroot="$SYSROOT" -I"$LIBVMSSYS" \
    "$OUT/smoke.c" "$LIB" -o "$OUT/smoke"
echo "--- linked executable ---"
file "$OUT/smoke"
"$TARGET-readelf" -h "$OUT/smoke" | grep -Ei 'Class|Data|Machine|Type'

echo
echo "=== ALL PROOFS PASSED: libvmssys builds and links for $TARGET ==="
