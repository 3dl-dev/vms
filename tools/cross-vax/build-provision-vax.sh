#!/bin/sh
# build-provision-vax.sh - cross-compile + link-prove ovmx_provision
# (PROVISION.EXE) for netbsd-vax (rd vms-5d1, epic vms-8e8, P4-C boot;
# docs/design-p4-netbsd-vax-boot.md §4.5).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host. PROVISION.EXE is the OVMX startup process (vms-9b7): PID 1
# (STARTUP.EXE) execs it to establish the SYSTEM identity from SYSUAF through
# the executive and provision the system tree, before execing DCL.EXE on
# SYS$MANAGER:STARTUP.COM in the same process -- it is a REQUIRED link in the
# boot chain STARTUP.EXE -> PROVISION.EXE -> DCL.EXE, not an optional image.
#
# PROVISION.EXE's fatal-halt path powers the machine off through
# ovmx_boot_power_off() -- the SAME boot-plumbing substrate seam (vms-28f)
# ovmx_init already uses for the identical need, reused here (not a second,
# drifting direct reboot(2) call) precisely because NetBSD's reboot(2) has a
# different signature/flag set than Linux's. src/ovmx_provision/CMakeLists.txt
# selects the matching backend object (ovmx_boot_netbsd.c here) the same way
# src/ovmx_init/CMakeLists.txt does; this script mirrors that CMake selection
# by hand for the cross build.
#
# Proofs, mirroring build-ovmx-init-vax.sh but topped with the PROVISION.EXE
# link:
#
#   0..0f. the elf32-vax dependency stack:
#          libvmssys.a, vmsprocess.a, libvms.a, vmslnm.a, vmsfs.a, vmsrms.a,
#          vmsqueue.a
#   1.  compile ovmx_provision.c + the NetBSD backend object
#       (ovmx_boot_netbsd.c) for elf32-vax.
#   2.  PROVISION.EXE LINK PROOF: link a real vax--netbsdelf ELF32 executable
#       against the whole elf32-vax stack + NetBSD libc + libpthread + libm +
#       libatomic. --start-group resolves the libvms<->vmsfs archive cycle.
#       Never executed (VAX cannot run on the amd64 host); the proof is a
#       clean vax--netbsdelf link of PROVISION.EXE.
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
OVMX_PROVISION="$SRC/src/ovmx_provision"
OVMX_INIT="$SRC/src/ovmx_init"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
OUT="${OUT:-/tmp/vax-provision-build}"
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

# --- common compile flags for ovmx_provision + its boot backend object ------
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I$OVMX_PROVISION -I$OVMX_INIT \
    -I$VMS_INCLUDE -I$VMSPROCESS_INCLUDE -I$VMSLNM_INCLUDE \
    -I$VMSFS_INCLUDE -I$VMSRMS_INCLUDE -I$LIBVMSSYS"

# --- proof 1: cross-compile ovmx_provision.c + the NetBSD boot backend ------
echo "=== proof 1: cross-compile ovmx_provision.c + ovmx_boot_netbsd.c ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_PROVISION/ovmx_provision.c" -o "$OUT/ovmx_provision.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_boot_netbsd.c"    -o "$OUT/ovmx_boot_netbsd.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/ovmx_provision.o" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf PROVISION.EXE over the whole stack -
echo "=== proof 2: link a real vax--netbsdelf PROVISION.EXE ==="
# Dynamic (no -static): same Decision A path (vms-42d) every other netbsd-vax
# OVMX image on this substrate takes.
"$CC" --sysroot="$SYSROOT" \
    "$OUT/ovmx_provision.o" "$OUT/ovmx_boot_netbsd.o" \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/PROVISION.EXE"
echo "--- linked executable ---"
file "$OUT/PROVISION.EXE"
"$TARGET-readelf" -h "$OUT/PROVISION.EXE" | grep -Ei 'Class|Data|Machine|Type'

HDR="$("$TARGET-readelf" -h "$OUT/PROVISION.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: PROVISION.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: PROVISION.EXE Machine is not Digital VAX"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: ovmx_provision (PROVISION.EXE) builds and links for $TARGET ==="
