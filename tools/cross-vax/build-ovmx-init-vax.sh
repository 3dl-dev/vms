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
echo "dependency stack built:"
echo "  $LIBVMSSYS_A"
echo "  $LIBVMSPROCESS_A"
echo "  $LIBVMS_A"
echo "  $LIBVMSLNM_A"
echo "  $LIBVMSFS_A"
echo "  $LIBVMSRMS_A"
echo "  $LIBVMSQUEUE_A"
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
#
# vms-329 (the coupled VAX ACP cutover): the shipped VAX PID 1 now reaches
# SYS$DISK through the executive Files-11 ACP, so this recipe defines BOTH
# capability macros and links the bridge translation units:
#
#   OVMX_HAVE_ACP        -- "$MOUNT/read SYS$DISK over the executive ACP".
#                           ovmx_boot_mount_system_disk_native() takes its ACP
#                           arm (vms_kif_acp_mount), which RETIRES the vmsfs.ko
#                           VFS mount of the boot unit: NetBSD's spec_vnops
#                           allows exactly ONE open of /dev/ra1c, so the ACP
#                           mount and a VFS mount are mutually exclusive. There
#                           is deliberately no fallback -- ACP or fail-honest.
#   OVMX_BOOT_ACP_BRIDGE -- "the ACP-read bridge TUs are in THIS link", so
#                           ovmx_init.c compiles stage_boot_images() and probes
#                           the installed-system marker over the ACP instead of
#                           stat()ing a POSIX path that no longer exists.
#
# _POSIX_C_SOURCE is DROPPED here (it was harmless while only the three base
# TUs compiled): src/ovmx_init/CMakeLists.txt selects the bare _NETBSD_SOURCE
# namespace for this substrate, and so does the standing ILP32 audit
# (build-acp-read-audit-vax.sh) -- defining _POSIX_C_SOURCE alongside it turns
# OFF the NetBSD surface the bridge's backend needs. Keeping all three recipes
# on ONE feature-test macro set is what makes the audit's green mean something
# for the shipped image.
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE \
    -DOVMX_HAVE_ACP -DOVMX_BOOT_ACP_BRIDGE=1 \
    -I$OVMX_INIT \
    -I$VMS_INCLUDE -I$VMSPROCESS_INCLUDE -I$VMSLNM_INCLUDE \
    -I$VMSFS_INCLUDE -I$VMSRMS_INCLUDE -I$LIBVMSSYS \
    -I$SRC/src/imgact -I$SRC/src/kernel"

# --- proof 2: compile the ovmx_init translation units for elf32-vax ----------
echo "=== proof 2: cross-compile ovmx_init.c + ovmx_boot_netbsd.c + sysboot.c ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_init.c"        -o "$OUT/ovmx_init.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_boot_netbsd.c" -o "$OUT/ovmx_boot_netbsd.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/sysboot.c"          -o "$OUT/sysboot.o"

# vms-329: the ACP-read bridge TUs. imgact_acp.c is the SAME ACP file-access
# walk IMGACT.EXE and the QEMU tests use; ovmx_boot_acp_read.c backs its three
# host primitives with libc for PID 1; ovmx_boot_sysgen_acp.c is the STRONG
# ovmx_sysgen_acp_read that must beat sysgen_params.h's #pragma-weak NULL
# default (proof 2b below is the weak-seam anchor that proves it did).
echo "=== proof 2a: cross-compile the ACP-read bridge TUs ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_boot_acp_read.c"   -o "$OUT/ovmx_boot_acp_read.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$OVMX_INIT/ovmx_boot_sysgen_acp.c" -o "$OUT/ovmx_boot_sysgen_acp.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$SRC/src/imgact/imgact_acp.c"      -o "$OUT/imgact_acp.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/ovmx_boot_netbsd.o" | grep -Ei 'file format|architecture'

# The NetBSD backend must really define every ovmx_boot.h op (fail-honest, INV-6:
# a backend that fakes-out an op would drop a symbol here).
echo "--- confirm the NetBSD backend defines every ovmx_boot.h op ---"
for sym in ovmx_boot_kernel_filesystems_mounted ovmx_boot_mount_kernel_filesystems \
           ovmx_boot_start_console_log_bridge ovmx_boot_load_module \
           ovmx_boot_open_executive ovmx_boot_system_disk_dev \
           ovmx_boot_system_disk_present \
           ovmx_boot_power_off; do
    if ! "$TARGET-nm" "$OUT/ovmx_boot_netbsd.o" | grep -qE " T $sym\$"; then
        echo "FAIL: ovmx_boot_netbsd.o does not define $sym"
        exit 1
    fi
done
echo "OK: every ovmx_boot.h op is defined by the NetBSD backend"

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
# STATIC (-static, vms-0ab boot-speed #2): PID 1 is a self-contained ELF32-vax
# with NO PT_INTERP, so NetBSD's ld.elf_so never re-relocates
# libc/libpthread/libm/libatomic on each fork+execve activation. The in-process
# IMGACT bails SS$_UNSUPPORTED on the foreign ld.elf_so interp, so every boot
# activation was a fork+execve paying full dynamic relocation from scratch --
# native VMS's prelinked activation never pays that. Still Decision A (vms-42d):
# the substrate activates a plain NetBSD ELF32-vax, only now statically linked.
# STARTUP.EXE references NO SYSUAF engine seam (readelf: no ovmx_sysuaf_* refs),
# so it needs no rms-bind anchor; its ONE weak seam (ovmx_sysgen_acp_read,
# proof 4) is strong-linked via ovmx_boot_sysgen_acp.o on the link line and so
# survives -static unchanged. --start-group resolves the libvms<->vmsfs cycle.
"$CC" --sysroot="$SYSROOT" -static \
    "$OUT/ovmx_init.o" "$OUT/ovmx_boot_netbsd.o" "$OUT/sysboot.o" \
    "$OUT/ovmx_boot_acp_read.o" "$OUT/ovmx_boot_sysgen_acp.o" "$OUT/imgact_acp.o" \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
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

# --- proof 4: WEAK-SEAM ANCHOR (vms-329) -------------------------------------
# sysgen_params.h declares ovmx_sysgen_acp_read as a #pragma weak that defaults
# to NULL, so a link that FORGETS ovmx_boot_sysgen_acp.o produces NO link error
# -- it silently keeps the NULL default and every VAX boot falls back to the
# default node name with %OVMX-W-NOPARAMS. That is precisely the static-link
# weak-seam class this project has been bitten by before, so assert on the
# LINKED image that the strong definition really won, plus the rest of the ACP
# closure the cutover depends on. Absence here is a hard failure, never a
# warning: a STARTUP.EXE without these is a PID 1 that cannot read its own
# system disk with the VFS mount retired.
echo
echo "=== proof 4: the ACP-read bridge really linked into STARTUP.EXE ==="
for sym in ovmx_sysgen_acp_read ovmx_boot_acp_stage ovmx_boot_acp_present \
           ovmx_boot_acp_mount_system_disk ovmx_boot_prepare_stage_dir \
           imgact_acp_open; do
    if ! "$TARGET-nm" "$OUT/STARTUP.EXE" | grep -qE " [TtWw] $sym\$"; then
        echo "FAIL: STARTUP.EXE does not define $sym -- the ACP-read bridge is"
        echo "      NOT in this link. With OVMX_HAVE_ACP on, PID 1 ACP-mounts"
        echo "      SYS\$DISK and the vmsfs.ko VFS mount is gone, so a missing"
        echo "      bridge means the boot cannot read ANY file on the volume."
        exit 1
    fi
done
# The strong definition must be a real text symbol, not the weak NULL stub.
if ! "$TARGET-nm" "$OUT/STARTUP.EXE" | grep -qE " T ovmx_sysgen_acp_read\$"; then
    echo "FAIL: ovmx_sysgen_acp_read resolved WEAK in STARTUP.EXE -- the"
    echo "      #pragma-weak NULL default won and OVMXVMSSYS.PAR will never be"
    echo "      read over the ACP (silent %OVMX-W-NOPARAMS on every boot)."
    exit 1
fi
echo "OK: ovmx_sysgen_acp_read is STRONG, and the ACP-read bridge is linked in"

# --- proof 5: TEETH -- the ACP arms are compiled IN, not #if'd out ------------
# A recipe that stopped passing -DOVMX_HAVE_ACP would still compile and link
# perfectly (every arm just disappears) and would silently ship a PID 1 that
# VFS-mounts the boot unit again. vms_kif_acp_mount is referenced ONLY from
# ovmx_boot_netbsd.c's OVMX_HAVE_ACP arm, so an undefined reference to it in
# that object is the positive evidence the macro reached the compiler.
echo "=== proof 5 (TEETH): the OVMX_HAVE_ACP mount arm is compiled in ==="
if ! "$TARGET-nm" -u "$OUT/ovmx_boot_netbsd.o" | grep -qE ' vms_kif_acp_mount$'; then
    echo "FAIL: ovmx_boot_netbsd.o does not reference vms_kif_acp_mount --"
    echo "      OVMX_HAVE_ACP did not reach the compiler and this STARTUP.EXE"
    echo "      would VFS-mount SYS\$DISK instead of \$MOUNTing it over the ACP."
    exit 1
fi
echo "OK: ovmx_boot_netbsd.o references vms_kif_acp_mount (ACP mount arm live)"

echo
echo "=== ALL PROOFS PASSED: ovmx_init (STARTUP.EXE) builds and links for $TARGET ==="
