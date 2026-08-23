#!/bin/sh
# build-loginout-vax.sh - cross-compile + link-prove tools/vms_login.c
# (LOGINOUT.EXE) for netbsd-vax (rd vms-5d1, epic vms-8e8, P4-C boot;
# docs/design-p4-netbsd-vax-boot.md §4.5).
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here
# touches the host. LOGINOUT.EXE (tools/vms_login.c) is the ONE OVMX login
# image (tools/CMakeLists.txt: OUTPUT_NAME "LOGINOUT"): JOB_CONTROL.EXE forks
# it on the console for every interactive session, and DCL.EXE's own login
# banner text is deliberately its exclusive emitter (src/vmsdcl/dcl_main.c) --
# it is the LAST link in the boot chain STARTUP.EXE -> PROVISION.EXE ->
# DCL.EXE (STARTUP.COM) -> JOB_CONTROL.EXE -> LOGINOUT.EXE -> DCL.EXE
# (interactive), the P4 capstone's "ovmx_init -> LOGINOUT -> DCL prompt" goal
# (vms-d59), and a REQUIRED image, not an optional one.
#
# Proofs, mirroring build-vmsdcl-vax.sh but topped with the LOGINOUT.EXE link:
#
#   0..0f. the elf32-vax dependency stack:
#          libvmssys.a, vmsprocess.a, libvms.a, vmslnm.a, vmsfs.a, vmsrms.a,
#          vmsqueue.a
#   1.  compile vms_login.c + loginout_display.c + mail_notify.c (the same
#       three translation units tools/CMakeLists.txt's vms_login target
#       builds) for elf32-vax.
#   2.  LOGINOUT.EXE LINK PROOF: link a real vax--netbsdelf ELF32 executable
#       against the whole elf32-vax stack + NetBSD libc + libpthread + libm +
#       libatomic. --start-group resolves the libvms<->vmsfs archive cycle.
#       Never executed (VAX cannot run on the amd64 host); the proof is a
#       clean vax--netbsdelf link of LOGINOUT.EXE.
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
TOOLS="$SRC/tools"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
OUT="${OUT:-/tmp/vax-loginout-build}"
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

# --- common compile flags for the vms_login TUs -----------------------------
CFLAGS_COMMON="--sysroot=$SYSROOT -O2 -Wall -Wextra \
    -D_NETBSD_SOURCE -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
    -I$TOOLS \
    -I$VMS_INCLUDE -I$VMSPROCESS_INCLUDE -I$VMSLNM_INCLUDE \
    -I$VMSFS_INCLUDE -I$VMSRMS_INCLUDE -I$LIBVMSSYS"

# --- proof 1: cross-compile vms_login.c + loginout_display.c + mail_notify.c
echo "=== proof 1: cross-compile vms_login.c + loginout_display.c + mail_notify.c ==="
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$TOOLS/vms_login.c"          -o "$OUT/vms_login.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$TOOLS/loginout_display.c"   -o "$OUT/loginout_display.o"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$TOOLS/mail_notify.c"         -o "$OUT/mail_notify.o"
# WEAK-SEAM ANCHOR (vms-0ab). LOGINOUT authenticates by calling sysuaf_lookup()
# (tools/vms_login.c), which in LIBVMS `#pragma weak`-references the SYSUAF RMS
# engine ovmx_sysuaf_read_user/_uic (src/vmsrms, sysuaf_rms.o) and returns
# "miss" when the cell is NULL. A WEAK undefined reference does NOT pull the
# defining archive member, so both the DYNAMIC baseline and a naive -static link
# leave those cells UND=0 -- every login then fails "User authorization failure"
# (the exact static-link weak-seam class that broke Alpha login) without ever
# reaching the ACP. src/vmslink/loginout_rms_bind.c is the PRODUCTION anchor for
# this seam (already reasoned+used on the LINK.EXE shareable path): its
# volatile-guarded rms_bind_never() makes STRONG references to the sys$open RMS
# family AND ovmx_sysuaf_read_user/_uic, so compiled+linked here it EXTRACTS
# sysuaf_rms.o from vmsrms.a and binds the weak cells. It executes nothing at
# run time (the guard is always false). Reused, not reinvented.
LOGINOUT_RMS_BIND="$SRC/src/vmslink/loginout_rms_bind.c"
# shellcheck disable=SC2086
"$CC" $CFLAGS_COMMON -c "$LOGINOUT_RMS_BIND"           -o "$OUT/loginout_rms_bind.o"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$OUT/vms_login.o" | grep -Ei 'file format|architecture'
echo

# --- proof 2: link a real vax--netbsdelf LOGINOUT.EXE over the whole stack --
echo "=== proof 2: link a real vax--netbsdelf LOGINOUT.EXE ==="
# STATIC (-static, vms-0ab boot-speed #2): self-contained ELF32-vax, no
# PT_INTERP -> ld.elf_so never re-relocates libc/libpthread/libm/libatomic on
# each fork+execve activation. Still Decision A (vms-42d), only statically
# linked. loginout_rms_bind.o is a PRIMARY object (before the archive group) so
# its strong refs pull the SYSUAF engine that LOGINOUT's weak seam needs.
"$CC" --sysroot="$SYSROOT" -static \
    "$OUT/vms_login.o" "$OUT/loginout_display.o" "$OUT/mail_notify.o" \
    "$OUT/loginout_rms_bind.o" \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/LOGINOUT.EXE"
echo "--- linked executable ---"
file "$OUT/LOGINOUT.EXE"
"$TARGET-readelf" -h "$OUT/LOGINOUT.EXE" | grep -Ei 'Class|Data|Machine|Type'

HDR="$("$TARGET-readelf" -h "$OUT/LOGINOUT.EXE")"
echo "$HDR" | grep -qiE 'Class:[[:space:]]+ELF32' \
    || { echo "FAIL: LOGINOUT.EXE is not ELFCLASS32 (VAX is 32-bit)"; exit 1; }
echo "$HDR" | grep -qiF 'Digital VAX' \
    || { echo "FAIL: LOGINOUT.EXE Machine is not Digital VAX"; exit 1; }

# --- proof 2b (WEAK-SEAM, vms-0ab): the SYSUAF engine is DEFINED, not UND ------
# The anti-LARP teeth: if the anchor failed to pull sysuaf_rms.o, these stay
# weak UND=0 and sysuaf_lookup() returns miss -> every login fails "User
# authorization failure". Assert they are DEFINED in the LINKED -static image.
echo "=== proof 2b (WEAK-SEAM): SYSUAF auth engine defined in LOGINOUT.EXE ==="
for sym in ovmx_sysuaf_read_user ovmx_sysuaf_read_uic sysuaf_authenticate purdy_s_hash; do
    if ! "$TARGET-readelf" -sW "$OUT/LOGINOUT.EXE" \
            | grep -E " $sym\$" | grep -qvE ' UND '; then
        echo "FAIL: $sym is UND/absent in LOGINOUT.EXE -- the SYSUAF weak seam"
        echo "      dropped; sysuaf_lookup() would return miss and every login"
        echo "      fail 'User authorization failure' (vms-0ab static weak-seam)."
        exit 1
    fi
done
echo "OK: SYSUAF engine (read_user/read_uic/authenticate/purdy_s_hash) DEFINED"

# --- proof 2c (STATIC, vms-0ab): no PT_INTERP / no dynamic NEEDED -------------
echo "=== proof 2c (STATIC): LOGINOUT.EXE is a self-contained static ELF32-vax ==="
if "$TARGET-readelf" -l "$OUT/LOGINOUT.EXE" | grep -qiE 'INTERP'; then
    echo "FAIL: LOGINOUT.EXE has a PT_INTERP -- not statically linked (vms-0ab)."; exit 1
fi
if "$TARGET-readelf" -d "$OUT/LOGINOUT.EXE" 2>/dev/null | grep -qiE 'NEEDED'; then
    echo "FAIL: LOGINOUT.EXE has a DT_NEEDED -- not statically linked (vms-0ab)."; exit 1
fi
echo "OK: no PT_INTERP, no DT_NEEDED -- fully static"

echo
echo "=== ALL PROOFS PASSED: vms_login (LOGINOUT.EXE) builds and links for $TARGET ==="
