#!/bin/sh
# build_link_native.sh — CMake-driven entrypoint for the VMS-native LINK.EXE
# shareable-image graph (bead vms-b6a, pillar vms-ade). Wires the mk_*_shr.sh /
# mk_dcl.sh / mk_loginout.sh recipes (src/vmslink/mk_libvms_shr.sh et al, the
# vms-b65/c39 lib-migration chain) into `cmake --build` so the SHIPPED OVMX
# libraries are VMS-native LINK.EXE shareables (ET_DYN + .vms$sv, NO
# DT_NEEDED) instead of ld-linked .so files.
#
# aarch64 AND x86_64 (vms-6da extends the aarch64-only graph vms-b6a wired:
# ARCH is now auto-detected from $CC -dumpmachine, the same detection
# CMakeLists.txt already uses to turn OVMX_LINK_NATIVE on, and threaded
# through to every mk_*_shr.sh recipe as CFLAGS/ARCH -- the SAME env-
# overridable convention lib_build_graph.sh's build_producer_graph()
# established for the raw-harness path (vms-cb5f/vms-a66), not a second
# custom target or a forked script).
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
# Env:    CC (default gcc), WORK (default /tmp/ovmx-link-native), ARCH
#         (default: detected from `$CC -dumpmachine`; aarch64 or x86_64)
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

CC_TRIPLE=$($CC -dumpmachine 2>/dev/null || true)
case "$CC_TRIPLE" in
    *aarch64*) DETECTED_ARCH=aarch64 ;;
    *x86_64*)  DETECTED_ARCH=x86_64 ;;
    *) DETECTED_ARCH= ;;
esac
ARCH=${ARCH:-$DETECTED_ARCH}
[ -n "$ARCH" ] || { echo "build_link_native: FAIL: could not detect ARCH from \$CC" \
        "($CC -dumpmachine -> '$CC_TRIPLE') and none given -- expected aarch64 or x86_64" >&2; exit 1; }
case "$ARCH" in
    aarch64) ARCHFLAG="-mno-outline-atomics" ;;
    x86_64)  ARCHFLAG="-mtls-dialect=gnu2" ;;
    *) echo "build_link_native: FAIL: unsupported ARCH=$ARCH (expected aarch64 or x86_64)" >&2; exit 1 ;;
esac
LIBCFLAGS="-fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -U_FORTIFY_SOURCE $ARCHFLAG"

LIBC=${LIBC:-$($CC -print-file-name=libc.a)}
LIBGCC=${LIBGCC:-$($CC -print-libgcc-file-name)}
case "$LIBC" in
    *musl*) : ;;
    *) echo "build_link_native: FAIL: \$CC ($CC) libc.a is '$LIBC' -- does not look" \
            "like musl. This graph is musl-only (docs/design-link-native-toolchain.md);" \
            "run inside the arm64/amd64 musl container/CI job, not the glibc dev toolchain." >&2
       exit 1 ;;
esac
[ -f "$LIBC" ]   || { echo "build_link_native: FAIL: no musl libc.a at $LIBC" >&2; exit 1; }
[ -f "$LIBGCC" ] || { echo "build_link_native: FAIL: no libgcc.a at $LIBGCC" >&2; exit 1; }

echo "build_link_native: LINK.EXE=$LINK_EXE  CC=$CC  ARCH=$ARCH  SYSLIB=$SYSLIB  SYSEXE=$SYSEXE"

echo "== LIBVMSSYS\$SHR.EXE =="
CC="$CC" ARCH="$ARCH" CFLAGS="$LIBCFLAGS" WORK="$WORK/vmssys" sh "$HERE/mk_vmssys_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SRC/libvmssys"

echo "== DECC\$SHR.EXE =="
WORK="$WORK/decc" sh "$HERE/mk_decc_shr.sh" \
    "$LINK_EXE" "$SYSLIB/DECC\$SHR.EXE" "$LIBC" "$LIBGCC"

echo "== LIBVMSPROCESS\$SHR.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/vmsprocess" sh "$HERE/mk_vmsprocess_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "" "$SRC/vmsprocess" "$SRC/libvms/include"

echo "== LIBVMSLNM\$SHR.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/vmslnm" sh "$HERE/mk_vmslnm_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SRC/vmslnm" "$SRC/libvms/include"

echo "== LIBVMSFS\$SHR.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/vmsfs" sh "$HERE/mk_vmsfs_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SRC/vmsfs" "$SRC/libvms/include" "$SRC/vmslnm/include"

echo "== LIBVMS\$SHR.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/libvms" sh "$HERE/mk_libvms_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" "$SYSLIB/LIBVMSSYS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SRC/libvms"

echo "== LIBVMSRMS\$SHR.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/vmsrms" sh "$HERE/mk_vmsrms_shr.sh" \
    "$LINK_EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" "$SYSLIB/DECC\$SHR.EXE" \
    "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSFS\$SHR.EXE" \
    "$SRC/vmsrms" "$SRC/libvms/include" "$SRC/vmsfs/include"

echo "== DCL.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/dcl" sh "$HERE/mk_dcl.sh" \
    "$LINK_EXE" "$SYSEXE/DCL.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$SRC/vmsdcl" "$SRC"

echo "== LOGINOUT.EXE =="
CC="$CC" CFLAGS="$LIBCFLAGS" WORK="$WORK/loginout" sh "$HERE/mk_loginout.sh" \
    "$LINK_EXE" "$SYSEXE/LOGINOUT.EXE" \
    "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
    "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
    "$SYSLIB/LIBVMSSYS\$SHR.EXE" "$REPO/tools/vms_login.c" "$SRC"

echo "build_link_native: full VMS-native LINK.EXE graph built ($ARCH) -- 5 libs + DECC\$SHR + DCL.EXE + LOGINOUT.EXE"
