#!/bin/bash
# TEST: COPY and DELETE work on files
# EXPECT: contains:COPY_TEST_CONTENT
# EXPECT_NOT: contains:No such file
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
echo "COPY_TEST_CONTENT" > "/vms/$TDIR/source.txt"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nCOPY source.txt dest.txt\nTYPE dest.txt\nDELETE dest.txt\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
