#!/bin/sh
# build-vmslnm-vax.sh - cross-compile + link-prove vmslnm for netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. vmslnm is the Logical Name Manager -- the first userspace library
# above libvms in the OVMX library graph (CLAUDE.md "Library Build Order"):
#   ... libvms (+pthread, m) -> vmslnm (+pthread) -> vmsfs -> vmsrms -> vmsdcl
#
# Two proofs, mirroring build-libvms-vax.sh:
#
#   0.  LIBVMSSYS DEPENDENCY: build libvmssys.a for netbsd-vax. vmslnm routes
#       LNM$SYSTEM/GROUP/JOB through vms_kif_lnm_translate (the /dev/vms client
#       in libvmssys); the elf32-vax libvmssys.a resolves those at link time.
#   1.  CMAKE WIRING: configure src/vmslnm standalone with the VAX toolchain file
#       (VMSLNM_STANDALONE branch) and build the `vmslnm` target -> libvmslnm.a
#       of elf32-vax objects (arch-checked with objdump).
#   2.  LINK PROOF: link a program that calls into vmslnm (lnm_init + a routed
#       lnm_translate) AGAINST NetBSD libc + libpthread and the elf32-vax
#       libvmssys.a, producing a real vax--netbsdelf ELF32 executable. This
#       proves the logical-name manager cross-links ON TOP of the elf32-vax
#       executive-client layer with no Linux-only facility pulled.
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
VMSLNM="$SRC/src/vmslnm"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
OUT="${OUT:-/tmp/vax-vmslnm-build}"
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

# --- proof 0: libvmssys.a (executive-client layer) --------------------------
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

# --- proof 1: CMake standalone build of the vmslnm library ------------------
echo "=== proof 1: CMake netbsd-vax build of vmslnm ==="
cmake -S "$VMSLNM" -B "$OUT/cmake" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/cmake" --target vmslnm -- -j"$(nproc)"
LIB="$(find "$OUT/cmake" -name 'libvmslnm.a' | head -1)"
test -n "$LIB"
echo "built: $LIB"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'lnm_translate*.o' | head -1)" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf executable -------------------------
echo "=== proof 2: link-libc executable (crt0/libc/libpthread from NetBSD) ==="
cat > "$OUT/smoke.c" <<'EOF'
/* Exercises vmslnm's public surface -- manager init + a routed translate --
 * forcing lnm_translate.o (which references vms_kif_lnm_translate, the /dev/vms
 * executive client) to link against the elf32-vax libvmssys.a + NetBSD libc +
 * libpthread. Never executed (VAX cannot run on the amd64 host); the proof is
 * that a real vax--netbsdelf ELF32 links clean. */
#include <stdint.h>
#include <stddef.h>

void    *lnm_init(void);                             /* lnm_table.c   */
void     lnm_shutdown(void *mgr);                    /* lnm_table.c   */
uint32_t lnm_translate(void *mgr, const char *table, /* lnm_translate.c */
                       const char *name, uint32_t idx,
                       char *equiv, uint32_t *equiv_len, uint32_t *attr);

int main(void)
{
    void   *mgr = lnm_init();
    char    equiv[256]; uint32_t elen = 0, attr = 0;
    uint32_t s = lnm_translate(mgr, "LNM$SYSTEM", "SYS$SYSDEVICE",
                               0, equiv, &elen, &attr);
    lnm_shutdown(mgr);
    return (int)(s + elen);
}
EOF
"$CC" --sysroot="$SYSROOT" \
    -I"$VMS_INCLUDE" -I"$VMSLNM_INCLUDE" -I"$LIBVMSSYS" \
    "$OUT/smoke.c" "$LIB" "$LIBVMSSYS_A" \
    -lpthread -latomic -o "$OUT/smoke"
echo "--- linked executable ---"
file "$OUT/smoke"
"$TARGET-readelf" -h "$OUT/smoke" | grep -Ei 'Class|Data|Machine|Type'

echo
echo "=== ALL PROOFS PASSED: vmslnm builds and links for $TARGET ==="
