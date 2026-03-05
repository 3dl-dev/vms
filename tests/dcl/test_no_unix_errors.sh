#!/bin/bash
# TEST: Error messages must not contain Unix strerror text
# EXPECT: contains:UNIX_ERROR_CHECK_COMPLETE
# EXPECT_NOT: contains:UNIX_ERROR_DETECTED
VMSDCL="${VMSDCL:-vmsdcl}"
UNIX_FOUND=0

# Unix strerror patterns that should never appear in VMS error messages
# These are common glibc strerror() return strings
UNIX_ERROR_PATTERNS="(No such file or directory|Permission denied|File exists|Is a directory|Not a directory|No space left on device|Read-only file system|Too many open files|Bad file descriptor|Input/output error|Invalid argument|Operation not permitted|Resource temporarily unavailable|Device or resource busy|Text file busy|Directory not empty)"

check_no_unix() {
    local desc="$1"
    local output="$2"
    if echo "$output" | grep -qE "$UNIX_ERROR_PATTERNS"; then
        echo "  UNIX_ERROR_DETECTED in $desc:"
        echo "$output" | grep -E "$UNIX_ERROR_PATTERNS" | head -3 | sed 's/^/    /'
        UNIX_FOUND=$((UNIX_FOUND + 1))
    fi
}

# Test commands that trigger errno-based errors
# Each should produce VMS-format errors, not Unix strerror text

# 1. File not found (ENOENT)
output=$(echo "DELETE NONEXISTENT_QWERTY_FILE.TXT" | $VMSDCL 2>&1)
check_no_unix "DELETE nonexistent" "$output"

# 2. File not found via TYPE (ENOENT)
output=$(echo "TYPE NONEXISTENT_QWERTY_FILE.TXT" | $VMSDCL 2>&1)
check_no_unix "TYPE nonexistent" "$output"

# 3. COPY nonexistent source (ENOENT)
output=$(echo "COPY NONEXISTENT_QWERTY_SRC.TXT NONEXISTENT_QWERTY_DST.TXT" | $VMSDCL 2>&1)
check_no_unix "COPY nonexistent" "$output"

# 4. RENAME nonexistent file (ENOENT)
output=$(echo "RENAME NONEXISTENT_QWERTY_FILE.TXT NEW_QWERTY.TXT" | $VMSDCL 2>&1)
check_no_unix "RENAME nonexistent" "$output"

# 5. SEARCH nonexistent file (ENOENT)
output=$(printf 'SEARCH NONEXISTENT_QWERTY_FILE.TXT "pattern"\n' | $VMSDCL 2>&1)
check_no_unix "SEARCH nonexistent" "$output"

# 6. OPEN nonexistent file
output=$(echo "OPEN/READ F NONEXISTENT_QWERTY_FILE.TXT" | $VMSDCL 2>&1)
check_no_unix "OPEN nonexistent" "$output"

# 7. SET DEFAULT invalid directory
output=$(echo "SET DEFAULT [.NONEXISTENT_QWERTY_DIR]" | $VMSDCL 2>&1)
check_no_unix "SET DEFAULT invalid" "$output"

# 8. CREATE /DIRECTORY on read-only path (may fail with EACCES or succeed)
output=$(echo "CREATE /DIRECTORY SYS\$SYSDEVICE:[NONEXISTENT_QWERTY_DIR.SUB1.SUB2.SUB3]" | $VMSDCL 2>&1)
check_no_unix "CREATE dir deep" "$output"

if [ $UNIX_FOUND -gt 0 ]; then
    echo "UNIX_ERROR_DETECTED: $UNIX_FOUND commands used Unix error text"
else
    echo "All error messages use VMS format"
fi
echo "UNIX_ERROR_CHECK_COMPLETE ($UNIX_FOUND issues)"
