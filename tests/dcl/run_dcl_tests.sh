#!/bin/bash
# DCL Integration Test Harness
# Finds all test_*.sh files in this directory, runs each, checks annotations.
#
# Annotation format in test files:
#   # TEST: <description>
#   # EXPECT: regex:<pattern>        — output must match regex
#   # EXPECT: contains:<string>      — output must contain string
#   # EXPECT_NOT: regex:<pattern>    — output must NOT match regex
#   # EXPECT_NOT: contains:<string>  — output must NOT contain string
#
# Each test file is a bash script that runs vmsdcl commands and produces output.
# The harness captures stdout+stderr and checks against annotations.

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Find vmsdcl binary — it may be named DCL.EXE in the build
if [ -n "${VMSDCL:-}" ]; then
    : # already set
elif command -v vmsdcl &>/dev/null; then
    VMSDCL=vmsdcl
elif command -v DCL.EXE &>/dev/null; then
    VMSDCL=DCL.EXE
else
    # Search common build locations
    for candidate in ./build/bin/DCL.EXE ./build/bin/vmsdcl ../build/bin/DCL.EXE; do
        if [ -x "$candidate" ]; then
            VMSDCL="$candidate"
            break
        fi
    done
fi
VMSDCL="${VMSDCL:-vmsdcl}"
export VMSDCL
TIMEOUT="${DCL_TEST_TIMEOUT:-10}"

# Ensure /vms directory exists (required for VMS path resolution)
if [ ! -d /vms ]; then
    mkdir -p /vms 2>/dev/null || true
fi
PASS=0
FAIL=0
SKIP=0
ERRORS=""

# Verify vmsdcl is available
if ! command -v "$VMSDCL" &>/dev/null; then
    echo "ERROR: vmsdcl not found in PATH and VMSDCL not set"
    echo "Set VMSDCL=/path/to/vmsdcl or add it to PATH"
    exit 1
fi

echo "=== DCL Integration Test Suite ==="
echo "Using: $(command -v "$VMSDCL")"
echo "Timeout: ${TIMEOUT}s per test"
echo ""

# Match $output against a pattern.
#
# Every call MUST pass the pattern after `--`. Without it, a pattern that
# begins with a dash is parsed by grep as an OPTION: grep prints
# "unrecognized option" and exits 2, and because 2 is neither 0 nor 1 the
# caller's plain `if grep ...` reads it as "did not match". For an EXPECT that
# is a spurious failure; for an EXPECT_NOT it is far worse — the assertion
# passes UNCONDITIONALLY and can never go red. That is exactly what happened to
# `# EXPECT_NOT: contains:---` in test_show_system.sh, the guard that keeps
# Rule 10 placeholder markers out of SHOW SYSTEM: it was inert from the day it
# was written (vms-6a7 round 2).
#
# So callers must also distinguish grep's three exit codes rather than testing
# for zero: 0 = matched, 1 = did not match, >=2 = grep could not evaluate the
# pattern at all. The third is a harness error and must fail the test loudly,
# never be folded into "did not match" — a pattern that cannot be evaluated is
# an assertion that cannot fail.
grep_match() {
    local flags="$1"
    local pattern="$2"
    printf '%s\n' "$output" | grep "$flags" -- "$pattern"
}

run_test() {
    local test_file="$1"
    local test_name
    test_name="$(basename "$test_file" .sh)"

    # Extract test description
    local description
    description=$(grep '^# TEST:' "$test_file" | head -1 | sed 's/^# TEST: *//')
    if [ -z "$description" ]; then
        description="$test_name"
    fi

    # Extract EXPECT annotations
    local expects=()
    local expect_nots=()
    while IFS= read -r line; do
        expects+=("${line#\# EXPECT: }")
    done < <(grep '^# EXPECT:' "$test_file" | grep -v '^# EXPECT_NOT:')
    while IFS= read -r line; do
        expect_nots+=("${line#\# EXPECT_NOT: }")
    done < <(grep '^# EXPECT_NOT:' "$test_file")

    # Skip if no annotations
    if [ ${#expects[@]} -eq 0 ] && [ ${#expect_nots[@]} -eq 0 ]; then
        echo "SKIP: $description (no EXPECT annotations)"
        SKIP=$((SKIP + 1))
        return 0
    fi

    # Run the test with timeout
    local output
    local exit_code
    output=$(timeout "$TIMEOUT" bash "$test_file" 2>&1) || true
    exit_code=$?

    if [ $exit_code -eq 124 ]; then
        echo "FAIL: $description (TIMEOUT after ${TIMEOUT}s)"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  TIMEOUT: $test_name"
        return 1
    fi

    # Check EXPECT annotations
    local failed=0
    local mrc
    for expect in "${expects[@]}"; do
        if [[ "$expect" == regex:* ]]; then
            local pattern="${expect#regex:}"
            grep_match -qE "$pattern"; mrc=$?
            if [ $mrc -ne 0 ]; then
                echo "FAIL: $description"
                if [ $mrc -gt 1 ]; then
                    echo "  HARNESS ERROR: grep could not evaluate regex: $pattern"
                else
                    echo "  Expected regex: $pattern"
                fi
                echo "  Output: $(echo "$output" | head -5)"
                failed=1
                break
            fi
        elif [[ "$expect" == contains:* ]]; then
            local needle="${expect#contains:}"
            grep_match -qF "$needle"; mrc=$?
            if [ $mrc -ne 0 ]; then
                echo "FAIL: $description"
                if [ $mrc -gt 1 ]; then
                    echo "  HARNESS ERROR: grep could not evaluate needle: $needle"
                else
                    echo "  Expected to contain: $needle"
                fi
                echo "  Output: $(echo "$output" | head -5)"
                failed=1
                break
            fi
        fi
    done

    # Check EXPECT_NOT annotations (only if not already failed)
    if [ $failed -eq 0 ]; then
        for expect_not in "${expect_nots[@]}"; do
            if [[ "$expect_not" == regex:* ]]; then
                local pattern="${expect_not#regex:}"
                grep_match -qE "$pattern"; mrc=$?
                if [ $mrc -ne 1 ]; then
                    echo "FAIL: $description"
                    if [ $mrc -gt 1 ]; then
                        echo "  HARNESS ERROR: grep could not evaluate regex: $pattern"
                    else
                        echo "  Should NOT match regex: $pattern"
                    fi
                    echo "  Output: $(echo "$output" | head -5)"
                    failed=1
                    break
                fi
            elif [[ "$expect_not" == contains:* ]]; then
                local needle="${expect_not#contains:}"
                grep_match -qF "$needle"; mrc=$?
                if [ $mrc -ne 1 ]; then
                    echo "FAIL: $description"
                    if [ $mrc -gt 1 ]; then
                        echo "  HARNESS ERROR: grep could not evaluate needle: $needle"
                    else
                        echo "  Should NOT contain: $needle"
                    fi
                    echo "  Output: $(echo "$output" | head -5)"
                    failed=1
                    break
                fi
            fi
        done
    fi

    if [ $failed -eq 0 ]; then
        echo "PASS: $description"
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: $test_name — $description"
    fi
}

# Find and run all test files
test_files=("$SCRIPT_DIR"/test_*.sh)
if [ ${#test_files[@]} -eq 0 ] || [ ! -f "${test_files[0]}" ]; then
    echo "No test_*.sh files found in $SCRIPT_DIR"
    exit 1
fi

for test_file in "${test_files[@]}"; do
    VMSDCL="$VMSDCL" run_test "$test_file"
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
if [ -n "$ERRORS" ]; then
    echo -e "Failures:$ERRORS"
fi

[ $FAIL -eq 0 ] && exit 0 || exit 1
