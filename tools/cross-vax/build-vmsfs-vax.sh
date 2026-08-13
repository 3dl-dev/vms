#!/bin/sh
# build-vmsfs-vax.sh - cross-compile + link-prove the userspace vmsfs library
# for netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. This is the USERSPACE vmsfs library (filespec translation, device
# table, file versioning, protection mapping) -- the layer above vmslnm in the
# OVMX library graph (CLAUDE.md "Library Build Order"):
#   ... libvms -> vmslnm (+pthread) -> vmsfs -> vmsrms -> vmsdcl
# NOT the kernel ODS-2 filesystem (src/kernel-core/vmsfs, cross-built by
# build-vmsfs-core-vax.sh) and NOT the genuine ODS-2 volume codec (src/vmsfs/ods2,
# its own gate) -- those are distinct modules.
#
# Three proofs, mirroring build-libvms-vax.sh:
#
#   0.  LIBVMSSYS DEPENDENCY: build libvmssys.a for netbsd-vax (executive-client
#       layer, reached transitively through vmslnm's vms_kif routing).
#   0b. VMSLNM DEPENDENCY: build libvmslnm.a for netbsd-vax. vmsfs's filespec
#       translation resolves logical names through lnm_translate* -- proving
#       vmsfs links ON TOP of the real elf32-vax vmslnm.a.
#   1.  CMAKE WIRING: configure src/vmsfs standalone with the VAX toolchain file
#       (VMSFS_STANDALONE branch) and build the `vmsfs` target -> libvmsfs.a of
#       elf32-vax objects (arch-checked with objdump).
#   2.  LINK PROOF: link a program that calls into vmsfs (filespec->linux path,
#       device count) AGAINST NetBSD libc + libpthread and the elf32-vax
#       vmslnm.a + libvmssys.a, producing a real vax--netbsdelf ELF32 executable.
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
VMSLNM="$SRC/src/vmslnm"
VMSFS="$SRC/src/vmsfs"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
OUT="${OUT:-/tmp/vax-vmsfs-build}"
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

# --- proof 0b: libvmslnm.a (logical name manager) ---------------------------
echo "=== proof 0b: build libvmslnm.a for netbsd-vax (dependency layer) ==="
cmake -S "$VMSLNM" -B "$OUT/vmslnm" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/vmslnm" --target vmslnm -- -j"$(nproc)"
LIBVMSLNM_A="$(find "$OUT/vmslnm" -name 'libvmslnm.a' | head -1)"
test -n "$LIBVMSLNM_A"
echo "built: $LIBVMSLNM_A"
echo

# --- proof 1: CMake standalone build of the vmsfs library -------------------
echo "=== proof 1: CMake netbsd-vax build of vmsfs (userspace) ==="
cmake -S "$VMSFS" -B "$OUT/cmake" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/cmake" --target vmsfs -- -j"$(nproc)"
LIB="$(find "$OUT/cmake" -name 'libvmsfs.a' | head -1)"
test -n "$LIB"
echo "built: $LIB"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'vmsfs_translate*.o' | head -1)" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf executable -------------------------
echo "=== proof 2: link-libc executable (crt0/libc/libpthread from NetBSD) ==="
cat > "$OUT/smoke.c" <<'EOF'
/* Exercises the userspace vmsfs surface -- filespec->Linux-path translation
 * (which resolves logical names through vmslnm) and the device table -- forcing
 * vmsfs_translate.o + vmsfs_device.o to link against the elf32-vax vmslnm.a +
 * libvmssys.a + NetBSD libc + libpthread. Never executed (VAX cannot run on the
 * amd64 host); the proof is that a real vax--netbsdelf ELF32 links clean. */
#include <stddef.h>

int vmsfs_to_linux_path(const char *vms_spec, char *linux_path, size_t path_size);
int vmsfs_device_count(void);

int main(void)
{
    char path[512];
    int r = vmsfs_to_linux_path("SYS$SYSDEVICE:[SYSMGR]LOGIN.COM", path, sizeof path);
    int n = vmsfs_device_count();
    return r + n;
}
EOF
"$CC" --sysroot="$SYSROOT" \
    -I"$VMS_INCLUDE" -I"$VMSLNM_INCLUDE" -I"$VMSFS_INCLUDE" -I"$LIBVMSSYS" \
    "$OUT/smoke.c" "$LIB" "$LIBVMSLNM_A" "$LIBVMSSYS_A" \
    -lpthread -latomic -o "$OUT/smoke"
echo "--- linked executable ---"
file "$OUT/smoke"
"$TARGET-readelf" -h "$OUT/smoke" | grep -Ei 'Class|Data|Machine|Type'

echo
echo "=== ALL PROOFS PASSED: vmsfs builds and links for $TARGET ==="
