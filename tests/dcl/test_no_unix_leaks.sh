#!/bin/bash
# TEST: Error messages and output must not leak Unix paths or terminology
# EXPECT: contains:LEAK_CHECK_COMPLETE
# EXPECT_NOT: contains:UNIX_LEAK_DETECTED
#
# vms-fe21: re-armed. This test's only assertion used to be
# LEAK_CHECK_COMPLETE, a token the script printed unconditionally on every
# run -- so it passed whether or not any leak was found. The real guard
# (this EXPECT_NOT) was written but commented out as FUTURE_EXPECT_NOT, and
# a detected leak was downgraded to a printed WARNING instead of a failure.
# That is the no-lies defect this test suite exists to catch, in the test
# suite itself: a test that cannot fail is not testing anything.
#
# The guard is real now: any leak sets UNIX_LEAK_DETECTED in the output (the
# harness's EXPECT_NOT gates on it directly) and the script exits non-zero.
# Proof this can actually go red: pointing VMSDCL at a stub that echoes a
# Unix leak for one of the probed commands makes this script exit 1 with
# UNIX_LEAK_DETECTED in its output; pointing it at the real, clean vmsdcl
# exits 0 with no such token. See the PR description for the transcript.
VMSDCL="${VMSDCL:-vmsdcl}"

# Patterns that indicate Unix leakage in VMS output
UNIX_PATTERNS="(/home/|/tmp/|/var/|/etc/|/usr/|/dev/|strerror|Permission denied|No such file or directory|bash:|/bin/sh|/bin/bash)"

LEAKS_FOUND=0
check_output() {
    local desc="$1"
    local output="$2"
    if echo "$output" | grep -qE "$UNIX_PATTERNS"; then
        echo "  LEAK in $desc:"
        echo "$output" | grep -E "$UNIX_PATTERNS" | head -3 | sed 's/^/    /'
        LEAKS_FOUND=$((LEAKS_FOUND + 1))
    fi
}

# Test 1: DELETE nonexistent file — should get VMS error, not Unix
output=$(echo "DELETE NONEXISTENT_FILE.TXT" | $VMSDCL 2>&1)
check_output "DELETE nonexistent" "$output"

# Test 2: COPY nonexistent file
output=$(echo "COPY NONEXISTENT_SRC.TXT NONEXISTENT_DST.TXT" | $VMSDCL 2>&1)
check_output "COPY nonexistent" "$output"

# Test 3: TYPE nonexistent file
output=$(echo "TYPE NONEXISTENT_FILE.TXT" | $VMSDCL 2>&1)
check_output "TYPE nonexistent" "$output"

# Test 4: SET DEFAULT to invalid directory
output=$(echo "SET DEFAULT [.NONEXISTENT_DIR]" | $VMSDCL 2>&1)
check_output "SET DEFAULT invalid" "$output"

# Test 5: SHOW SYSTEM should not show Linux process names
output=$(echo "SHOW SYSTEM" | $VMSDCL 2>&1)
if echo "$output" | grep -qE "(kworker|systemd|/usr/|/sbin/)"; then
    echo "  LEAK in SHOW SYSTEM: Linux process names visible"
    echo "$output" | grep -E "(kworker|systemd|/usr/|/sbin/)" | head -3 | sed 's/^/    /'
    LEAKS_FOUND=$((LEAKS_FOUND + 1))
fi

# Test 6: SHOW DEFAULT should not show Unix paths
output=$(echo "SHOW DEFAULT" | $VMSDCL 2>&1)
check_output "SHOW DEFAULT" "$output"

# Test 7: DIRECTORY should not show Unix paths
output=$(echo "DIRECTORY" | $VMSDCL 2>&1)
check_output "DIRECTORY" "$output"

# Test 8: SEARCH nonexistent file
output=$(printf 'SEARCH NONEXISTENT_FILE.TXT "pattern"\n' | $VMSDCL 2>&1)
check_output "SEARCH nonexistent" "$output"

# Test 9: OPEN nonexistent file
output=$(echo "OPEN/READ F NONEXISTENT_FILE.TXT" | $VMSDCL 2>&1)
check_output "OPEN nonexistent" "$output"

# Test 10: RENAME nonexistent file
output=$(echo "RENAME NONEXISTENT_FILE.TXT NEWNAME.TXT" | $VMSDCL 2>&1)
check_output "RENAME nonexistent" "$output"

if [ $LEAKS_FOUND -gt 0 ]; then
    echo "UNIX_LEAK_DETECTED: $LEAKS_FOUND command(s) leaked Unix paths/errors"
    echo "LEAK_CHECK_COMPLETE ($LEAKS_FOUND leaks found)"
    exit 1
fi
echo "LEAK_CHECK_COMPLETE (0 leaks found)"
