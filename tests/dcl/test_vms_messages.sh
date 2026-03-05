#!/bin/bash
# TEST: Error messages use VMS %FAC-SEV-IDENT format
# EXPECT: regex:%[A-Z]+-[WSEIF]-[A-Z]+,
# EXPECT: contains:VMS_MSG_FORMAT_OK
# EXPECT_NOT: contains:VMS_MSG_FORMAT_FAIL
VMSDCL="${VMSDCL:-vmsdcl}"
FAILURES=0

check_vms_format() {
    local desc="$1"
    local output="$2"
    # Extract lines that look like error messages (start with %)
    local err_lines
    err_lines=$(echo "$output" | grep '^%' || true)
    if [ -z "$err_lines" ]; then
        # No error output — that's ok if the command succeeded
        return 0
    fi
    # Every line starting with % must match %FAC-SEV-IDENT, format
    while IFS= read -r line; do
        if ! echo "$line" | grep -qE '^%[A-Z]+-[WSEIF]-[A-Z]+, '; then
            echo "  BAD FORMAT in $desc: $line"
            FAILURES=$((FAILURES + 1))
        fi
    done <<< "$err_lines"
}

# Test 1: Invalid command verb → %DCL-E-IVVERB
output=$(echo "XYZZY_INVALID_CMD" | $VMSDCL 2>&1)
check_vms_format "invalid verb" "$output"
if echo "$output" | grep -q '%DCL-E-IVVERB'; then
    echo "PASS: IVVERB format"
    echo "$output" | grep '^%'
else
    echo "FAIL: IVVERB format"
fi

# Test 2: Missing SHOW keyword → %DCL-E-NOKEYW
output=$(echo "SHOW" | $VMSDCL 2>&1)
check_vms_format "show no keyword" "$output"

# Test 3: SET DEFAULT to invalid dir → %DCL-E-NODIR or %DCL-E-DIRECT
output=$(echo "SET DEFAULT [.NONEXISTENT_QWERTY_DIR]" | $VMSDCL 2>&1)
check_vms_format "set default invalid" "$output"

# Test 4: DELETE nonexistent file → should have VMS format
output=$(echo "DELETE NONEXISTENT_FILE_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "delete nonexistent" "$output"

# Test 5: TYPE nonexistent file → should have VMS format
output=$(echo "TYPE NONEXISTENT_FILE_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "type nonexistent" "$output"

# Test 6: RENAME nonexistent file → should have VMS format
output=$(echo "RENAME NONEXISTENT_FILE_QWERTY.TXT NEW_QWERTY.TXT" | $VMSDCL 2>&1)
check_vms_format "rename nonexistent" "$output"

# Test 7: IF without ENDIF (flow control error) → VMS format
output=$(printf 'IF 1 .EQ. 1\n' | $VMSDCL 2>&1)
check_vms_format "if without endif" "$output"

# Test 8: ELSE without IF → %DCL-E-NOIFBLK
output=$(echo "ELSE" | $VMSDCL 2>&1)
check_vms_format "else without if" "$output"

# Test 9: GOTO with no label → %DCL-E-NOLAB
output=$(echo "GOTO" | $VMSDCL 2>&1)
check_vms_format "goto no label" "$output"

# Test 10: RETURN without GOSUB → %DCL-E-NOGOSUB
output=$(echo "RETURN" | $VMSDCL 2>&1)
check_vms_format "return without gosub" "$output"

# Test 11: Unrecognized SHOW keyword → %DCL-E-IVKEYW
output=$(echo "SHOW XYZZY_INVALID" | $VMSDCL 2>&1)
check_vms_format "show invalid keyword" "$output"

if [ $FAILURES -eq 0 ]; then
    echo "VMS_MSG_FORMAT_OK"
else
    echo "VMS_MSG_FORMAT_FAIL ($FAILURES bad format messages)"
fi
