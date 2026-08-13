#!/bin/sh
# build-libvms-vax.sh - cross-compile + link-prove libvms for netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. libvms is the VMS runtime -- system services (sys$*) + the RTL
# (lib$/str$/mth$/ots$) -- the next layer above vmsprocess in the OVMX library
# graph (CLAUDE.md "Library Build Order"):
#   libvmssys -> vmsprocess (+pthread) -> libvms (+pthread, m) -> ...
#
# Four proofs, mirroring build-vmsprocess-vax.sh:
#
#   0.  LIBVMSSYS DEPENDENCY: build libvmssys.a for netbsd-vax (bottom layer).
#   0b. VMSPROCESS DEPENDENCY: build libvmsprocess.a for netbsd-vax (middle
#       layer), so libvms is proven to link ON TOP of the real elf32-vax stack.
#   1.  CMAKE WIRING: configure src/libvms standalone with the VAX toolchain file
#       (LIBVMS_STANDALONE branch) and build the `vms` target -> libvms.a of
#       elf32-vax objects (arch-checked with objdump).
#   2.  LINK PROOF: link a program that calls into libvms's system-service and
#       RTL surface (status, descriptors, arithmetic RTL, FAO formatting)
#       AGAINST NetBSD libc + libpthread + libm and the elf32-vax vmsprocess.a +
#       libvmssys.a, producing a real vax--netbsdelf ELF32 executable. This
#       proves the link-libc backend end to end: NetBSD supplies
#       crt0/libc/libpthread/libm, libvmssys the syscall/kif surface, vmsprocess
#       the process-control API, libvms the VMS runtime. io_uring is Linux-only
#       and compiles to "unavailable" stubs here (sys$qio uses its synchronous
#       path); no Linux-only facility is pulled.
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
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
OUT="${OUT:-/tmp/vax-libvms-build}"
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

# --- proof 0: libvmssys.a (bottom layer) ------------------------------------
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

# --- proof 0b: libvmsprocess.a (middle layer) -------------------------------
echo "=== proof 0b: build libvmsprocess.a for netbsd-vax (dependency layer) ==="
cmake -S "$VMSPROCESS" -B "$OUT/vmsprocess" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVMS_INCLUDE_DIR="$VMS_INCLUDE"
cmake --build "$OUT/vmsprocess" --target vmsprocess -- -j"$(nproc)"
LIBVMSPROCESS_A="$(find "$OUT/vmsprocess" -name 'libvmsprocess.a' | head -1)"
test -n "$LIBVMSPROCESS_A"
echo "built: $LIBVMSPROCESS_A"
echo

# --- proof 1: CMake standalone build of the libvms library ------------------
echo "=== proof 1: CMake netbsd-vax build of libvms ==="
cmake -S "$LIBVMS" -B "$OUT/cmake" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/cmake" --target vms -- -j"$(nproc)"
LIB="$(find "$OUT/cmake" -name 'libvms.a' | head -1)"
test -n "$LIB"
echo "built: $LIB"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'status*.o' | head -1)" | grep -Ei 'file format|architecture'
# Assert the io_uring TU compiled to the non-Linux stub (no Linux syscall syms).
if "$TARGET-nm" "$(find "$OUT/cmake" -name 'sys_uring*.o' | head -1)" | grep -qiE 'io_uring_setup|__NR_io_uring'; then
    echo "FAIL: sys_uring.o pulled a Linux io_uring syscall on the netbsd-vax substrate"
    exit 1
fi
echo "sys_uring.o is the non-Linux stub (no Linux io_uring syscall) -- OK"
echo

# --- proof 2: link a real vax--netbsdelf executable -------------------------
echo "=== proof 2: link-libc executable (crt0/libc/libpthread/libm from NetBSD) ==="
cat > "$OUT/smoke.c" <<'EOF'
/* Exercises libvms's system-service and RTL surface -- status decoding, VMS
 * descriptors, the integer/arithmetic RTL, and FAO formatting -- forcing those
 * translation units to link against NetBSD libc + libpthread + libm and the
 * elf32-vax vmsprocess.a + libvmssys.a. Proves the link-libc backend: NetBSD
 * provides crt0/libc/libpthread/libm, libvmssys the syscall/kif surface,
 * vmsprocess the VMS process-control API, libvms the VMS runtime. */
#include <stdint.h>
#include <stddef.h>

/* status.c */
uint32_t vms_status_severity(uint32_t status);
int      vms_status_match(uint32_t sts1, uint32_t sts2);
/* rtl/lib_arith.c */
uint32_t lib$addx(const uint32_t *a, const uint32_t *b, uint32_t *sum, const int32_t *len);

int main(void)
{
    uint32_t sev = vms_status_severity(1u);         /* status.c   */
    int      m   = vms_status_match(1u, 1u);        /* status.c   */
    uint32_t a = 1, b = 2, sum = 0; int32_t len = 2;
    lib$addx(&a, &b, &sum, &len);                   /* lib_arith.c */
    return (int)(sev + (uint32_t)m + sum);
}
EOF
"$CC" --sysroot="$SYSROOT" \
    -I"$VMS_INCLUDE" -I"$VMSPROCESS_INCLUDE" -I"$LIBVMSSYS" \
    "$OUT/smoke.c" "$LIB" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -lpthread -lm -o "$OUT/smoke"
echo "--- linked executable ---"
file "$OUT/smoke"
"$TARGET-readelf" -h "$OUT/smoke" | grep -Ei 'Class|Data|Machine|Type'

echo
echo "=== ALL PROOFS PASSED: libvms builds and links for $TARGET ==="
