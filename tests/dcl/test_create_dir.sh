#!/bin/bash
# TEST: CREATE/DIRECTORY creates a new directory
# EXPECT: contains:NEWDIR
# EXPECT_NOT: contains:mkdir:
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nCREATE/DIRECTORY [.NEWDIR]\nDIRECTORY [.NEWDIR]\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
