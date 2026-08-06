#!/bin/sh
# build_link_native.sh — CMake-driven entrypoint for the VMS-native LINK.EXE
# shareable-image graph (bead vms-b6a, pillar vms-ade). Wires the mk_*_shr.sh /
# mk_dcl.sh / mk_loginout.sh recipes (src/vmslink/mk_libvms_shr.sh et al, the
# vms-b65/c39 lib-migration chain) into `cmake --build` so the SHIPPED OVMX
# libraries are VMS-native LINK.EXE shareables (ET_DYN + .vms$sv, NO
# DT_NEEDED) instead of ld-linked .so files.
#
# aarch64-only (LINK.EXE has an x86_64 backend as of vms-cd1/2e4/bdf, but
# wiring that path into this graph is a separate item, vms-6da, which extends
# the same CMake targets this script's caller creates).
#
# NOT a test harness: unlike run_dcl_native.sh / run_login_native.sh (which
# also rebuild IMGACT.EXE + LINK.EXE from scratch per run and then ACTIVATE
# the result end-to-end), this script is invoked BY CMake with an
# already-built LINK.EXE (the `vmslink` CMake target, an ordinary host tool —
# see src/vmslink/CMakeLists.txt) and produces only the link graph. IMGACT
# activation stays out of `cmake --build`'s scope: a build step is not an
# activation proof, and CLAUDE.md Rule 9's "one runtime target" means the
# activation proof belongs to the QEMU path, not a build-time Docker step.
#
# Usage:  build_link_native.sh <LINK.EXE> <out-SYSLIB-dir> <out-SYSEXE-dir> [repo-root]
# Env:    CC (default gcc), WORK (default /tmp/ovmx-link-native)
set -e

LINK_EXE=${1:?usage: build_link_native.sh <LINK.EXE> <SYSLIB-dir> <SYSEXE-dir> [repo-root]}
SYSLIB=${2:?need output SYSLIB dir}
SYSEXE=${3:?need output SYSEXE dir}
HERE=$(cd "$(dirname "$0")" && pwd)                # src/vmslink
REPO=${4:-$(cd "$HERE/../.." && pwd)}
SRC="$REPO/src"
CC=${CC:-gcc}
WORK=${WORK:-/tmp/ovmx-link-native}
mkdir -p "$SYSLIB" "$SYSEXE" "$WORK"

LIBC=${LIBC:-$($CC -print-file-name=libc.a)}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
case "$LIBC" in
    *musl*) : ;;
    *) echo "build_link_native: FAIL: \$CC ($CC) libc.a is '$LIBC' -- does not look" \
            "like musl. This graph is musl-only (docs/design-link-native-toolchain.md);" \
            "run inside the arm64 musl container/CI job, not the glibc dev toolchain." >&2
       exit 1 ;;
esac
[ -f "$LIBC" ]   || { echo "build_link_native: FAIL: no musl libc.a at $LIBC" >&2; exit 1; }
[ -f "$LIBGCC" ] || { echo "build_link_native: FAIL: no libgcc.a at $LIBGCC" >&2; exit 1; }

echo "build_link_native: LINK.EXE=$LINK_EXE  CC=$CC  SYSLIB=$SYSLIB  SYSEXE=$SYSEXE"

echo "== LIBVMSSYS\$SHR.EXE =="
CC="$CC" WORK="$WORK/vmssys" sh "$HERE/mk_vmssys_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SRC/libvmssys"

echo "== DECC\$SHR.EXE =="
WORK="$WORK/decc" sh "$HERE/mk_decc_shr.sh" \
    "$LINK_EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"

echo "== LIBVMSPROCESS\$SHR.EXE =="
CC="$CC" WORK="$WORK/vmsprocess" sh "$HERE/mk_vmsprocess_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "" "$SRC/vmsprocess" "$SRC/libvms/include"

echo "== LIBVMSLNM\$SHR.EXE =="
CC="$CC" WORK="$WORK/vmslnm" sh "$HERE/mk_vmslnm_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SRC/vmslnm" "$SRC/libvms/include"

echo "== LIBVMSFS\$SHR.EXE =="
CC="$CC" WORK="$WORK/vmsfs" sh "$HERE/mk_vmsfs_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SRC/vmsfs" "$SRC/libvms/include" "$SRC/vmslnm/include"

echo "== LIBVMS\$SHR.EXE =="
CC="$CC" WORK="$WORK/libvms" sh "$HERE/mk_libvms_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SRC/libvms"

echo "== LIBVMSRMS\$SHR.EXE =="
CC="$CC" WORK="$WORK/vmsrms" sh "$HERE/mk_vmsrms_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SRC/vmsrms" "$SRC/libvms/include" "$SRC/vmsfs/include"

echo "== DCL.EXE =="
CC="$CC" WORK="$WORK/dcl" sh "$HERE/mk_dcl.sh" \
    "$LINK_EXE" "$SYSEXE/DCL.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SRC/vmsdcl" "$SRC"

echo "== LOGINOUT.EXE =="
CC="$CC" WORK="$WORK/loginout" sh "$HERE/mk_loginout.sh" \
    "$LINK_EXE" "$SYSEXE/LOGINOUT.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$REPO/tools/vms_login.c" "$SRC"

echo "build_link_native: full VMS-native LINK.EXE graph built -- 5 libs + DECC\$SHR + DCL.EXE + LOGINOUT.EXE"
