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
#
# vms-329: -DOVMX_HAVE_ACP. PROVISION provisions the system tree and the account
# homes over the executive Files-11 ACP (IO$_MODIFY / IO$_CREATE by FID), not
# with POSIX lchown()/mkdir() on a /vms passthrough -- the coupled VAX cutover
# retired that passthrough with the vmsfs.ko VFS mount, so the POSIX arm here
# would walk a directory that no longer exists and provision NOTHING while
# reporting success (INV-6's fake-success class). _POSIX_C_SOURCE is dropped
# for the same reason build-ovmx-init-vax.sh drops it: it turns OFF the NetBSD
# namespace the shared ovmx_boot_netbsd.c compiled here needs, and
# src/ovmx_provision/CMakeLists.txt already selects bare _NETBSD_SOURCE.
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -DOVMX_HAVE_ACP \
    -I$OVMX_PROVISION -I$OVMX_INIT \
    -I$VMS_INCLUDE -I$VMSPROCESS_INCLUDE -I$VMSLNM_INCLUDE \
    -I$VMSFS_INCLUDE -I$VMSRMS_INCLUDE -I$LIBVMSSYS -I$SRC/src/kernel"

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
# STATIC (-static, vms-0ab boot-speed #2): self-contained ELF32-vax, no
# PT_INTERP -> no ld.elf_so re-relocation per fork+execve activation. Still
# Decision A (vms-42d), only statically linked. PROVISION.EXE reads SYSUAF to
# establish the SYSTEM identity: ovmx_provision.c makes a STRONG call into the
# SYSUAF RMS engine (ovmx_sysuaf_read_user), so sysuaf_rms.o is pulled from
# vmsrms.a by a real reference -- NOT the weak seam -- and therefore survives
# -static unchanged (proof 3b below asserts the engine really linked). No
# rms-bind anchor is needed here; the strong call is the anchor.
"$CC" --sysroot="$SYSROOT" -static \
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

# --- proof 3 (TEETH, vms-329): the ACP provisioning arm is compiled IN --------
# Dropping -DOVMX_HAVE_ACP would still compile and link perfectly -- the POSIX
# lchown()/mkdir() arm takes over -- and would silently ship a PROVISION.EXE
# that writes NOTHING to the ODS-2 volume (every lchown ENOENT is swallowed).
# own_object_acp() exists ONLY inside the ACP arm, and the IO$_MODIFY it drives
# reaches the executive through vms_kif_acp_fileop, so both are positive
# evidence the macro really reached the compiler.
echo "=== proof 3 (TEETH): the OVMX_HAVE_ACP provisioning arm is compiled in ==="
if ! "$TARGET-nm" "$OUT/ovmx_provision.o" | grep -qE ' [Tt] own_object_acp$'; then
    echo "FAIL: ovmx_provision.o does not define own_object_acp -- the ACP"
    echo "      provisioning arm is #if'd OUT, so this PROVISION.EXE would"
    echo "      lchown() a /vms tree that no longer exists on the VAX and"
    echo "      report success having written nothing (INV-6)."
    exit 1
fi
if ! "$TARGET-nm" -u "$OUT/ovmx_provision.o" | grep -qE ' vms_kif_acp_fileop$'; then
    echo "FAIL: ovmx_provision.o does not reference vms_kif_acp_fileop -- no"
    echo "      IO\$_MODIFY reaches the executive from this image."
    exit 1
fi
echo "OK: own_object_acp defined + vms_kif_acp_fileop referenced (ACP arm live)"

# --- proof 3b (WEAK-SEAM, vms-0ab): the SYSUAF engine survived -static ---------
# The -static cutover's hazard is the static-link weak-seam class: sysuaf.c in
# libvms `#pragma weak`-references ovmx_sysuaf_read_user and returns "miss" when
# the cell is NULL, so a -static link that failed to pull sysuaf_rms.o would
# silently ship a PROVISION.EXE that cannot read SYSUAF and halts the boot with
# "no SYSTEM record" -- the exact class that broke Alpha login. ovmx_provision.c
# makes a STRONG call, so the member IS pulled; assert the definitions really
# landed in the LINKED -static image (not a weak UND) so a future refactor that
# drops the strong call is caught here, not at boot.
echo "=== proof 3b (WEAK-SEAM): the SYSUAF auth engine is defined in PROVISION.EXE ==="
for sym in ovmx_sysuaf_read_user ovmx_sysuaf_read_uic sysuaf_authenticate purdy_s_hash; do
    if ! "$TARGET-readelf" -sW "$OUT/PROVISION.EXE" \
            | grep -E " $sym\$" | grep -qvE ' UND '; then
        echo "FAIL: $sym is not DEFINED in PROVISION.EXE -- the SYSUAF engine"
        echo "      weak seam dropped under -static; PROVISION cannot read SYSUAF"
        echo "      and the boot would halt with no SYSTEM record (vms-0ab)."
        exit 1
    fi
done
echo "OK: SYSUAF engine (read_user/read_uic/authenticate/purdy_s_hash) defined in the -static image"

# --- proof 3c (STATIC, vms-0ab): no PT_INTERP / no dynamic NEEDED -------------
echo "=== proof 3c (STATIC): PROVISION.EXE is a self-contained static ELF32-vax ==="
if "$TARGET-readelf" -l "$OUT/PROVISION.EXE" | grep -qiE 'INTERP'; then
    echo "FAIL: PROVISION.EXE has a PT_INTERP -- not statically linked (vms-0ab)."; exit 1
fi
if "$TARGET-readelf" -d "$OUT/PROVISION.EXE" 2>/dev/null | grep -qiE 'NEEDED'; then
    echo "FAIL: PROVISION.EXE has a DT_NEEDED -- not statically linked (vms-0ab)."; exit 1
fi
echo "OK: no PT_INTERP, no DT_NEEDED -- fully static"

echo
echo "=== ALL PROOFS PASSED: ovmx_provision (PROVISION.EXE) builds and links for $TARGET ==="
