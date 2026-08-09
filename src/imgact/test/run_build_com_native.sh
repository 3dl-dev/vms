#!/bin/sh
# run_build_com_native.sh -- self-host S3.2 proof (bead vms-615): a DCL command
# procedure (BUILD.COM) drives a complete compile -> link -> run of a C program
# from inside an OVMX DCL session, using the OVMX-NATIVE toolchain (TCC.EXE +
# LINK.EXE, both OVMX-native images activated through IMGACT.EXE), with ZERO
# bash / no host tool anywhere in the build path. BUILD.COM replaces the
# run_*.sh shell build drivers -- this is "Build-native" self-hosting (vms-678).
#
# WHAT THIS PROVES (the vms-615 endpoint):
#   1. The producer graph + IMGACT.EXE + TCC.EXE + a NATIVE LINK.EXE + DCL.EXE
#      all build VMS-native (LINK.EXE --executable / --shareable, PT_INTERP=
#      IMGACT.EXE). A host bootstrap LINK.EXE BUILDS these images -- allowed
#      (CLAUDE.md Rule 9: a build step is not an activation proof). What is
#      proven native is the RUNTIME path below.
#   2. DCL.EXE is IMGACT-activated and runs `@SYS$SYSTEM:BUILD HELLO`. Inside
#      that DCL session, BUILD.COM invokes TCC.EXE (foreign command) to compile
#      HELLO.C -> HELLO.OBJ and the native LINK.EXE (foreign command LNK) to
#      link HELLO.OBJ -> HELLO.EXE, then activates HELLO.EXE -- every toolchain
#      step an OVMX-native image, driven from DCL, NO bash in the path.
#   3. The produced HELLO.EXE runs and prints 'hello', and the procedure reports
#      success ($STATUS success) -- OVMX built and ran an OVMX-produced image
#      from within OVMX's own shell.
#
# BUILD_EXPECT (default 1, BLOCKING -- mirrors DCL_EXPECT_LINK / TCC_EXPECT_LINK
# / LINK_EXPECT): a failure of any native build or of the @BUILD run is a real
# regression. Set BUILD_EXPECT=0 to soften the native-toolchain build/link steps
# to a SKIP (exit 2) while iterating; the @BUILD assertions stay hard FAILs.
#
# link.c / imgact.c / tcc are the complete toolchain and are OUT of scope here
# (do NOT edit them); this bead's product is BUILD.COM + the DCL driver plumbing.
#
# arm64 musl Alpine container only (CLAUDE.md test loop): tinycc's configure
# auto-selects TCC_TARGET_ARM64 from uname -m, so -- like run_tcc_native.sh --
# this is aarch64-only (native or arm64 binfmt/QEMU emulation). Needs root to
# create /vms.
set -e
CC=${CC:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)          # src/imgact/test
IMGACT_DIR=$(cd "$HERE/.." && pwd)           # src/imgact
LINK_DIR=$(cd "$IMGACT_DIR/../vmslink" && pwd)
SRC=$(cd "$IMGACT_DIR/.." && pwd)            # src
REPO=$(cd "$SRC/.." && pwd)                  # repo root
TCC_SRC="$REPO/third-party/tcc/src"
LIBVMSSYS_DIR="$SRC/libvmssys"
VMSPROC_DIR="$SRC/vmsprocess"
VMSLNM_DIR="$SRC/vmslnm"
VMSFS_DIR="$SRC/vmsfs"
LIBVMS_DIR="$SRC/libvms"
VMSRMS_DIR="$SRC/vmsrms"
DCL_DIR="$SRC/vmsdcl"
LIBVMS_INC="$LIBVMS_DIR/include"
LNM_INC="$VMSLNM_DIR/include"
VMSFS_INC="$VMSFS_DIR/include"
RMS_INC="$VMSRMS_DIR/include"
WORK=${WORK:-/tmp/build-com-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ARCH=aarch64 ;;
    *) echo "SKIP-FAIL: run_build_com_native.sh needs a native aarch64 host (got $(uname -m)) -- tinycc's configure auto-selects TCC_TARGET_ARM64 from uname -m, and TCC.EXE is aarch64-only today. Run in the arm64 musl Alpine container per CLAUDE.md / bead vms-615."; exit 1 ;;
esac
export ARCH

SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
mkdir -p "$SYSEXE" "$SYSLIB"

LIBC=${LIBC:-/usr/lib/libc.a}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
[ -f "$LIBC" ]   || { echo "SKIP-FAIL: no musl libc.a at $LIBC (need an $ARCH musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "FAIL: no libgcc.a at $LIBGCC"; exit 1; }
[ -d "$TCC_SRC" ] || { echo "FAIL: third-party/tcc/src not found -- vendoring bead vms-4ba.1 missing?"; exit 1; }

# ---- Producer graph (IMGACT.EXE + bootstrap LINK.EXE + DECC$SHR + the five
# OVMX shareables). Same builder run_dcl_native.sh / run_link_native.sh use.
. "$HERE/lib_build_graph.sh"
build_producer_graph

echo
echo "== build TCC.EXE VMS-native (mk_tcc.sh) =="
set +e
CC="$CC" WORK="$WORK/mk-tcc" sh "$LINK_DIR/mk_tcc.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/TCC.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$TCC_SRC" 2>"$WORK/tcc-link.err"
TRC=$?
set -e
echo "-- mk_tcc.sh exit=$TRC; message: --"; tail -4 "$WORK/tcc-link.err" | sed 's/^/   /'
if [ "$TRC" -ne 0 ]; then
    [ "${BUILD_EXPECT:-1}" = "1" ] && { echo "FAIL: TCC.EXE build failed (regression)"; exit 1; }
    echo "SKIP (BUILD_EXPECT=0): TCC.EXE build failed."; exit 2
fi
readelf -lW "$SYSEXE/TCC.EXE" | grep -q 'INTERP' || { echo "FAIL: TCC.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/TCC.EXE"

echo
echo "== build LINK.EXE VMS-native (mk_link.sh: link.c + RMS shim) =="
set +e
CC="$CC" ARCH="$ARCH" WORK="$WORK/mk-link" sh "$LINK_DIR/mk_link.sh" \
    "$WORK/LINK.EXE" "$SYSEXE/LINK.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SRC" 2>"$WORK/link-build.err"
LRC=$?
set -e
echo "-- mk_link.sh exit=$LRC; message: --"; tail -4 "$WORK/link-build.err" | sed 's/^/   /'
if [ "$LRC" -ne 0 ]; then
    [ "${BUILD_EXPECT:-1}" = "1" ] && { echo "FAIL: native LINK.EXE build failed (regression)"; exit 1; }
    echo "SKIP (BUILD_EXPECT=0): native LINK.EXE build failed."; exit 2
fi
readelf -lW "$SYSEXE/LINK.EXE" | grep -q 'INTERP' || { echo "FAIL: native LINK.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/LINK.EXE"

echo
echo "== build DCL.EXE VMS-native (mk_dcl.sh) =="
set +e
sh "$LINK_DIR/mk_dcl.sh" "$WORK/LINK.EXE" "$SYSEXE/DCL.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    "$DCL_DIR" "$SRC" 2>"$WORK/dcl-link.err"
DRC=$?
set -e
echo "-- mk_dcl.sh exit=$DRC; message: --"; tail -4 "$WORK/dcl-link.err" | sed 's/^/   /'
if [ "$DRC" -ne 0 ]; then
    [ "${BUILD_EXPECT:-1}" = "1" ] && { echo "FAIL: DCL.EXE build failed (regression)"; exit 1; }
    echo "SKIP (BUILD_EXPECT=0): DCL.EXE build failed."; exit 2
fi
readelf -lW "$SYSEXE/DCL.EXE" | grep -q 'INTERP' || { echo "FAIL: DCL.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/DCL.EXE"

echo
echo "== stage BUILD.COM into SYS\$SYSTEM + a HELLO.C in the project directory =="
cp "$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/BUILD.COM" "$SYSEXE/BUILD.COM"
# The project directory is the system-disk root ([000000] -> /vms): an
# unqualified filespec ("HELLO.C") resolves to vms_default_root (=SYSDISK_MOUNT,
# /vms) inside every OVMX image, so a bare name lands in the SAME place for
# DCL, TCC.EXE and LINK.EXE. (BUILD.COM's own default directory is set to
# SYS$SYSDEVICE:[000000] below so DCL's RUN resolves there too.)
PROJ=/vms
rm -f "$PROJ"/HELLO.OBJ* "$PROJ"/HELLO.EXE* "$PROJ"/hello.obj* "$PROJ"/hello.exe*
cat > "$PROJ/HELLO.C" <<'EOF'
#include <stdio.h>
int main(void)
{
    printf("hello\n");
    return 0;
}
EOF
printf '$ @SYS$SYSTEM:BUILD HELLO\n$ EXIT\n' > "$PROJ/DObuild.com"
echo "-- SYSEXE: --"; ls -1 "$SYSEXE" | sed 's/^/   /'
echo "-- project dir ($PROJ): --"; ls -1 "$PROJ" | grep -iE 'hello|dobuild' | sed 's/^/   /'

echo
echo "== run the DCL session: DCL.EXE activates, @SYS\$SYSTEM:BUILD HELLO =="
set +e
( cd "$PROJ" && VMS_ROOT=/vms VMS_DEFAULT_DIR='SYS$SYSDEVICE:[000000]' \
    "$SYSEXE/DCL.EXE" "$PROJ/DObuild.com" ) > "$WORK/build.out" 2>&1
BRC=$?
set -e
echo "-- @BUILD session output: --"; sed 's/^/   /' "$WORK/build.out"
echo "-- DCL.EXE exit=$BRC --"
echo "-- project dir after build (HELLO.*): --"; ls -la "$PROJ" | grep -iE 'hello' | sed 's/^/   /'

echo
echo "== assertions =="
# The produced image -- RMS mints a ;version suffix on disk and vmsfs lowercases
# the unqualified name, so match case-insensitively with the version suffix.
HELLO_EXE=$(ls "$PROJ"/hello.exe\;* "$PROJ"/HELLO.EXE\;* 2>/dev/null | head -1)
[ -n "$HELLO_EXE" ] || { echo "FAIL: BUILD.COM did not produce HELLO.EXE (no hello.exe;N in $PROJ)"; exit 1; }
echo "-- produced image: $HELLO_EXE --"
readelf -lW "$HELLO_EXE" | grep -q 'INTERP' || { echo "FAIL: HELLO.EXE has no PT_INTERP (IMGACT)"; exit 1; }

grep -q 'hello' "$WORK/build.out" || { echo "FAIL: the produced HELLO.EXE did not print 'hello' from the DCL session"; exit 1; }
grep -q '%BUILD-S-OK' "$WORK/build.out" || { echo "FAIL: BUILD.COM did not report success (%BUILD-S-OK)"; exit 1; }
[ "$BRC" -eq 0 ] || { echo "FAIL: the DCL @BUILD session did not exit clean (got $BRC)"; exit 1; }

echo
echo "================================================================================"
echo "MILESTONE (vms-615, self-host S3.2): a DCL command procedure (BUILD.COM) drives"
echo "compile -> link -> run end-to-end using the OVMX-native TCC.EXE + LINK.EXE as DCL"
echo "foreign commands, all activated through IMGACT.EXE, with ZERO bash / no host tool"
echo "in the build path. @SYS\$SYSTEM:BUILD HELLO compiled HELLO.C, linked HELLO.EXE, and"
echo "the produced image ran and printed 'hello'. The shell build driver is replaced by"
echo "a DCL driver -- Build-native self-hosting (vms-678)."
echo "================================================================================"
