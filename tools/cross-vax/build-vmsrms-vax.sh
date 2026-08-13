#!/bin/sh
# build-vmsrms-vax.sh - cross-compile + link-prove vmsrms for netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. vmsrms is Record Management Services (sequential / relative / indexed
# file organizations) -- the top of the userspace library graph before DCL
# (CLAUDE.md "Library Build Order"):
#   ... libvms -> vmslnm -> vmsfs -> vmsrms -> vmsdcl
#
# RMS file I/O is ordinary POSIX (open/lseek/off_t/fcntl/dirent/fnmatch): the O_*
# flags resolve from the NetBSD sysroot's <fcntl.h> and off_t is the sysroot's
# LFS type -- no Linux syscall number and no Linux-numeric O_* constant is
# assumed. On-disk RMS layouts (record prefixes, VBN math) are byte counts and
# fixed-width offsets, ILP32-clean under the 32-bit VAX compiler; the RAB's
# in-memory `off_t _current_offset` is a host file position, not an on-disk
# field. (audit-ilp32-vax-libvmssys.md §11.)
#
# Six proofs, mirroring build-libvms-vax.sh but over the full stack:
#
#   0.  libvmssys.a   (syscall/kif layer)
#   0b. libvmsprocess.a (process-control layer)
#   0c. libvms.a      (VMS runtime: sys$*, lib$/str$/mth$/ots$ -- RMS calls
#                      lib$cvt_vectim for VMS binary time)
#   0d. libvmslnm.a   (logical name manager)
#   0e. libvmsfs.a    (userspace filespec/device layer -- RMS resolves specs
#                      through vmsfs_to_linux_path)
#   1.  CMAKE WIRING: configure src/vmsrms standalone with the VAX toolchain file
#       (VMSRMS_STANDALONE branch) -> libvmsrms.a of elf32-vax objects.
#   2.  LINK PROOF: link a program that calls into RMS ($OPEN/$PARSE) AGAINST the
#       whole elf32-vax stack + NetBSD libc + libpthread + libm, producing a real
#       vax--netbsdelf ELF32 executable. --start-group resolves the libvms<->vmsfs
#       archive cycle.
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
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
OUT="${OUT:-/tmp/vax-vmsrms-build}"
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

# --- proofs 0..0e: the elf32-vax dependency stack ---------------------------
LIBVMSSYS_A="$(build_dep libvmssys "$LIBVMSSYS" vmssys libvmssys.a)"
LIBVMSPROCESS_A="$(build_dep vmsprocess "$VMSPROCESS" vmsprocess libvmsprocess.a)"
LIBVMS_A="$(build_dep libvms "$LIBVMS" vms libvms.a)"
LIBVMSLNM_A="$(build_dep vmslnm "$VMSLNM" vmslnm libvmslnm.a)"
LIBVMSFS_A="$(build_dep vmsfs "$VMSFS" vmsfs libvmsfs.a)"
echo "dependency stack built:"
echo "  $LIBVMSSYS_A"
echo "  $LIBVMSPROCESS_A"
echo "  $LIBVMS_A"
echo "  $LIBVMSLNM_A"
echo "  $LIBVMSFS_A"
echo

# --- proof 1: CMake standalone build of the vmsrms library ------------------
echo "=== proof 1: CMake netbsd-vax build of vmsrms ==="
cmake -S "$VMSRMS" -B "$OUT/cmake" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/cmake" --target vmsrms -- -j"$(nproc)"
LIB="$(find "$OUT/cmake" -name 'libvmsrms.a' | head -1)"
test -n "$LIB"
echo "built: $LIB"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'rms_core*.o' | head -1)" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf executable -------------------------
echo "=== proof 2: link-libc executable over the full elf32-vax stack ==="
cat > "$OUT/smoke.c" <<'EOF'
/* Exercises RMS's file-service surface -- $PARSE (filespec -> NAM, pulls vmsfs)
 * and $OPEN (pulls rms_core + lib$cvt_vectim from libvms) -- forcing the RMS
 * objects to link against the whole elf32-vax stack (libvms + vmsfs + vmslnm +
 * vmsprocess + libvmssys) and NetBSD libc + libpthread + libm. Never executed
 * (VAX cannot run on the amd64 host); the proof is a clean vax--netbsdelf link.
 * The RMS entry points carry `$` (a GNU-C identifier char), matching rms/rms.h. */
#include <stdint.h>

uint32_t sys$parse(void *fab, void (*err)(void *), void (*suc)(void *));
uint32_t sys$open (void *fab, void (*err)(void *), void (*suc)(void *));

int main(void)
{
    uint32_t p = sys$parse((void *)0, 0, 0);   /* 0 = null fn-ptr (no AST) */
    uint32_t o = sys$open ((void *)0, 0, 0);
    return (int)(p + o);
}
EOF
"$CC" --sysroot="$SYSROOT" \
    -I"$VMS_INCLUDE" -I"$VMSPROCESS_INCLUDE" -I"$VMSLNM_INCLUDE" \
    -I"$VMSFS_INCLUDE" -I"$VMSRMS_INCLUDE" -I"$LIBVMSSYS" \
    "$OUT/smoke.c" \
    -Wl,--start-group \
        "$LIB" "$LIBVMS_A" "$LIBVMSFS_A" "$LIBVMSLNM_A" \
        "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/smoke"
echo "--- linked executable ---"
file "$OUT/smoke"
"$TARGET-readelf" -h "$OUT/smoke" | grep -Ei 'Class|Data|Machine|Type'

echo
echo "=== ALL PROOFS PASSED: vmsrms builds and links for $TARGET ==="
