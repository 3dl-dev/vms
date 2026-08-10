#!/bin/sh
# run_link_selfhost_native.sh -- self-host S4 FIXPOINT proof (bead vms-62b, the
# vms-678 Build-native 1.0 gate).  OVMX builds a nontrivial OVMX component --
# the OVMX linker LINK.EXE itself -- from WITHIN OVMX, byte-stable, with ZERO
# bash / no host tool anywhere in the FIXPOINT build path:
#
#   * A host bootstrap linker BUILDS gen1 LINK.EXE (CLAUDE.md Rule 9: a build
#     step is not an activation proof).  gen1 is an OVMX-native image.
#   * DCL session #1 runs @SYS$SYSTEM:BUILD LINKG2 LINK OVMX_LINK_RMS_IO.
#     Inside that session BUILD.COM (multi-TU) drives the OVMX-native TCC.EXE to
#     compile link.c + ovmx_link_rms_io.c to objects and the OVMX-native gen1
#     LINK.EXE to link them into gen2 LINK.EXE -- every toolchain step an OVMX
#     image activated through IMGACT.EXE, NO bash in the path.  gen2 is the
#     first OVMX-BUILT linker.
#   * gen2 is installed as SYS$SYSTEM:LINK.EXE.  DCL session #2 runs the SAME
#     @BUILD (LINKG3): TCC.EXE recompiles the sources and the OVMX-built gen2
#     LINK.EXE links them into gen3 -- the OVMX-built linker linking itself.
#   * THE FIXPOINT: gen2 and gen3 are byte-identical (sha256/cmp).  A linker
#     that reproduces itself under its own successor is a true self-hosting
#     fixpoint -- the tcc gen2==gen3 pattern (vms-4ba), now for LINK.EXE.
#
# This is the S4 milestone: "OVMX builds OVMX from within."  link.c / imgact.c /
# tcc are the toolchain and are OUT of scope here (do NOT edit them); this bead's
# product is the multi-TU BUILD.COM + the DCL symbol-eval it needs (vms-5c1).
#
# LINK_SELFHOST_EXPECT (default 1, BLOCKING -- mirrors BUILD_EXPECT/LINK_EXPECT):
# a failure of any native build, of either @BUILD run, or of the byte-stability
# check is a real regression.  Set LINK_SELFHOST_EXPECT=0 to soften the native
# toolchain build/link steps to a SKIP (exit 2) while iterating; the @BUILD and
# fixpoint assertions stay hard FAILs.
#
# arm64 musl Alpine container only (CLAUDE.md test loop): tinycc's configure
# auto-selects TCC_TARGET_ARM64 from uname -m, so -- like run_build_com_native.sh
# / run_tcc_native.sh -- this is aarch64-only (native or arm64 binfmt/QEMU
# emulation).  Needs root to create /vms.
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
WORK=${WORK:-/tmp/link-selfhost-native}
rm -rf "$WORK"; mkdir -p "$WORK"

case "$(uname -m)" in
    aarch64) ARCH=aarch64 ;;
    *) echo "SKIP-FAIL: run_link_selfhost_native.sh needs a native aarch64 host (got $(uname -m)) -- tinycc's configure auto-selects TCC_TARGET_ARM64 from uname -m, and TCC.EXE is aarch64-only today. Run in the arm64 musl Alpine container per CLAUDE.md / bead vms-62b."; exit 1 ;;
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
# OVMX shareables). Same builder run_build_com_native.sh uses.
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
    [ "${LINK_SELFHOST_EXPECT:-1}" = "1" ] && { echo "FAIL: TCC.EXE build failed (regression)"; exit 1; }
    echo "SKIP (LINK_SELFHOST_EXPECT=0): TCC.EXE build failed."; exit 2
fi
readelf -lW "$SYSEXE/TCC.EXE" | grep -q 'INTERP' || { echo "FAIL: TCC.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/TCC.EXE"

echo
echo "== build gen1 LINK.EXE VMS-native (mk_link.sh: host bootstrap builds it) =="
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
    [ "${LINK_SELFHOST_EXPECT:-1}" = "1" ] && { echo "FAIL: gen1 native LINK.EXE build failed (regression)"; exit 1; }
    echo "SKIP (LINK_SELFHOST_EXPECT=0): gen1 native LINK.EXE build failed."; exit 2
fi
readelf -lW "$SYSEXE/LINK.EXE" | grep -q 'INTERP' || { echo "FAIL: gen1 LINK.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/LINK.EXE"
cp "$SYSEXE/LINK.EXE" "$WORK/LINK.gen1"

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
    [ "${LINK_SELFHOST_EXPECT:-1}" = "1" ] && { echo "FAIL: DCL.EXE build failed (regression)"; exit 1; }
    echo "SKIP (LINK_SELFHOST_EXPECT=0): DCL.EXE build failed."; exit 2
fi
readelf -lW "$SYSEXE/DCL.EXE" | grep -q 'INTERP' || { echo "FAIL: DCL.EXE has no PT_INTERP"; exit 1; }
chmod +x "$SYSEXE/DCL.EXE"

echo
echo "== stage BUILD.COM + LINK.EXE's own sources into the project directory =="
cp "$REPO/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/BUILD.COM" "$SYSEXE/BUILD.COM"
# The project directory is the system-disk root ([000000] -> /vms). Stage the
# LINK.EXE translation units under their VMS (upper-case) names so BUILD.COM's
# `'SRC'.C` resolves them: LINK.C = link.c, OVMX_LINK_RMS_IO.C = the RMS shim.
PROJ=/vms
rm -f "$PROJ"/LINK.OBJ* "$PROJ"/link.obj* \
      "$PROJ"/OVMX_LINK_RMS_IO.OBJ* "$PROJ"/ovmx_link_rms_io.obj* \
      "$PROJ"/LINKSH.EXE* "$PROJ"/linksh.exe*
cp "$LINK_DIR/link.c"               "$PROJ/LINK.C"
cp "$LINK_DIR/ovmx_link_rms_io.c"   "$PROJ/OVMX_LINK_RMS_IO.C"

# CFLAGS handed to TCC.EXE inside DCL: the SAME defines/includes mk_link.sh uses
# to compile link.c, but tcc ignores gcc-only codegen flags so only the -D/-I
# set matters.  Absolute container paths -- headers are data, not tools; no host
# compiler/linker is in this path.  -DOVMX_RMS_IO selects link.c's RMS I/O seams.
# -I$LINK_DIR: ovmx_link_rms_io.h sits next to the .c (src/vmslink/), NOT under
# include/ where ovmx_image.h/ovmx_symvec.h live -- gcc finds it via quoted-
# include-from-source-dir, but tcc's including file is the staged /vms/LINK.C, so
# the header's real directory must be an explicit -I.
LC_CFLAGS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_RMS_IO -I$LINK_DIR -I$LINK_DIR/include -I$SRC/vmsrms/include -I$SRC/libvms/include"

# DCL driver scripts: NORUN=1 (a linker cannot be RUN with no args); CFLAGS set.
# Written as `=` (quoted) so the include paths keep their case (`:=` upcases).
# BOTH generations build the SAME output name (LINKSH): identical inputs -- same
# objects, same producers, SAME output name -- is the correct fixpoint test, and
# it removes any dependence on whether the image format embeds the output
# basename.  The two runs are disambiguated on disk by cleaning between them.
BUILD_ONE='$ CFLAGS = "%s"\n$ NORUN = "1"\n$ @SYS$SYSTEM:BUILD LINKSH LINK OVMX_LINK_RMS_IO\n$ EXIT\n'
# shellcheck disable=SC2059
printf "$BUILD_ONE" "$LC_CFLAGS" > "$PROJ/DOgen2.com"
# shellcheck disable=SC2059
printf "$BUILD_ONE" "$LC_CFLAGS" > "$PROJ/DOgen3.com"

echo "-- SYSEXE: --"; ls -1 "$SYSEXE" | sed 's/^/   /'
echo "-- project dir ($PROJ) sources: --"; ls -1 "$PROJ" | grep -iE 'link|ovmx_link|dogen' | sed 's/^/   /'

find_output () {
    # $1 = base name (e.g. LINKG2); echo the real on-disk image path (highest RMS
    # ;version), or empty. vmsfs lowercases the unqualified name and RMS mints a
    # ;version suffix, and sys$create also drops a "<image>;N.rms_meta" sidecar
    # next to it -- exclude the sidecar and pick the highest pure-digit ;version.
    lc=$(echo "$1" | tr 'A-Z' 'a-z')
    ls "$PROJ"/"$lc".exe\;* "$PROJ"/"$1".EXE\;* 2>/dev/null \
        | grep -vE '\.rms_meta$' \
        | sort -t';' -k2 -n \
        | tail -1
}

echo
echo "== gen2: DCL session #1 -- @BUILD LINKSH LINK OVMX_LINK_RMS_IO (linked by gen1) =="
set +e
( cd "$PROJ" && VMS_ROOT=/vms VMS_DEFAULT_DIR='SYS$SYSDEVICE:[000000]' \
    "$SYSEXE/DCL.EXE" "$PROJ/DOgen2.com" ) > "$WORK/gen2.out" 2>&1
G2RC=$?
set -e
echo "-- @BUILD gen2 session output: --"; sed 's/^/   /' "$WORK/gen2.out"
echo "-- DCL.EXE exit=$G2RC --"
grep -q '%BUILD-S-OK' "$WORK/gen2.out" || { echo "FAIL: gen2 @BUILD did not report success (%BUILD-S-OK)"; exit 1; }
[ "$G2RC" -eq 0 ] || { echo "FAIL: the gen2 @BUILD session did not exit clean (got $G2RC)"; exit 1; }
GEN2=$(find_output LINKSH)
[ -n "$GEN2" ] || { echo "FAIL: gen2 @BUILD did not produce LINKSH.EXE"; exit 1; }
readelf -lW "$GEN2" | grep -q 'INTERP' || { echo "FAIL: gen2 LINK.EXE has no PT_INTERP (IMGACT)"; exit 1; }
cp "$GEN2" "$WORK/LINK.gen2"
echo "-- gen2 image: $GEN2 ($(wc -c < "$WORK/LINK.gen2") bytes) --"

echo
echo "== install gen2 as SYS\$SYSTEM:LINK.EXE (the OVMX-built linker becomes the linker) =="
cp "$WORK/LINK.gen2" "$SYSEXE/LINK.EXE"
chmod +x "$SYSEXE/LINK.EXE"
# Clean the gen2 output + objects so gen3 is unambiguously the run #2 product.
rm -f "$PROJ"/linksh.exe* "$PROJ"/LINKSH.EXE* \
      "$PROJ"/link.obj* "$PROJ"/LINK.OBJ* \
      "$PROJ"/ovmx_link_rms_io.obj* "$PROJ"/OVMX_LINK_RMS_IO.OBJ*

echo
echo "== gen3: DCL session #2 -- @BUILD LINKSH LINK OVMX_LINK_RMS_IO (linked by gen2) =="
set +e
( cd "$PROJ" && VMS_ROOT=/vms VMS_DEFAULT_DIR='SYS$SYSDEVICE:[000000]' \
    "$SYSEXE/DCL.EXE" "$PROJ/DOgen3.com" ) > "$WORK/gen3.out" 2>&1
G3RC=$?
set -e
echo "-- @BUILD gen3 session output: --"; sed 's/^/   /' "$WORK/gen3.out"
echo "-- DCL.EXE exit=$G3RC --"
grep -q '%BUILD-S-OK' "$WORK/gen3.out" || { echo "FAIL: gen3 @BUILD did not report success -- the OVMX-BUILT gen2 LINK.EXE failed to link LINK.EXE"; exit 1; }
[ "$G3RC" -eq 0 ] || { echo "FAIL: the gen3 @BUILD session did not exit clean (got $G3RC)"; exit 1; }
GEN3=$(find_output LINKSH)
[ -n "$GEN3" ] || { echo "FAIL: gen3 @BUILD did not produce LINKSH.EXE"; exit 1; }
readelf -lW "$GEN3" | grep -q 'INTERP' || { echo "FAIL: gen3 LINK.EXE has no PT_INTERP (IMGACT)"; exit 1; }
cp "$GEN3" "$WORK/LINK.gen3"
echo "-- gen3 image: $GEN3 ($(wc -c < "$WORK/LINK.gen3") bytes) --"

echo
echo "== FIXPOINT: gen2 == gen3 (byte-stable self-link) =="
S2=$(sha256sum "$WORK/LINK.gen2" | cut -d' ' -f1)
S3=$(sha256sum "$WORK/LINK.gen3" | cut -d' ' -f1)
echo "   gen2 sha256 = $S2"
echo "   gen3 sha256 = $S3"
if ! cmp -s "$WORK/LINK.gen2" "$WORK/LINK.gen3"; then
    echo "FAIL: gen2 != gen3 -- the OVMX-built LINK.EXE does NOT reproduce itself byte-for-byte."
    echo "      first differing bytes:"; cmp "$WORK/LINK.gen2" "$WORK/LINK.gen3" | sed 's/^/      /' || true
    exit 1
fi

echo
echo "================================================================================"
echo "MILESTONE (vms-62b, self-host S4 -- OVMX builds OVMX from within): the OVMX-native"
echo "TCC.EXE + LINK.EXE, driven by a multi-TU BUILD.COM from a DCL session with ZERO"
echo "bash in the build path, compiled and linked the OVMX linker LINK.EXE (link.c +"
echo "ovmx_link_rms_io.c) into gen2, then the OVMX-BUILT gen2 LINK.EXE linked LINK.EXE"
echo "again into gen3.  gen2 and gen3 are BYTE-IDENTICAL ($S2)"
echo "-- a true self-hosting fixpoint: the OVMX-built linker reproduces itself."
echo "================================================================================"
