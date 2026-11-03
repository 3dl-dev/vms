#!/bin/bash
# Negative control for the host harness's RMS output route (rd vms-a6ce).
#
# run_dcl_tests.sh hands each test dcl_rms_run as $VMSDCL: DCL starts with
# SYS$OUTPUT/SYS$ERROR naming a disk file and its stdout/stderr on the null
# device, so the only way its output reaches a test is as records DCL $PUT into
# that file through RMS, read back by rms_read_records with $GET. This proves
# the route is load-bearing: it builds DCL in a sandbox, runs a few ported
# tests through the real harness, then MUTATES DCL so a file SYS$OUTPUT is no
# longer bound to the RMS route (dcl_io.c dcl_out_bind_ppf: the FILE branch
# never taken, so DCL's records fall to the stdio fallback) and requires every
# one of those tests to go RED. A harness that still passed would be reading
# DCL's output some other way than through the file.
#
# Usage: negctl_rms_route.sh <source-root>   (DCL_RMS_RUN: the build's dcl_rms_run)
# Not named test_*.sh: the harness must not collect its own control.
set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
if [ ! -x "${DCL_RMS_RUN:-}" ]; then
    echo "FAIL: DCL_RMS_RUN (the build's dcl_rms_run) is not set or not executable"
    exit 1
fi
command -v cmake >/dev/null 2>&1 || {
    echo "FAIL: cmake is not available, so no mutant can be built -- reported FAILED, never skipped"
    exit 1
}

# Ported tests whose assertions are on records DCL's sink writes -- SHOW SYMBOL
# output and %DCL- messages -- not on a WRITE SYS$OUTPUT, which (with
# SYS$OUTPUT a file) $PUTs its record through RMS on its own path, route or
# no route (dcl_cmd_io.c), and so stays green under this mutation.
CASES="test_builtin_exit_status.sh test_parser_errors.sh test_symbol_scope_domain.sh"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

ROOT="$WORK/tree"
mkdir -p "$ROOT" "$WORK/suite"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp "$SRC_ROOT/CMakeLists.txt" "$ROOT/CMakeLists.txt"
cp "$SRC_ROOT/tests/dcl/run_dcl_tests.sh" "$WORK/suite/"
for c in $CASES; do
    [ -f "$SRC_ROOT/tests/dcl/$c" ] || { echo "FAIL: control case $c is missing"; exit 1; }
    cp "$SRC_ROOT/tests/dcl/$c" "$WORK/suite/"
done

BUILD="$WORK/build"
build_dcl() {
    cmake -B "$BUILD" -S "$ROOT" -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF >"$WORK/cmake.log" 2>&1 || return 1
    cmake --build "$BUILD" --target vmsdcl -j"$(nproc)" >"$WORK/build.log" 2>&1 || return 1
    [ -x "$BUILD/bin/DCL.EXE" ]
}
run_suite() {
    VMSDCL="$BUILD/bin/DCL.EXE" DCL_RMS_RUN="$DCL_RMS_RUN" \
        bash "$WORK/suite/run_dcl_tests.sh" >"$WORK/$1.log" 2>&1
}
n_cases=$(echo $CASES | wc -w)
status=0

echo "vms-a6ce negative control: DCL's output must reach the harness only through the RMS file route"

# Positive: the unmutated sandbox DCL passes every case through the route.
if ! build_dcl; then
    echo "  FAIL: the sandbox DCL did not build"; tail -20 "$WORK/build.log"; exit 1
fi
if run_suite pos && grep -q "Results: $n_cases passed, 0 failed" "$WORK/pos.log"; then
    echo "  PASS: positive control -- unmutated DCL passes $n_cases/$n_cases through dcl_rms_run"
else
    echo "  FAIL: positive control -- unmutated DCL does not pass the cases"
    sed 's/^/    | /' "$WORK/pos.log"
    status=1
fi

# Mutant: a file SYS$OUTPUT/SYS$ERROR is never bound to the RMS route.
IO_C="$ROOT/src/vmsdcl/dcl_io.c"
needle='} else if (is_null_device(fd)) {'
if ! grep -qF "$needle" "$IO_C"; then
    echo "  FAIL: mutation could not be applied -- dcl_out_bind_ppf's FILE branch moved, so this control tested NOTHING"
    exit 1
fi
sed -i 's/} else if (is_null_device(fd)) {/} else if (0 \&\& is_null_device(fd)) {/' "$IO_C"
if ! build_dcl; then
    echo "  FAIL: the mutant DCL did not build"; tail -20 "$WORK/build.log"; exit 1
fi
run_suite mut
red=$(grep -c '^FAIL: ' "$WORK/mut.log")
if [ "$red" -eq "$n_cases" ] && grep -q "Results: 0 passed, $n_cases failed" "$WORK/mut.log"; then
    echo "  PASS: mutant (file route dropped) -- all $n_cases cases RED"
else
    echo "  FAIL: mutant (file route dropped) -- $red of $n_cases cases red; the harness does not depend on the RMS route"
    sed 's/^/    | /' "$WORK/mut.log"
    status=1
fi

[ "$status" = 0 ] && echo "vms-a6ce negative control: PASS" || echo "vms-a6ce negative control: FAIL"
exit "$status"
