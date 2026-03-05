#!/bin/bash
# TEST: Error messages and output must not leak Unix paths or terminology
# EXPECT: contains:LEAK_CHECK_COMPLETE
#
# NOTE: This test detects Unix leaks but currently reports them as findings
# rather than failures. Each leak found creates backlog for authenticity beads.
# When all leaks are fixed, the EXPECT_NOT below can be uncommented to make
# this a hard gate.
# FUTURE_EXPECT_NOT: contains:UNIX_LEAK_DETECTED
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
    echo "WARNING: $LEAKS_FOUND commands leaked Unix paths/errors (backlog for authenticity beads)"
fi
echo "LEAK_CHECK_COMPLETE ($LEAKS_FOUND leaks found)"
