#!/bin/sh
# mk_librarian.sh — build recipe for LIBRARIAN.EXE AS a VMS-native EXECUTABLE
# image (bead vms-1c2, self-host S3 spine, epic vms-59a). Mirrors mk_link.sh
# exactly: librarian.c + the generic whole-file RMS I/O shim (ovmx_link_rms_io.c,
# reused from LINK.EXE's native image) are compiled with the proven
# freestanding-musl CFLAGS and -DOVMX_OLB_RMS_IO, then linked by a bootstrap
# LINK.EXE via `--executable --use {DECC$SHR + the five OVMX shareables}` into a
# PT_INTERP=IMGACT.EXE image. IMGACT activates the result and it runs AS an OVMX
# image — reading its input .OBJ and .OLB through RMS (sys$open/$get) and writing
# the .OLB it creates through RMS (sys$create/$put).
#
# TWO LINK.EXE ROLES, DO NOT CONFLATE (the same distinction mk_link.sh draws):
# the <LINK.EXE> argument is a BOOTSTRAP linker (an ordinary host tool, the
# `vmslink` CMake target) used to BUILD the native LIBRARIAN.EXE image. The
# OUTPUT is the NATIVE LIBRARIAN.EXE image, which runs under IMGACT inside OVMX.
# The gate this bead closes is that the OUTPUT runs natively — the host tool
# merely builds it, which CLAUDE.md Rule 9 explicitly allows (a build step is not
# an activation proof; activation is proven by run_librarian_native.sh).
#
# RMS I/O (vms-1c2): librarian.c + ovmx_olb.h carry `#ifdef OVMX_OLB_RMS_IO`
# seams at every file-I/O site — the .OBJ member read (read_obj), the .OLB read
# (olb__slurp), the .OLB write (olb_write, which assembles the whole ar image in
# one buffer then writes it), and the /EXTRACT write (cmd_extract). Each routes
# through ovmx_link_rms_io.c's whole-file slurp/write over src/vmsrms's
# sys$open/$connect/$get/$create/$put/$close. A DISTINCT macro (OVMX_OLB_RMS_IO,
# not LINK's OVMX_RMS_IO) so link.c and dcl_library.c, which also include
# ovmx_olb.h, are byte-for-byte unaffected.
#
# Usage:  mk_librarian.sh <bootstrap-LINK.EXE> <out-LIBRARIAN.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             [repo-src-dir]
# Env:    CC (default gcc), CFLAGS (default freestanding-musl for detected ARCH),
#         ARCH (default: detected from `$CC -dumpmachine`; aarch64 or x86_64)
# Must run in the musl container where the producer .EXE already exist.
set -e

LINK_EXE=${1:?usage: mk_librarian.sh <bootstrap-LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> [repo-src]}
OUT=${2:?need output LIBRARIAN.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
HERE=$(cd "$(dirname "$0")" && pwd)                      # src/vmslink
SRC=${9:-$(cd "$HERE/.." && pwd)}                        # src
CC=${CC:-gcc}

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR"; do
    [ -f "$f" ] || { echo "mk_librarian: producer image not found: $f"; exit 1; }
done
[ -f "$HERE/librarian.c" ]        || { echo "mk_librarian: librarian.c not found in $HERE"; exit 1; }
[ -f "$HERE/ovmx_link_rms_io.c" ] || { echo "mk_librarian: ovmx_link_rms_io.c not found in $HERE"; exit 1; }

WORK=${WORK:-/tmp/mk-librarian}
mkdir -p "$WORK"

# ARCH-specific codegen flag, same convention as mk_link.sh.
CC_TRIPLE=$($CC -dumpmachine 2>/dev/null || true)
case "$CC_TRIPLE" in
    *aarch64*) DETECTED_ARCH=aarch64 ;;
    *x86_64*)  DETECTED_ARCH=x86_64 ;;
    *) DETECTED_ARCH= ;;
esac
ARCH=${ARCH:-$DETECTED_ARCH}
case "$ARCH" in
    aarch64) ARCHFLAG="-mno-outline-atomics" ;;
    x86_64)  ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "mk_librarian: FAIL: unsupported/undetected ARCH=$ARCH (expected aarch64 or x86_64)"; exit 1 ;;
esac

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector $ARCHFLAG -U_FORTIFY_SOURCE}"
# OVMX_OLB_RMS_IO switches librarian.c + ovmx_olb.h to the RMS whole-file paths.
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_OLB_RMS_IO"
# -I$HERE so ovmx_olb.h (in include/) can find ovmx_link_rms_io.h (in $HERE).
INCS="-I$HERE -I$HERE/include -I$SRC/vmsrms/include -I$SRC/libvms/include"

echo "mk_librarian: CC=$CC ARCH=$ARCH"
echo "mk_librarian: cc librarian.c + ovmx_link_rms_io.c (freestanding-musl, -DOVMX_OLB_RMS_IO)"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/librarian.o"        "$HERE/librarian.c"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/ovmx_link_rms_io.o" "$HERE/ovmx_link_rms_io.c"

echo "mk_librarian: bootstrap LINK.EXE --executable --use {6 producers} -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" \
    -o "$OUT" "$WORK/librarian.o" "$WORK/ovmx_link_rms_io.o"

echo "mk_librarian: created $OUT"
