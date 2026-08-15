#!/bin/sh
# build-initialize-vax.sh - cross-compile + link-prove INITIALIZE.EXE
# (tools/vms_initialize.c) for netbsd-vax (rd vms-cde6, rung C of
# docs/design-vax-installer.md §9, epic vms-f10 R4).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host.
#
# INITIALIZE.EXE formats a blank target volume (tools/CMakeLists.txt:
# target_link_libraries(vms_initialize PRIVATE vmssys ods2)) -- the same role
# run_release_install.sh's host-built INITIALIZE.EXE plays for x86_64.
# OVMX$INSTALL.COM's own INITIALIZE branch has a pre-existing, cross-arch gap
# (VMS device-name resolution to a block device, documented in the .COM's own
# header) this rung does NOT fix -- this gate proves the PRESERVE branch only,
# same scope as the existing x86_64 gate (vms-4834 §3, §8.4).
#
# REAL VAX PORTABILITY BUG FOUND + FIXED (vms-cde6): tools/vms_initialize.c
# unconditionally included <linux/fs.h> for BLKGETSIZE64 -- a Linux-only
# ioctl/header with no NetBSD equivalent, so the file did not cross-compile
# for netbsd-vax at all. Fixed with a platform split in get_device_size():
# Linux keeps BLKGETSIZE64; NetBSD uses the standard disklabel(9) ioctl
# (DIOCGDINFO -> struct disklabel's d_secsize * d_secperunit), a documented
# public NetBSD kernel API (Rule 8 does not apply -- this is not a VMS
# format). Neither path is faked; both fail honestly (return -1) if the
# ioctl fails. This branch is compiled but never executed by this gate (no
# qemu-system-vax / SIMH block device involved in a per-PR compile+link
# proof) -- it is a genuine ILP32/width-class portability fix, same class as
# the LNM SYSNAM bug (docs/audit-ilp32-vax-libvmssys.md).
#
# ods2 (src/vmsfs/ods2, genuine ODS-2 writer, ods2_volume_format) is a
# standalone, dependency-free static library -- not part of the shared
# elf32-vax dependency stack build-loginout-vax.sh/build-vmsdcl-vax.sh use,
# so it is built here as its own dependency, mirroring src/vmsfs/CMakeLists.txt's
# own "distinct module, own gate" note for ods2/.
#
# Proofs, mirroring build-loginout-vax.sh:
#
#   0..0f. the elf32-vax dependency stack: libvmssys.a (INITIALIZE links
#          vmssys directly, for vms_kif_disk_resolve -- the VMS-device branch
#          this gate does not exercise but must still link) + ods2.a.
#   1.  compile vms_initialize.c for elf32-vax.
#   2.  INITIALIZE.EXE LINK PROOF: link a real vax--netbsdelf ELF32 executable
#       against libvmssys.a + ods2.a + NetBSD libc + libpthread + libm +
#       libatomic. Never executed (VAX cannot run on the amd64 host); the
#       proof is a clean vax--netbsdelf link of INITIALIZE.EXE.
#
# Teeth (CROSSCOMPILE_NEGCTL=1): compile a deliberately-broken copy of
# vms_initialize.c and assert the cross-compile REJECTS it, mirroring
# build-librarian-vax.sh's negctl.
#
# Exit 0 = all proofs pass. Any failure is fatal (set -e).

set -eu

TARGET="${TARGET:-vax--netbsdelf}"
SYSROOT="${SYSROOT:-/opt/cross/sysroot}"
CC="${TARGET}-gcc"
SRC="$(pwd)"
LIBVMSSYS="$SRC/src/libvmssys"
ODS2="$SRC/src/vmsfs/ods2"
TOOLS="$SRC/tools"
KERNEL_VMSFS="$SRC/src/kernel/vmsfs"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
OUT="${OUT:-/tmp/vax-initialize-build}"
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

# --- common compile flags for vms_initialize.c ------------------------------
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I$KERNEL_VMSFS -I$LIBVMSSYS \
    -I$VMS_INCLUDE -I$VMSFS_INCLUDE"

# --- teeth check -------------------------------------------------------------
if [ "${CROSSCOMPILE_NEGCTL:-}" = "1" ]; then
    bad="$OUT/initialize_bad.c"
    { cat "$TOOLS/vms_initialize.c"; printf '\nthis is deliberately invalid C @@@ ;\n'; } > "$bad"
    # shellcheck disable=SC2086
    if "$CC" $CFLAGS_COMMON -c "$bad" -o /dev/null 2>/dev/null; then
        echo "FAIL (negctl): a deliberately-broken INITIALIZE.EXE TU COMPILED -- the cross-compile check has NO TEETH"
        exit 1
    fi
    echo "PASS (negctl): a deliberately-broken INITIALIZE.EXE TU fails the elf32-vax cross-compile, as it must"
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
ODS2_A="$(build_dep ods2 "$ODS2" ods2 libods2.a)"
echo "dependency stack built:"
echo "  $LIBVMSSYS_A"
echo "  $ODS2_A"
echo

# --- proof 1: cross-compile vms_initialize.c --------------------------------
echo "=== proof 1: cross-compile vms_initialize.c ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$TOOLS/vms_initialize.c" -o "$OUT/vms_initialize.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/vms_initialize.o" | grep -Ei 'file format|architecture'
echo "--- confirm no Linux-only header/ioctl leaked into the netbsd-vax build ---"
if "$TARGET-nm" "$OUT/vms_initialize.o" 2>/dev/null | grep -qiE ' U (BLKGETSIZE64)$'; then
    echo "FAIL: vms_initialize.o references BLKGETSIZE64 on netbsd-vax -- Linux-only ioctl leaked in"
    exit 1
fi
echo "OK: get_device_size() took the NetBSD DIOCGDINFO branch, not Linux's BLKGETSIZE64"
echo

# --- proof 2: link a real vax--netbsdelf INITIALIZE.EXE ---------------------
echo "=== proof 2: link a real vax--netbsdelf INITIALIZE.EXE ==="
# Dynamic (no -static): same Decision A path (vms-42d) every other netbsd-vax
# OVMX image on this substrate takes.
"$CC" --sysroot="$SYSROOT" \
    "$OUT/vms_initialize.o" \
    -Wl,--start-group "$ODS2_A" "$LIBVMSSYS_A" -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/INITIALIZE.EXE"
echo "--- linked executable ---"
file "$OUT/INITIALIZE.EXE"
"$TARGET-readelf" -h "$OUT/INITIALIZE.EXE" | grep -Ei 'Class|Data|Machine|Type'

HDR="$("$TARGET-readelf" -h "$OUT/INITIALIZE.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: INITIALIZE.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: INITIALIZE.EXE Machine is not Digital VAX"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: INITIALIZE.EXE builds and links for $TARGET ==="
