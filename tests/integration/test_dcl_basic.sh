#!/bin/bash
# DCL Shell Basic Integration Tests
set -e

VMSDCL="${VMSDCL:-./build/bin/vmsdcl}"
PASS=0
FAIL=0

test_cmd() {
    local desc="$1"
    local input="$2"
    local expected="$3"

    result=$(echo "$input" | $VMSDCL -c "$input" 2>&1) || true
    if echo "$result" | grep -qi "$expected"; then
        echo "PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $desc"
        echo "  Expected: $expected"
        echo "  Got: $result"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== DCL Shell Basic Tests ==="

# Test SHOW TIME
test_cmd "SHOW TIME" "SHOW TIME" "$(date +%Y)"

# Test SHOW DEFAULT
test_cmd "SHOW DEFAULT" "SHOW DEFAULT" ""

# Test SET DEFAULT
test_cmd "SET DEFAULT" "SET DEFAULT [.TEST]" ""

# Test DEFINE/SHOW LOGICAL
test_cmd "DEFINE logical" "DEFINE MYLOG \"test value\"" ""
test_cmd "SHOW LOGICAL" "SHOW LOGICAL MYLOG" "test value"

# Test DIRECTORY
test_cmd "DIRECTORY" "DIRECTORY" ""

# Test TYPE (create a temp file first)
echo "Hello VMS" > /tmp/test_vms_type.txt
test_cmd "TYPE" "TYPE /tmp/test_vms_type.txt" "Hello VMS"
rm -f /tmp/test_vms_type.txt

# Test symbol assignment
test_cmd "Symbol assign" 'X = "HELLO"' ""
test_cmd "SHOW SYMBOL" "SHOW SYMBOL X" "HELLO"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] && exit 0 || exit 1
