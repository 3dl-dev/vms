#!/bin/bash
#
# run_conformance.sh - VMS Conformance Test Runner
#
# Runs VMS C programs from tests/conformance/vms_programs/ inside the ovmx-test
# Docker container to verify VMS API compatibility.
#

set -e

# Color output for better readability
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Counters
TOTAL=0
PASSED=0
FAILED=0

# Test directory
TEST_DIR="/tests/vms_programs"
BUILD_DIR="/tmp/conformance_build"

echo "VMS Conformance Test Suite"
echo "============================"
echo

# Create build directory
mkdir -p "$BUILD_DIR"

# Compilation flags
INCLUDE_FLAGS="-I/src/src/libvms/include -I/src/src/vmsprocess/include -I/src/src/vmsfs/include -I/src/src/vmsrms/include"
# The VMS runtime libraries are built as OpenVMS-style shareable images
# (OUTPUT_NAME "LIBVMS$SHR", SUFFIX ".EXE" — see src/libvms/CMakeLists.txt),
# so plain -lvms/-lvmsfs/... do NOT resolve (ld looks for libvms.so). Link
# the exact filenames with -l:NAME. Only libvmssys keeps a conventional
# archive name (libvmssys.a), so -lvmssys still works for it.
LIB_FLAGS="-L/src/build/lib -l:LIBVMSRMS\$SHR.EXE -l:LIBVMSFS\$SHR.EXE -l:LIBVMS\$SHR.EXE -l:LIBVMSPROCESS\$SHR.EXE -lvmssys -lpthread -lm"
CFLAGS="-Wall -Wextra -O2"

# Find all .c files in test directory
if [ ! -d "$TEST_DIR" ]; then
    echo -e "${RED}ERROR: Test directory $TEST_DIR not found${NC}"
    exit 1
fi

# Process each test
for test_file in "$TEST_DIR"/*.c; do
    if [ ! -f "$test_file" ]; then
        continue
    fi

    TOTAL=$((TOTAL + 1))

    test_name=$(basename "$test_file" .c)
    test_bin="$BUILD_DIR/$test_name"

    echo -e "${YELLOW}Test: $test_name${NC}"

    # Compile.
    # NOTE: `set -e` is active, so gcc must be guarded by `if !` — a bare
    # `gcc ...; rc=$?` would abort the whole script on the first compile
    # failure, before any diagnostic is printed (which is exactly how this
    # harness used to die silently on program 1).
    echo -n "  Compiling... "
    if ! gcc $CFLAGS $INCLUDE_FLAGS -o "$test_bin" "$test_file" $LIB_FLAGS > "$BUILD_DIR/${test_name}_compile.log" 2>&1; then
        echo -e "${RED}FAILED${NC}"
        echo "  Compilation errors:"
        head -10 "$BUILD_DIR/${test_name}_compile.log" | sed 's/^/    /'
        FAILED=$((FAILED + 1))
        echo
        continue
    fi
    echo -e "${GREEN}OK${NC}"

    # Run test
    echo -n "  Running...   "
    if LD_LIBRARY_PATH=/src/build/lib "$test_bin" > "$BUILD_DIR/${test_name}_output.log" 2>&1; then
        test_exit=$?
    else
        test_exit=$?
    fi

    if [ $test_exit -eq 0 ]; then
        echo -e "${GREEN}PASS${NC}"
        PASSED=$((PASSED + 1))

        # Show test output
        if [ -s "$BUILD_DIR/${test_name}_output.log" ]; then
            cat "$BUILD_DIR/${test_name}_output.log" | sed 's/^/    /'
        fi
    else
        echo -e "${RED}FAIL (exit code: $test_exit)${NC}"
        FAILED=$((FAILED + 1))

        # Show test output
        if [ -s "$BUILD_DIR/${test_name}_output.log" ]; then
            echo "  Output:"
            cat "$BUILD_DIR/${test_name}_output.log" | sed 's/^/    /'
        fi
    fi

    echo
done

# Summary
echo "============================"
echo "Summary:"
echo "  Total:  $TOTAL"
echo -e "  ${GREEN}Passed: $PASSED${NC}"
if [ $FAILED -gt 0 ]; then
    echo -e "  ${RED}Failed: $FAILED${NC}"
else
    echo "  Failed: $FAILED"
fi
echo

# Exit code
if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed.${NC}"
    exit 1
fi
