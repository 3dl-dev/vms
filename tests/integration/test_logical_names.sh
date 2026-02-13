#!/bin/bash
# Logical Name Integration Tests
set -e

VMSDCL="${VMSDCL:-./build/bin/vmsdcl}"
PASS=0
FAIL=0

echo "=== Logical Name Tests ==="

# Test basic DEFINE/SHOW
result=$(echo 'DEFINE TESTLOG "Hello World"
SHOW LOGICAL TESTLOG
EXIT' | $VMSDCL 2>&1)
if echo "$result" | grep -q "Hello World"; then
    echo "PASS: Basic DEFINE/SHOW"
    PASS=$((PASS + 1))
else
    echo "FAIL: Basic DEFINE/SHOW"
    FAIL=$((FAIL + 1))
fi

# Test DEASSIGN
result=$(echo 'DEFINE TESTLOG "value"
DEASSIGN TESTLOG
SHOW LOGICAL TESTLOG
EXIT' | $VMSDCL 2>&1)
if echo "$result" | grep -qi "no .* found\|NOLOGNAM"; then
    echo "PASS: DEASSIGN"
    PASS=$((PASS + 1))
else
    echo "FAIL: DEASSIGN"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
