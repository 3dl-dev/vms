#!/bin/sh
#
# test_cmake_shims_tracked_negctl.sh - negative controls for the vms-4847 gate.
#
# WHY THIS EXISTS. test_cmake_shims_tracked.sh is a lint over the real repo.
# A lint nobody has tried to evade is an assertion about nothing (see
# test_runtime_target_negctl.sh's header for the same argument). This proves,
# in a disposable sandbox git repo (never the real tree, never the real
# .gitignore), that the gate actually reds for BOTH ways a shim can go
# missing, and does NOT red for the innocent case:
#
#   1. referenced, and untracked (present on disk, never `git add`-ed, or
#      re-ignored by .gitignore) -> gate must FAIL
#   2. referenced, and absent from disk entirely -> gate must FAIL
#   3. referenced, present on disk, tracked -> gate must PASS
#
# Each case gets its own minimal fixture so a mutation that reds for the
# wrong reason (e.g. case 2 accidentally reported as case 1's message) is
# still visible to a human reading -v output, even though this script only
# asserts exit status per case.
#
# Usage: test_cmake_shims_tracked_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_cmake_shims_tracked.sh"

if [ ! -x "$GATE" ] && [ ! -f "$GATE" ]; then
    echo "FAIL: gate script not found at $GATE"
    exit 1
fi

if ! command -v git >/dev/null 2>&1; then
    echo "FAIL: git not found -- cannot run sandbox negative controls"
    exit 1
fi

status=0
passed=0
failed=0

check() {
    label="$1"
    expect="$2"   # "pass" or "fail"
    WORK=$(mktemp -d)
    (
        cd "$WORK" || exit 1
        git init -q .
        git config user.email test@example.invalid
        git config user.name test
        mkdir -p sub
        : > sub/dummy_no_python3.cmake  # placeholder so `git add .` below has SOMETHING to commit even in case 2 fixtures
        rm -f sub/dummy_no_python3.cmake
    ) >/dev/null 2>&1
    "$3" "$WORK"
    out=$("$GATE" "$WORK" 2>&1)
    rc=$?
    if [ "$expect" = "pass" ]; then
        if [ "$rc" = 0 ]; then
            echo "PASS: $label (gate exited 0 as expected)"
            passed=$((passed + 1))
        else
            echo "FAIL: $label -- expected gate to PASS (exit 0), got exit $rc:"
            echo "$out" | sed 's/^/    /'
            failed=$((failed + 1))
            status=1
        fi
    else
        if [ "$rc" != 0 ]; then
            echo "PASS: $label (gate exited $rc as expected)"
            passed=$((passed + 1))
        else
            echo "FAIL: $label -- expected gate to FAIL (nonzero exit), got exit 0:"
            echo "$out" | sed 's/^/    /'
            failed=$((failed + 1))
            status=1
        fi
    fi
    rm -rf "$WORK"
}

# Case 1: referenced, present on disk, but NEVER git-added (the "silent"
# failure mode -- indistinguishable from success by `git status` alone once
# .gitignore is swallowing it, or simply forgotten).
fixture_untracked() {
    d="$1"
    cat > "$d/CMakeLists.txt" <<'EOF'
add_test(NAME fixture_gate
         COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/fixture_no_python3.cmake)
EOF
    echo 'message(FATAL_ERROR "no python3")' > "$d/fixture_no_python3.cmake"
    ( cd "$d" && git add CMakeLists.txt && git commit -q -m init )
    # fixture_no_python3.cmake deliberately left OUT of the commit.
}
check "referenced + present-but-untracked shim" fail fixture_untracked

# Case 2: referenced, but absent from disk entirely (e.g. deleted after the
# CMakeLists.txt reference was written, or never written at all).
fixture_absent() {
    d="$1"
    cat > "$d/CMakeLists.txt" <<'EOF'
add_test(NAME fixture_gate
         COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/fixture_no_python3.cmake)
EOF
    ( cd "$d" && git add CMakeLists.txt && git commit -q -m init )
    # fixture_no_python3.cmake never created at all.
}
check "referenced + entirely absent shim" fail fixture_absent

# Case 3: referenced, present, and properly tracked -- the innocent case the
# gate must NOT flag.
fixture_tracked() {
    d="$1"
    cat > "$d/CMakeLists.txt" <<'EOF'
add_test(NAME fixture_gate
         COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/fixture_no_python3.cmake)
EOF
    echo 'message(FATAL_ERROR "no python3")' > "$d/fixture_no_python3.cmake"
    ( cd "$d" && git add CMakeLists.txt fixture_no_python3.cmake && git commit -q -m init )
}
check "referenced + present + tracked shim" pass fixture_tracked

echo ""
echo "cmake shims tracked negative controls: $passed passed, $failed failed"
exit $status
