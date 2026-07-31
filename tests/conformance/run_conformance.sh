#!/bin/bash
#
# run_conformance.sh - VMS Conformance Test Runner
#
# Runs VMS C programs from tests/conformance/vms_programs/ inside the ovmx-test
# Docker container to verify VMS API compatibility.
#
# ---------------------------------------------------------------------------
# WHAT THIS HARNESS CAN AND CANNOT ASSERT -- READ BEFORE ADDING A PROGRAM
#
# It runs on a plain Linux host in a tooling container. There is no /dev/vms
# here and there never will be: the only OVMX runtime is the kernel/QEMU path
# (CLAUDE.md Rule 9). So this harness can only assert VMS behaviour that is
# EXECUTIVE-INDEPENDENT -- descriptors, FAO, $GETMSG, the math/string/time RTL,
# constant values. A program here that calls a system service backed by the
# executive is asserting that the service SUCCEEDS WITH NO EXECUTIVE PRESENT,
# which is a state OpenVMS is never in and OVMX refuses to boot into (vms-0ff).
#
# MOVED OUT FOR EXACTLY THAT REASON (vms-2a8):
#
#   vms_programs/test_event_flags.c  ->  tests/qemu/test_syssvc_ef_local.c
#
# It exercised $SETEF/$CLREF/$READEF/$WAITFR and passed here for eight rounds
# because src/libvms/syssvc/sys_event.c kept all 128 event flags in
# per-process memory and never called the executive -- the facade vms-2a8
# deleted (Rule 11). The moment sys_event.c became a reader of /dev/vms, all
# eight of its checks failed with status 0x2a4 (SS$_BUGCHECK, what vms_kif
# returns when /dev/vms is absent). Its green had been the facade's green.
#
# It was NOT weakened and NOT skipped -- under Rule 10 a permanently skipped
# test is a failing test, and keeping it green where it stood would have
# required giving $SETEF the per-process fallback the epic exists to delete.
# All thirteen of its checks moved verbatim and three were added; see that
# file's header. This is the same move vms-8019 made with the host lib$getjpi
# block in tests/libvms/test_lib_rtl.c.
# ---------------------------------------------------------------------------
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
