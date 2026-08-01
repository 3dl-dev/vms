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
    for expect in "${expects[@]}"; do
        if [[ "$expect" == regex:* ]]; then
            local pattern="${expect#regex:}"
            if ! echo "$output" | grep -qE "$pattern"; then
                echo "FAIL: $description"
                echo "  Expected regex: $pattern"
                echo "  Output: $(echo "$output" | head -5)"
                failed=1
                break
            fi
        elif [[ "$expect" == contains:* ]]; then
            local needle="${expect#contains:}"
            if ! echo "$output" | grep -qF "$needle"; then
                echo "FAIL: $description"
                echo "  Expected to contain: $needle"
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
                if echo "$output" | grep -qE "$pattern"; then
                    echo "FAIL: $description"
                    echo "  Should NOT match regex: $pattern"
                    echo "  Output: $(echo "$output" | head -5)"
                    failed=1
                    break
                fi
            elif [[ "$expect_not" == contains:* ]]; then
                local needle="${expect_not#contains:}"
                if echo "$output" | grep -qF "$needle"; then
                    echo "FAIL: $description"
                    echo "  Should NOT contain: $needle"
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
