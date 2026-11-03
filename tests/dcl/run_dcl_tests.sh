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
#
# DCL'S OUTPUT COMES THROUGH RMS (rd vms-a6ce). A test's $VMSDCL is not DCL.EXE
# itself but dcl_rms_run (tests/dcl/dcl_rms_run.sh, built next to DCL.EXE): it
# starts DCL with SYS$OUTPUT/SYS$ERROR assigned to a disk file and its stdout/
# stderr on the null device, so DCL's records reach that file by RMS $PUT, and
# hands them back read with RMS $GET (rms_read_records). The assertions are the
# same strings; what they are checked against is the records of DCL's output
# file, not a stdio pipe. The real image is $DCL_RMS_REAL.
#
# A test that cannot run that way says so, with the rd item that tracks it:
#   # DCL_OUTPUT: stdio <rd-item> <why>
# and gets DCL.EXE as $VMSDCL, as before. There is no silent fallback: with no
# dcl_rms_run beside DCL.EXE the harness refuses to run.

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
DCL_RMS_REAL="$(command -v "$VMSDCL" 2>/dev/null || echo "$VMSDCL")"
DCL_RMS_RUN="${DCL_RMS_RUN:-$(dirname "$DCL_RMS_REAL")/dcl_rms_run}"
export DCL_RMS_REAL
if [ ! -x "$DCL_RMS_RUN" ]; then
    echo "ERROR: no dcl_rms_run at $DCL_RMS_RUN (DCL's RMS output route, rd vms-a6ce)"
    echo "Build the dcl_rms_run target, or set DCL_RMS_RUN"
    exit 1
fi
TIMEOUT="${DCL_TEST_TIMEOUT:-10}"

# Ensure /vms directory exists (required for VMS path resolution)
if [ ! -d /vms ]; then
    mkdir -p /vms 2>/dev/null || true
fi
PASS=0
FAIL=0
SKIP=0
ERRORS=""
STDIO=0
STDIO_LIST=""

# Verify vmsdcl is available
if ! command -v "$VMSDCL" &>/dev/null; then
    echo "ERROR: vmsdcl not found in PATH and VMSDCL not set"
    echo "Set VMSDCL=/path/to/vmsdcl or add it to PATH"
    exit 1
fi

echo "=== DCL Integration Test Suite ==="
echo "Using: $DCL_RMS_REAL (output through $DCL_RMS_RUN)"
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

    # DCL's output route: through the RMS file (dcl_rms_run), unless the test
    # names the item that keeps it on stdio
    local dcl="$DCL_RMS_RUN"
    if grep -qE '^# DCL_OUTPUT: stdio [a-z]+-[0-9a-f]+ ' "$test_file"; then
        dcl="$DCL_RMS_REAL"
        STDIO=$((STDIO + 1))
        STDIO_LIST="${STDIO_LIST}\n  $test_name ($(sed -n 's/^# DCL_OUTPUT: stdio \([a-z]*-[0-9a-f]*\) .*/\1/p' "$test_file" | head -1))"
    elif grep -q '^# DCL_OUTPUT:' "$test_file"; then
        echo "FAIL: $description (HARNESS ERROR: '# DCL_OUTPUT:' must be 'stdio <rd-item> <why>')"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: $test_name — malformed DCL_OUTPUT"
        return 1
    fi

    # Run the test with timeout
    local output
    local exit_code
    # a test that does more than one command's worth of file I/O names its
    # own bound ("# TIMEOUT: <seconds>"; each host RMS $CLOSE fsyncs)
    local t_limit
    t_limit=$(sed -n 's/^# TIMEOUT: *\([0-9][0-9]*\)$/\1/p' "$test_file" | head -1)
    t_limit="${t_limit:-$TIMEOUT}"
    output=$(VMSDCL="$dcl" timeout "$t_limit" bash "$test_file" 2>&1)
    exit_code=$?

    if [ $exit_code -eq 124 ]; then
        echo "FAIL: $description (TIMEOUT after ${t_limit}s)"
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
    run_test "$test_file"
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
echo "=== DCL output read through RMS in $(( ${#test_files[@]} - STDIO )) test files; $STDIO still on stdio ==="
if [ "$STDIO" -gt 0 ]; then
    echo -e "Still on stdio:$STDIO_LIST"
fi
if [ -n "$ERRORS" ]; then
    echo -e "Failures:$ERRORS"
fi

[ $FAIL -eq 0 ] && exit 0 || exit 1
