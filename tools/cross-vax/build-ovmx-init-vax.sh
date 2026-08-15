#!/bin/sh
# build-ovmx-init-vax.sh - cross-compile + link-prove ovmx_init (STARTUP.EXE,
# PID 1) for netbsd-vax against the NetBSD boot backend (rd vms-f2e, epic
# vms-8e8, P4-C boot; docs/design-p4-netbsd-vax-boot.md §4.5).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host. ovmx_init is the boot orchestrator (STARTUP.EXE); it is the
# TOP of the boot bring-up chain and the LAST piece before the boot capstone
# (vms-5d1 -> vms-d59). It links the whole userspace library stack that the
# lower P4-C gates already cross-build (libvmssys -> vmsprocess -> libvms ->
# vmslnm -> vmsfs -> vmsrms/vmsqueue).
#
# THE POINT OF THIS GATE. The boot-plumbing seam (vms-28f) put every host-OS
# boot primitive PID 1 needs behind ovmx_boot.h; this proves the NetBSD
# realization of that seam (src/ovmx_init/ovmx_boot_netbsd.c) actually compiles
# and links ONE ovmx_init.c into a real elf32-vax STARTUP.EXE -- i.e. the boot
# orchestrator runs on the NetBSD/vax substrate through the substrate backend,
# with NO #ifdef fork of the boot logic in ovmx_init.c (INV-DRIFT). The boot
# activation model on this substrate is Decision A (ld.elf_so, rd vms-42d), so
# STARTUP.EXE is linked as an ordinary NetBSD ELF32-vax dynamic executable, the
# same path build-activation-vax.sh asserts.
#
# Proofs, mirroring build-vmsdcl-vax.sh but topped with the STARTUP.EXE link:
#
#   0..0f. the elf32-vax dependency stack:
#          libvmssys.a, vmsprocess.a, libvms.a, vmslnm.a, vmsfs.a, vmsrms.a,
#          vmsqueue.a
#   1.  INV-DRIFT: assert ovmx_init.c has NO `#if(def) __NetBSD__ / __linux__`
#       boot-logic fork -- the substrate split lives ONLY in the backend files.
#   2.  compile the three ovmx_init translation units for elf32-vax:
#         ovmx_init.c            (the shared boot orchestrator)
#         ovmx_boot_netbsd.c     (the NetBSD backend of the ovmx_boot.h seam)
#         sysboot.c              (SYSBOOT> prompt; NetBSD boot-flag-register twin)
#       opcom_kmsg.c is NOT built here -- it is the Linux /dev/kmsg bridge; the
#       NetBSD console-log twin lives in ovmx_boot_netbsd.c (/dev/klog).
#   3.  STARTUP.EXE LINK PROOF: link a real vax--netbsdelf ELF32 executable
#       against the whole elf32-vax stack + NetBSD libc + libpthread + libm +
#       libatomic. --start-group resolves the libvms<->vmsfs archive cycle.
#       Never executed (VAX cannot run on the amd64 host); the proof is a clean
#       vax--netbsdelf link of PID 1.
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
OVMX_INIT="$SRC/src/ovmx_init"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
OUT="${OUT:-/tmp/vax-ovmx-init-build}"
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
# vms-351 (epic vms-5eb "B2"): the genuine-ODS-2 reader sublibrary. PID 1 now
# registers the boot device into the userspace mounted-volume table
# (vmsfs_volume.c, compiled below), which links this. Built from the ods2
# standalone CMake project (src/vmsfs/ods2) exactly as the vmsfs stack above --
# it is NOT part of libvmsfs.a (the standalone src/vmsfs build omits the ods2
# sublibrary + vmsfs_volume; both live only in the non-standalone else branch).
LIBODS2_A="$(build_dep ods2 "$VMSFS/ods2" ods2 libods2.a)"
echo "dependency stack built:"
echo "  $LIBVMSSYS_A"
echo "  $LIBVMSPROCESS_A"
echo "  $LIBVMS_A"
echo "  $LIBVMSLNM_A"
echo "  $LIBVMSFS_A"
echo "  $LIBVMSRMS_A"
echo "  $LIBVMSQUEUE_A"
echo "  $LIBODS2_A"
echo

# --- proof 1: INV-DRIFT -- ovmx_init.c has no substrate #ifdef fork ----------
echo "=== proof 1: INV-DRIFT -- ovmx_init.c boot logic is substrate-neutral ==="
if grep -nE '#[[:space:]]*if(def)?[[:space:]].*(__NetBSD__|__linux__)' \
        "$OVMX_INIT/ovmx_init.c"; then
    echo "FAIL: ovmx_init.c carries a substrate #ifdef -- the boot sequence must"
    echo "      stay ONE source; the substrate split lives ONLY in the ovmx_boot"
    echo "      backend files (ovmx_boot_linux.c / ovmx_boot_netbsd.c)."
    exit 1
fi
echo "OK: ovmx_init.c has no __NetBSD__/__linux__ boot-logic fork"
echo

# --- common compile flags for the ovmx_init TUs ------------------------------
# _NETBSD_SOURCE exposes the NetBSD extensions the backend + PID 1 use
# (mount(2)/reboot(2)/sethostname(2), statvfs f_fstypename); _POSIX_C_SOURCE
# keeps the shared POSIX surface. The include set is exactly ovmx_init's CMake
# target include dirs, plus the local src/ovmx_init headers (ovmx_boot.h,
# sysboot.h).
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -D_POSIX_C_SOURCE=200809L \
    -I$OVMX_INIT \
    -I$VMS_INCLUDE -I$VMSPROCESS_INCLUDE -I$VMSLNM_INCLUDE \
    -I$VMSFS_INCLUDE -I$VMSRMS_INCLUDE -I$LIBVMSSYS"

# --- proof 2: compile the three ovmx_init translation units for elf32-vax ----
echo "=== proof 2: cross-compile ovmx_init.c + ovmx_boot_netbsd.c + sysboot.c ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_init.c"        -o "$OUT/ovmx_init.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_boot_netbsd.c" -o "$OUT/ovmx_boot_netbsd.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/sysboot.c"          -o "$OUT/sysboot.o"
# vms-351: the userspace mounted-volume table PID 1 registers the boot device
# into. A src/vmsfs TU compiled directly here (it is built by src/vmsfs's CMake
# only in the non-standalone tree, so the standalone libvmsfs.a above omits it);
# it links the ods2 reader sublibrary (LIBODS2_A) added to proof 3's link group.
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$VMSFS/vmsfs_volume.c"         -o "$OUT/vmsfs_volume.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/ovmx_boot_netbsd.o" | grep -Ei 'file format|architecture'

# The NetBSD backend must really define every ovmx_boot.h op (fail-honest, INV-6:
# a backend that fakes-out an op would drop a symbol here).
echo "--- confirm the NetBSD backend defines every ovmx_boot.h op ---"
for sym in ovmx_boot_kernel_filesystems_mounted ovmx_boot_mount_kernel_filesystems \
           ovmx_boot_start_console_log_bridge ovmx_boot_load_module \
           ovmx_boot_open_executive ovmx_boot_system_disk_dev \
           ovmx_boot_system_disk_present ovmx_boot_mount_system_disk \
           ovmx_boot_power_off; do
    if ! "$TARGET-nm" "$OUT/ovmx_boot_netbsd.o" | grep -qE " T $sym\$"; then
        echo "FAIL: ovmx_boot_netbsd.o does not define $sym"
        exit 1
    fi
done
echo "OK: all 9 ovmx_boot.h ops defined by the NetBSD backend"

# The backend must open the REAL executive device, not a faked descriptor
# (Rule 9 / INV-6). Assert the literal /dev/vms open is in the source.
if ! grep -qF 'open("/dev/vms", O_RDWR | O_CLOEXEC)' "$OVMX_INIT/ovmx_boot_netbsd.c"; then
    echo "FAIL: ovmx_boot_netbsd.c does not open the real /dev/vms executive device"
    exit 1
fi
echo "OK: ovmx_boot_open_executive() opens the real /dev/vms (fail-honest)"
echo

# --- proof 3: link a real vax--netbsdelf STARTUP.EXE over the whole stack ----
echo "=== proof 3: link a real vax--netbsdelf STARTUP.EXE (PID 1) ==="
# Dynamic (no -static): Decision A activates OVMX images on netbsd-vax through
# NetBSD's ld.elf_so (rd vms-42d), so PID 1 is a plain NetBSD ELF32-vax dynamic
# exe. --start-group resolves the libvms<->vmsfs archive cycle.
# vms-351: vmsfs_volume.o + libods2.a join the link (PID 1's boot-device
# registration). libods2.a goes in the --start-group with the rest of the
# archive stack so vmsfs_volume.o's ods2_bdev_* references resolve.
"$CC" --sysroot="$SYSROOT" \
    "$OUT/ovmx_init.o" "$OUT/ovmx_boot_netbsd.o" "$OUT/sysboot.o" \
    "$OUT/vmsfs_volume.o" \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" "$LIBODS2_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/STARTUP.EXE"
echo "--- linked executable ---"
file "$OUT/STARTUP.EXE"
"$TARGET-readelf" -h "$OUT/STARTUP.EXE" | grep -Ei 'Class|Data|Machine|Type'

# Hard assertions on the linked PID 1.
HDR="$("$TARGET-readelf" -h "$OUT/STARTUP.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: STARTUP.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: STARTUP.EXE Machine is not Digital VAX"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: ovmx_init (STARTUP.EXE) builds and links for $TARGET ==="
