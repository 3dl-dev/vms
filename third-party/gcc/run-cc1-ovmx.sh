#!/bin/sh
# run-cc1-ovmx.sh — container driver for the cc1-as-an-OVMX-image attempt
# (bead vms-5b7e, epic vms-da0 F2b/F2c). Runs, in ONE musl (Alpine)
# container (mirrors run-host-probe-cc1.sh's shape):
#   1. rebuild-cc1-fpic.sh   -- force cc1's ~956 objects + support libs to
#                               -fPIC over the F2a-configured tree (bind-
#                               mounted from the host, reused, not re-
#                               configured).
#   2. the OVMX producer graph (IMGACT.EXE, LINK.EXE, DECC$SHR, the five
#      shareables) -- same build_producer_graph() shape as
#      src/imgact/test/lib_build_graph.sh, ARCH=x86_64.
#   3. mk_cc1_ovmx.sh        -- parse cc1's own link line, whole-archive the
#                               upstream C++ runtime + static GMP/MPFR/MPC/
#                               z, LINK.EXE --executable -> CC1.EXE.
#   4. IF CC1.EXE links: chmod +x, activate it through IMGACT.EXE (`cc1
#      --version` first -- cheapest possible activation probe), report the
#      first wall.
#
# CATALOG-ONLY: reports the first genuine wall (link / static-lib /
# activation) and stops. Does not force a working cc1. link.c/imgact.c are
# out of this bead's file-domain.
#
# Usage:  third-party/gcc/run-cc1-ovmx.sh [gcc-build-work-dir]
# Must run from repo root. Needs docker or podman.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
IMG=docker.io/library/alpine:3.20
GCC_WORK=${1:-/tmp/gcc-cc1-hostprobe}
RESULT_DIR=${RESULT_DIR:-/tmp/cc1-ovmx-result}
mkdir -p "$RESULT_DIR"

ENGINE=podman
command -v podman >/dev/null 2>&1 || ENGINE=docker
command -v "$ENGINE" >/dev/null 2>&1 || { echo "run-cc1-ovmx: neither podman nor docker found"; exit 1; }

[ -d "$GCC_WORK" ] || { echo "run-cc1-ovmx: FAIL: $GCC_WORK not found -- run third-party/gcc/run-host-probe-cc1.sh (F2a) first"; exit 1; }

"$ENGINE" run --rm -e JOBS="${JOBS:-}" -e SKIP_FPIC="${SKIP_FPIC:-}" -v "$REPO":/src:Z -v "$GCC_WORK":/gccwork:Z -v "$RESULT_DIR":/result:Z -w /src "$IMG" sh -c '
    set -e
    echo "== apk add build deps =="
    apk add --no-cache g++ gmp-dev mpfr-dev mpc1-dev zlib-dev make musl-dev binutils flex bison linux-headers readelf >/dev/null 2>&1 \
      || apk add --no-cache g++ gmp-dev mpfr-dev mpc1-dev zlib-dev make musl-dev binutils flex bison linux-headers >/dev/null 2>&1
    ARCH=x86_64
    CC=gcc
    CXX=g++

    echo "== step 1: rebuild-cc1-fpic.sh (-fPIC over the F2a tree) =="
    if [ -n "$SKIP_FPIC" ]; then
        echo "SKIP_FPIC set -- reusing the already-fPIC /gccwork tree (step 1 skipped)"
        R1=0
    else
        set +e
        sh /src/third-party/gcc/rebuild-cc1-fpic.sh /gccwork > /result/01-rebuild-fpic.log 2>&1
        R1=$?
        set -e
        tail -80 /result/01-rebuild-fpic.log
    fi
    echo "rebuild-cc1-fpic.sh exit=$R1"
    if [ "$R1" -ne 0 ]; then
        echo "STOP: -fPIC rebuild did not complete -- see /result/01-rebuild-fpic.log (THE WALL)"
        exit 0
    fi

    echo "== step 2: build the OVMX producer graph (IMGACT.EXE, LINK.EXE, DECC\$SHR, 5 shareables) =="
    WORK=/tmp/mk-cc1-graph
    rm -rf "$WORK"; mkdir -p "$WORK"
    SYSEXE="$WORK/SYSEXE"; SYSLIB="$WORK/SYSLIB"
    mkdir -p "$SYSEXE" "$SYSLIB"
    SRC=/src/src
    IMGACT_DIR="$SRC/imgact"
    LINK_DIR="$SRC/vmslink"
    LIBVMSSYS_DIR="$SRC/libvmssys"
    VMSPROC_DIR="$SRC/vmsprocess"
    VMSLNM_DIR="$SRC/vmslnm"
    VMSFS_DIR="$SRC/vmsfs"
    LIBVMS_DIR="$SRC/libvms"
    VMSRMS_DIR="$SRC/vmsrms"
    LIBVMS_INC="$LIBVMS_DIR/include"
    LNM_INC="$VMSLNM_DIR/include"
    VMSFS_INC="$VMSFS_DIR/include"
    RMS_INC="$VMSRMS_DIR/include"
    LIBC=/usr/lib/libc.a
    LIBGCC=$($CC -print-libgcc-file-name)
    export ARCH CC WORK SYSEXE SYSLIB IMGACT_DIR LINK_DIR LIBVMSSYS_DIR VMSPROC_DIR VMSLNM_DIR VMSFS_DIR LIBVMS_DIR VMSRMS_DIR LIBVMS_INC LNM_INC VMSFS_INC RMS_INC LIBC LIBGCC
    . "$SRC/imgact/test/lib_build_graph.sh"
    set +e
    build_producer_graph > /result/02-producer-graph.log 2>&1
    R2=$?
    set -e
    tail -60 /result/02-producer-graph.log
    echo "build_producer_graph exit=$R2"
    if [ "$R2" -ne 0 ]; then
        echo "STOP: OVMX producer graph did not build -- see /result/02-producer-graph.log (a pre-existing wall unrelated to cc1, blocks this bead but is not this beads finding)"
        exit 0
    fi

    echo "== step 3: mk_cc1_ovmx.sh (parse cc1 link line, static gmp/mpfr/mpc/z + whole-archive libstdc++, LINK.EXE) =="
    set +e
    CXX="$CXX" WORK=/tmp/mk-cc1 sh "$LINK_DIR/mk_cc1_ovmx.sh" \
        "$WORK/LINK.EXE" "$SYSEXE/CC1.EXE" \
        "$SYSLIB/DECC\$SHR.EXE" "$SYSLIB/LIBVMS\$SHR.EXE" "$SYSLIB/LIBVMSPROCESS\$SHR.EXE" \
        "$SYSLIB/LIBVMSFS\$SHR.EXE" "$SYSLIB/LIBVMSLNM\$SHR.EXE" "$SYSLIB/LIBVMSRMS\$SHR.EXE" \
        /gccwork > /result/03-mk-cc1-ovmx.log 2>&1
    R3=$?
    set -e
    cat /result/03-mk-cc1-ovmx.log
    echo "mk_cc1_ovmx.sh exit=$R3"

    if [ "$R3" -ne 0 ]; then
        echo "STOP: CC1.EXE did not link -- see /result/03-mk-cc1-ovmx.log (THE WALL)"
        exit 0
    fi

    echo "== step 4: CC1.EXE linked -- activate through IMGACT.EXE =="
    readelf -lW "$SYSEXE/CC1.EXE" | grep -q INTERP && echo "PT_INTERP present" || echo "NO PT_INTERP"
    chmod +x "$SYSEXE/CC1.EXE"
    set +e
    "$SYSEXE/CC1.EXE" --version > /result/04-cc1-activate.out 2>&1
    R4=$?
    set -e
    echo "-- CC1.EXE --version output (exit=$R4): --"
    sed "s/^/   /" /result/04-cc1-activate.out
    echo "run-cc1-ovmx: DONE (activation exit=$R4)"
' 2>&1 | tee "$RESULT_DIR/00-full.log"
