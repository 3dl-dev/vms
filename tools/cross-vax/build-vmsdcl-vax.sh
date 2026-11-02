#!/bin/sh
# build-vmsdcl-vax.sh - cross-compile + link-prove vmsdcl (the DCL shell) for
# netbsd-vax.
#
# Runs INSIDE the ovmx-cross-vax container (tools/cross-vax/Dockerfile), which
# provides the vax--netbsdelf gcc/ld + a NetBSD/vax sysroot. Nothing here touches
# the host. vmsdcl is the DCL command-language interpreter -- the TOP of the
# userspace library graph and the LAST userspace library before the init+images
# integration (CLAUDE.md "Library Build Order"):
#   ... libvms -> vmslnm -> vmsfs -> vmsrms -> vmsdcl (+ vmsqueue)
#
# DCL is overwhelmingly string/parse work over ordinary POSIX (open/lseek/off_t/
# fcntl/dirent/fnmatch/termios/statvfs); the constants resolve from the NetBSD
# sysroot's headers and off_t is the sysroot's LFS type -- no Linux syscall
# number and no Linux-numeric constant is assumed. DCL integer symbols are
# longwords (expr_val_t.ival is `long`, printed `%ld`): on ILP32 VAX `long` is
# 32-bit, which is exactly the VMS longword width -- self-consistent, no width
# fix needed (docs/audit-ilp32-vax-libvmssys.md §12). readline is OPTIONAL and
# absent from the NetBSD/vax sysroot, so HAVE_READLINE stays undefined and DCL's
# own non-readline line editor (dcl_io.c / dcl_editor.c) is used -- the same
# degrade path the static musl QEMU build takes.
#
# Proofs, mirroring build-vmsrms-vax.sh but topped with the DCL.EXE link:
#
#   0..0f. the elf32-vax dependency stack:
#          libvmssys.a, vmsprocess.a, libvms.a, vmslnm.a, vmsfs.a, vmsrms.a,
#          vmsqueue.a
#   1.  CMAKE WIRING: configure src/vmsdcl standalone with the VAX toolchain
#       file (VMSDCL_STANDALONE branch) -> libvmsdcl.a of elf32-vax objects.
#   2.  DCL.EXE LINK PROOF: link the REAL DCL shell -- every DCL object forced in
#       with --whole-archive, so nothing is silently dropped -- AGAINST the whole
#       elf32-vax stack + NetBSD libc + libpthread + libm + libatomic, producing
#       a real vax--netbsdelf ELF32 executable. --start-group resolves the
#       libvms<->vmsfs archive cycle. Never executed (VAX cannot run on the amd64
#       host); the proof is a clean vax--netbsdelf link of the entire shell.
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
VMSDCL="$SRC/src/vmsdcl"
VMS_INCLUDE="$SRC/src/libvms/include"
VMSPROCESS_INCLUDE="$SRC/src/vmsprocess/include"
VMSLNM_INCLUDE="$SRC/src/vmslnm/include"
VMSFS_INCLUDE="$SRC/src/vmsfs/include"
VMSRMS_INCLUDE="$SRC/src/vmsrms/include"
VMSDCL_INCLUDE="$SRC/src/vmsdcl/include"
OUT="${OUT:-/tmp/vax-vmsdcl-build}"
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

# --- proof 1: CMake standalone build of the vmsdcl object archive -----------
echo "=== proof 1: CMake netbsd-vax build of vmsdcl (libvmsdcl.a) ==="
cmake -S "$VMSDCL" -B "$OUT/cmake" -G "Unix Makefiles" \
    -DCMAKE_MAKE_PROGRAM="$MAKE_PROG" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$OUT/cmake" --target vmsdcl -- -j"$(nproc)"
LIBDCL="$(find "$OUT/cmake" -name 'libvmsdcl.a' | head -1)"
test -n "$LIBDCL"
echo "built: $LIBDCL"
echo "--- object arch check (must be VAX / ELF32 LSB) ---"
"$TARGET-objdump" -f "$(find "$OUT/cmake" -name 'dcl_main*.o' | head -1)" | grep -Ei 'file format|architecture'
echo "--- confirm readline was NOT pulled in (optional; absent from sysroot) ---"
if "$TARGET-nm" "$LIBDCL" 2>/dev/null | grep -qiE ' U (readline|add_history|rl_redisplay)$'; then
    echo "FAIL: DCL archive references GNU readline symbols -- HAVE_READLINE leaked into the netbsd-vax build"
    exit 1
fi
echo "OK: no undefined readline symbols -- DCL's own non-readline line editor is used"
echo

# --- the WEAK SEAMS libvms reaches LIBVMSRMS through (rd vms-1f7) -----------
# libvms declares its LIBVMSRMS entry points `#pragma weak` (rightslist.c,
# sysuaf.c, rms_textfile.c, ...) so a bare LIBVMS unit test links without
# LIBVMSRMS and takes the honest miss. In THIS static-archive link a weak
# undefined reference does NOT pull its archive member in, so unless DCL also
# references the symbol strongly it stays NULL -- and on the VAX disk that left
# F$IDENTIFIER, SHOW PROCESS's "User Identifier:" and SHOW DEVICE's owner UIC
# on the miss ([001,004], never [SYSTEM]) while the very same code linked by the
# top-level CMake build resolved. Every weak symbol libvms leaves undefined that
# LIBVMSRMS DEFINES is forced in with --undefined, and proof 3 below fails the
# build if any is still weak in the linked image.
WEAK_SEAMS=$("$TARGET-nm" "$LIBVMS_A" 2>/dev/null | awk '$1=="w"{print $2}' | sort -u)
# Only LIBVMSRMS's: that is the library the seams name (the shared-library
# runtimes bind exactly these when LIBVMSRMS$SHR is loaded). Other weak hooks
# (e.g. the in-process image activator's) keep their own link decision.
STACK_DEFS=$("$TARGET-nm" --defined-only "$LIBVMSRMS_A" 2>/dev/null | awk 'NF==3{print $3}' | sort -u)
printf '%s\n' "$STACK_DEFS" > "$OUT/stack-defs.txt"
FORCE_SEAMS=$(printf '%s\n' "$WEAK_SEAMS" | grep -Fx -f "$OUT/stack-defs.txt" || true)
SEAM_FLAGS=""
for s in $FORCE_SEAMS; do SEAM_FLAGS="$SEAM_FLAGS -Wl,--undefined=$s"; done
echo "weak LIBVMS seams LIBVMSRMS defines (forced in): $(echo $FORCE_SEAMS)"
test -n "$FORCE_SEAMS" || { echo "FAIL: found no weak LIBVMS seams -- the scan itself is broken"; exit 1; }

# --- proof 2: link the REAL DCL.EXE over the full elf32-vax stack -----------
echo "=== proof 2: link a real vax--netbsdelf DCL.EXE over the whole stack ==="
# --whole-archive on libvmsdcl.a forces EVERY DCL object in, so any unresolved
# symbol (against the stack or libc) is a hard link error -- a complete shell,
# not just the objects crt0 happens to reach through main().
"$CC" --sysroot="$SYSROOT" \
    -I"$VMS_INCLUDE" -I"$VMSPROCESS_INCLUDE" -I"$VMSLNM_INCLUDE" \
    -I"$VMSFS_INCLUDE" -I"$VMSRMS_INCLUDE" -I"$VMSDCL_INCLUDE" -I"$LIBVMSSYS" \
    -Wl,--whole-archive "$LIBDCL" -Wl,--no-whole-archive \
    $SEAM_FLAGS \
    -Wl,--start-group \
        "$LIBVMSQUEUE_A" "$LIBVMSRMS_A" "$LIBVMS_A" "$LIBVMSFS_A" \
        "$LIBVMSLNM_A" "$LIBVMSPROCESS_A" "$LIBVMSSYS_A" \
    -Wl,--end-group \
    -lpthread -lm -latomic -o "$OUT/DCL.EXE"
echo "--- linked executable ---"
file "$OUT/DCL.EXE"
"$TARGET-readelf" -h "$OUT/DCL.EXE" | grep -Ei 'Class|Data|Machine|Type'


# --- proof 3: no LIBVMS seam the stack defines is left weak-NULL -----------
echo "=== proof 3: every forced seam is DEFINED in DCL.EXE (none left weak) ==="
STILL_WEAK=""
for s in $FORCE_SEAMS; do
    "$TARGET-nm" "$OUT/DCL.EXE" | awk -v s="$s" '$NF==s && ($(NF-1)=="T"||$(NF-1)=="D"||$(NF-1)=="B"||$(NF-1)=="R"){f=1} END{exit !f}' \
        || STILL_WEAK="$STILL_WEAK $s"
done
if [ -n "$STILL_WEAK" ]; then
    echo "FAIL: DCL.EXE leaves these LIBVMS seams weak-NULL:$STILL_WEAK"
    exit 1
fi
echo "OK: all $(echo $FORCE_SEAMS | wc -w) seams are defined (e.g. ovmx_rightslist_idtoasc)"
"$TARGET-nm" "$OUT/DCL.EXE" | grep -q ' T ovmx_rightslist_idtoasc$' \
    || { echo "FAIL: ovmx_rightslist_idtoasc is not in DCL.EXE -- SHOW PROCESS cannot resolve a UIC"; exit 1; }

echo
echo "=== ALL PROOFS PASSED: vmsdcl builds and links a DCL.EXE for $TARGET ==="
