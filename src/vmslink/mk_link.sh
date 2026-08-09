#!/bin/sh
# mk_link.sh — build recipe for LINK.EXE AS a VMS-native EXECUTABLE image
# (bead vms-b5a, self-host S3.1). The OVMX linker linking ITSELF: link.c +
# its RMS I/O shim (ovmx_link_rms_io.c) are compiled with the proven
# freestanding-musl CFLAGS and -DOVMX_RMS_IO, then linked by a bootstrap
# LINK.EXE via `--executable --use {DECC$SHR + the five OVMX shareables}` into a
# PT_INTERP=IMGACT.EXE image. IMGACT activates the result and it runs AS an OVMX
# image — reading its input object through RMS (sys$open/$get) and writing its
# output image through RMS (sys$create/$put). Mirrors mk_tcc.sh / mk_dcl.sh
# exactly (same CFLAGS, same producer graph, same --executable link step).
#
# TWO LINK.EXE ROLES, DO NOT CONFLATE (the same distinction mk_tcc.sh draws for
# tcc): the <LINK.EXE> argument is a BOOTSTRAP linker (an ordinary host tool, the
# `vmslink` CMake target) used to BUILD the native image. The OUTPUT is the
# NATIVE LINK.EXE image, which runs under IMGACT inside OVMX. The gate this bead
# closes is that the OUTPUT runs natively — the host tool merely builds it, which
# CLAUDE.md Rule 9 / docs/design-link-native-toolchain.md explicitly allow (a
# build step is not an activation proof; activation is proven by run_link_native.sh).
#
# RMS I/O (vms-b5a): link.c carries four `#ifdef OVMX_RMS_IO` seams (slurp's
# object read, emit's image write, file_is_archive's magic peek — the fourth is
# the include). ovmx_link_rms_io.c implements them over src/vmsrms's
# sys$open/$connect/$get/$create/$put/$close. Read is byte-exact (FAB$C_FIX
# mrs=1: no EOF space-pad); write is byte-exact (FAB$C_FIX mrs=0: per-put length).
# --use producer shareable reads (load_producer) are left on the stock open()
# path — the direct analog of the header-search reads tcc left on stock in
# vms-4ba.5. See ovmx_link_rms_io.h.
#
# Usage:  mk_link.sh <bootstrap-LINK.EXE> <out-LINK.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             [repo-src-dir]
# Env:    CC (default gcc), CFLAGS (default freestanding-musl for detected ARCH),
#         ARCH (default: detected from `$CC -dumpmachine`; aarch64 or x86_64)
# Must run in the musl container where the producer .EXE already exist.
set -e

LINK_EXE=${1:?usage: mk_link.sh <bootstrap-LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> [repo-src]}
OUT=${2:?need output LINK.EXE path}
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
    [ -f "$f" ] || { echo "mk_link: producer image not found: $f"; exit 1; }
done
[ -f "$HERE/link.c" ] || { echo "mk_link: link.c not found in $HERE"; exit 1; }

WORK=${WORK:-/tmp/mk-link}
mkdir -p "$WORK"

# ARCH-specific codegen flag, same convention as build_link_native.sh /
# lib_build_graph.sh: aarch64 needs no outline-atomic helpers DECC$SHR lacks;
# x86_64 needs TLSDESC codegen (not that link.c uses TLS, but keep the flag
# uniform with every other native image build).
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
    *) echo "mk_link: FAIL: unsupported/undetected ARCH=$ARCH (expected aarch64 or x86_64)"; exit 1 ;;
esac

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector $ARCHFLAG -U_FORTIFY_SOURCE}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DOVMX_RMS_IO"
INCS="-I$HERE/include -I$SRC/vmsrms/include -I$SRC/libvms/include"

echo "mk_link: CC=$CC ARCH=$ARCH"
echo "mk_link: cc link.c + ovmx_link_rms_io.c (freestanding-musl, -DOVMX_RMS_IO)"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/link.o" "$HERE/link.c"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/ovmx_link_rms_io.o" "$HERE/ovmx_link_rms_io.c"

echo "mk_link: bootstrap LINK.EXE --executable --use {6 producers} -> $OUT"
# shellcheck disable=SC2086
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" \
    -o "$OUT" "$WORK/link.o" "$WORK/ovmx_link_rms_io.o"

echo "mk_link: created $OUT"
