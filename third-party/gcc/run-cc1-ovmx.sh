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
    # SYSEXE/SYSLIB MUST be the canonical INTERP path: LINK.EXE stamps every
    # image PT_INTERP=/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE (link.c IMGACT_INTERP),
    # so IMGACT.EXE and the --use producers have to live there at activation or
    # the kernel reports "not found" (exit 127) on an otherwise-valid image.
    # The working vmslink/imgact activation tests (run_test_x86_64_tls.sh et al.)
    # build the graph straight into these paths — mirror them.
    SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE; SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
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

    # DEFINITIVE probe: cc1 is the compiler PROPER (not the gcc driver), so make
    # it actually COMPILE. This exercises the whole pipeline as an OVMX image —
    # integrated preprocess -> parse -> gimple -> RTL -> x86_64 asm emission ->
    # and the OUTPUT FILE WRITE through the executive (RMS/$QIO). If probe.s
    # comes out with real assembly, cc1 genuinely runs; a clean --version alone
    # could be a trivial early-exit.
    echo "== step 5: DEFINITIVE — cc1 compiles a real TU (parse->RTL->asm->file write over the executive) =="
    printf "int square(int x){ return x*x; }\nint cube(int x){ return x*square(x); }\n" > /tmp/probe.c
    rm -f /tmp/probe.s
    set +e
    "$SYSEXE/CC1.EXE" -quiet -O1 /tmp/probe.c -o /tmp/probe.s > /result/05-cc1-compile.out 2>&1
    R5=$?
    set -e
    echo "-- cc1 compile exit=$R5; /result/05 log: --"; sed "s/^/   /" /result/05-cc1-compile.out
    if [ -s /tmp/probe.s ]; then
        echo "-- probe.s emitted ($(wc -l < /tmp/probe.s) lines) — head: --"
        sed "s/^/   /" /tmp/probe.s | head -30
        if grep -qE "square:" /tmp/probe.s && grep -qiE "imul|mul" /tmp/probe.s; then
            echo "run-cc1-ovmx: PROVEN — cc1 compiled C to x86_64 assembly as an OVMX image (square/cube present, multiply emitted)."
        else
            echo "run-cc1-ovmx: PARTIAL — probe.s emitted but expected symbols/insns not found; inspect above."
        fi
        cp /tmp/probe.s /result/probe.s 2>/dev/null || true
    else
        echo "run-cc1-ovmx: cc1 produced NO probe.s (exit=$R5) — activation runs but compile pipeline did not emit output; THIS is the next executive wall."
        # cc1 is built -g, so a backtrace NAMES the faulting frame — turn
        # "segfault" into an actionable location (cc1 fn vs IMGACT vs a null
        # deferred-import stub). gdb runs CC1.EXE through its IMGACT INTERP.
        if [ "$R5" -ge 128 ]; then
            echo "== step 6: gdb backtrace of the compile-time fault (cc1 has -g symbols) =="
            apk add --no-cache gdb >/dev/null 2>&1 || echo "  (gdb install failed — skipping backtrace)"
            if command -v gdb >/dev/null 2>&1; then
                gdb -batch -nx \
                    -ex "set pagination off" \
                    -ex "run" \
                    -ex "echo \n==FAULT==\n" \
                    -ex "bt 40" \
                    -ex "info registers rip rsp" \
                    --args "$SYSEXE/CC1.EXE" -quiet -O1 /tmp/probe.c -o /tmp/probe.s \
                    > /result/06-cc1-gdb-bt.txt 2>&1
                echo "-- gdb backtrace (tail): --"
                sed "s/^/   /" /result/06-cc1-gdb-bt.txt | tail -45
            fi
        fi
    fi
    echo "== step 7: isolate the fault — argv delivery + stdin->stdout compile (no file write) =="
    # (7a) argv delivery: real cc1 rejects a bogus flag (exit 1 + message). If
    # CC1.EXE exits 0 silently, IMGACT is not delivering argv to the image.
    set +e
    "$SYSEXE/CC1.EXE" --ovmx-bogus-flag > /result/07a-argv.out 2>&1
    R7A=$?
    echo "-- (7a) cc1 --ovmx-bogus-flag: exit=$R7A, output: --"; sed "s/^/   /" /result/07a-argv.out
    # (7b) compile from STDIN to STDOUT — exercises parse->RTL->asm WITHOUT the
    # RMS output-file-create path. If asm appears here but the -o probe.s run
    # faulted, the wall is the executive file WRITE, not the compile pipeline.
    printf "int seven(void){ return 7; }\n" | "$SYSEXE/CC1.EXE" -quiet - > /result/07b-stdin.out 2>&1
    R7B=$?
    set -e
    echo "-- (7b) cc1 -quiet - (stdin->stdout): exit=$R7B, output: --"; sed "s/^/   /" /result/07b-stdin.out | head -20
    if grep -qiE "seven|ret|mov" /result/07b-stdin.out; then
        echo "run-cc1-ovmx: cc1 COMPILES from stdin->stdout — the fault is the file-write (RMS) path, not the compiler."
    fi
    echo "run-cc1-ovmx: DONE (activation exit=$R4, compile exit=$R5)"
' 2>&1 | tee "$RESULT_DIR/00-full.log"
