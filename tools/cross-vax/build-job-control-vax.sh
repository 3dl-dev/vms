#!/bin/sh
# build-job-control-vax.sh - cross-compile + link-prove ovmx_job_control
# (JOB_CONTROL.EXE) for netbsd-vax (rd vms-5d1, epic vms-8e8, P4-C boot;
# docs/design-p4-netbsd-vax-boot.md §4.5).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host. JOB_CONTROL.EXE is the OVMX console-session process
# (vms-8d2): created DETACHED by SYS$STARTUP:JOB_CONTROL_STARTUP.COM (run from
# STARTUP.COM as part of PROVISION.EXE's DCL session) and it in turn forks
# LOGINOUT.EXE on the console -- a REQUIRED link in the boot chain
# STARTUP.EXE -> PROVISION.EXE -> DCL.EXE (STARTUP.COM) -> JOB_CONTROL.EXE ->
# LOGINOUT.EXE -> DCL.EXE (interactive), not an optional image.
#
# JOB_CONTROL.EXE is an ORDINARY image (no boot-plumbing substrate seam
# dependency -- it holds no reboot(2)/mount(2) call of its own), so this
# script needs no NetBSD backend object, only the ordinary elf32-vax library
# stack.
#
# Proofs, mirroring build-vmsdcl-vax.sh but topped with the JOB_CONTROL.EXE
# link:
#
#   0..0f. the elf32-vax dependency stack:
#          libvmssys.a, vmsprocess.a, libvms.a, vmslnm.a, vmsfs.a, vmsrms.a,
#          vmsqueue.a
#   1.  compile ovmx_job_control.c for elf32-vax.
#   2.  JOB_CONTROL.EXE LINK PROOF: link a real vax--netbsdelf ELF32
#       executable against the whole elf32-vax stack + NetBSD libc +
#       libpthread + libm + libatomic. --start-group resolves the
#       libvms<->vmsfs archive cycle. Never executed (VAX cannot run on the
#       amd64 host); the proof is a clean vax--netbsdelf link of
#       JOB_CONTROL.EXE.
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
OVMX_JOB_CONTROL="$SRC/src/ovmx_job_control"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
OUT="${OUT:-/tmp/vax-job-control-build}"
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

# --- proof 1: cross-compile ovmx_job_control.c -------------------------------
echo "=== proof 1: cross-compile ovmx_job_control.c ==="
"$CC" --sysroot="$SYSROOT" -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I"$VMS_INCLUDE" -I"$VMSFS_INCLUDE" -I"$VMSLNM_INCLUDE" -I"$LIBVMSSYS" \
    -c "$OVMX_JOB_CONTROL/ovmx_job_control.c" -o "$OUT/ovmx_job_control.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/ovmx_job_control.o" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf JOB_CONTROL.EXE over the stack -----
echo "=== proof 2: link a real vax--netbsdelf JOB_CONTROL.EXE ==="
# Dynamic (no -static): same Decision A path (vms-42d) every other netbsd-vax
# OVMX image on this substrate takes.
"$CC" --sysroot="$SYSROOT" \
    "$OUT/ovmx_job_control.o" \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/JOB_CONTROL.EXE"
echo "--- linked executable ---"
file "$OUT/JOB_CONTROL.EXE"
"$TARGET-readelf" -h "$OUT/JOB_CONTROL.EXE" | grep -Ei 'Class|Data|Machine|Type'

HDR="$("$TARGET-readelf" -h "$OUT/JOB_CONTROL.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: JOB_CONTROL.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: JOB_CONTROL.EXE Machine is not Digital VAX"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: ovmx_job_control (JOB_CONTROL.EXE) builds and links for $TARGET ==="
