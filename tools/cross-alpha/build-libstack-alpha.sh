#!/usr/bin/env bash
#
# build-libstack-alpha.sh - Rung A3 of the OVMX-on-Alpha epic (rd vms-fed).
#
# Cross-builds the FULL OVMX userland library stack for alpha-linux-gnu and runs
# its compiled unit tests under qemu-alpha (user mode). Proves the RTL stack --
# vmsprocess -> libvms -> {vmslnm -> vmsfs -> vmsrms} and vmsdcl, plus vmsqueue/
# ods2 -- is LP64-clean on a 64-bit big-register Alpha target.
#
# Build/TEST tooling only (Rule 9): compiling and emulating the freestanding +
# glibc-hosted RTL to prove it ports; never a runtime.
#
# Usage:  tools/cross-alpha/build-libstack-alpha.sh
# Requires: the ovmx-cross-alpha image (built here if absent).
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE=ovmx-cross-alpha
BUILD=/tmp/ovmx-alpha-libstack

cd "$REPO_ROOT"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">> building $IMAGE"
    docker build -t "$IMAGE" -f tools/cross-alpha/Dockerfile tools/cross-alpha
fi

mkdir -p "$BUILD"

docker run --rm \
    -v "$REPO_ROOT":/src:ro \
    -v "$BUILD":/b \
    "$IMAGE" bash -euo pipefail -c '
    export QEMU_LD_PREFIX=/usr/alpha-linux-gnu

    echo "== configure (alpha cross toolchain, static, tests on) =="
    cmake -S /src -B /b \
        -DCMAKE_TOOLCHAIN_FILE=/src/tools/cross-alpha/toolchain-alpha-linux.cmake \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTS=ON -DBUILD_TOOLS=OFF -DOVMX_STATIC=ON >/b/configure.log 2>&1

    echo "== build the library stack =="
    for t in vmsprocess vms vmslnm vmsfs vmsrms vmsqueue vmsdcl; do
        cmake --build /b --target "$t" -j"$(nproc)" >/b/build-$t.log 2>&1 \
            || { echo "BUILD FAILED: $t"; tail -30 /b/build-$t.log; exit 1; }
        echo "  built $t"
    done

    echo "== build + run the compiled unit tests under qemu-alpha =="
    # Build every in-scope unit-test binary. tests/qemu (executive/kernel) is
    # out of scope for this rung -- it has no __alpha__ asm bootstrap yet.
    cmake --build /b -j"$(nproc)" -- -k >/b/build-tests.log 2>&1 || true

    pass=0; fail=0; skip=0; failed=""
    run() {
        local b="$1"; shift
        [ -x "/b/bin/$b" ] || { echo "  MISSING $b"; fail=$((fail+1)); failed="$failed $b(missing)"; return; }
        if timeout 120 qemu-alpha "/b/bin/$b" "$@" >/tmp/o 2>&1; then
            pass=$((pass+1))
        else
            rc=$?
            if [ "$rc" = 77 ]; then skip=$((skip+1)); echo "  SKIP $b (env: $(head -1 /tmp/o))"; return; fi
            fail=$((fail+1)); failed="$failed $b(rc=$rc)"; echo "  FAIL($rc) $b"; tail -5 /tmp/o
        fi
    }

    # core libs
    for b in test_vmsprocess test_vmslnm test_vmsfs test_searchlist_open \
             test_vmsrms test_concealed_rooted test_disk_transparency \
             test_idx_close_flush test_iterative_translation test_sysroot_composed \
             test_vmsqueue; do run "$b"; done
    # libvms suite (two need an argument / env; supply them)
    for b in /b/bin/test_libvms_*; do
        base=$(basename "$b")
        case "$base" in
            test_libvms_sysuaf_auth) run "$base" /src/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/SYSUAF.DAT ;;
            *) run "$base" ;;
        esac
    done
    # scs, ods2, dcl
    for b in /b/bin/test_scs_*; do run "$(basename "$b")"; done
    for b in /b/bin/test_ods2*;  do
        base=$(basename "$b")
        if [ "$base" = test_ods2_real ]; then
            ODS2_REAL_FIXTURE=/src/tests/ods2/real_vax_ods2.dsk run "$base"
        else run "$base"; fi
    done
    run test_dcl_prompt_echo_race
    run test_help_engine

    # DCL longword grounding (rd vms-c71 oracle): SHOW SYMBOL must render an
    # integer as a LONGWORD on Alpha exactly as on OpenVMS VAX V7.3.
    echo "== DCL SHOW SYMBOL longword grounding (alpha) =="
    printf "IDENT_L = -2147483644\nSHOW SYMBOL IDENT_L\nIDENT_D = 8388736\nSHOW SYMBOL IDENT_D\n" \
        | timeout 30 qemu-alpha /b/bin/DCL.EXE 2>&1 | grep -iE "IDENT_[LD]" > /tmp/sym || true
    cat /tmp/sym
    if grep -qE "IDENT_L = -2147483644[[:space:]]+Hex = 80000004[[:space:]]+Octal = 20000000004$" /tmp/sym \
       && grep -qE "IDENT_D = 8388736[[:space:]]+Hex = 00800080[[:space:]]+Octal = 00040000200$" /tmp/sym; then
        echo "  PASS: longword rendering matches the VAX V7.3 oracle"
    else
        echo "  FAIL: longword rendering diverged from the oracle"; fail=$((fail+1)); failed="$failed dcl_longword_grounding"
    fi

    echo ""
    echo "RESULT: pass=$pass fail=$fail skip=$skip"
    [ -n "$failed" ] && echo "FAILED:$failed"
    [ "$fail" = 0 ] || exit 1
    echo "PASS: OVMX lib stack cross-builds for alpha-linux and its unit tests pass under qemu-alpha"
'
