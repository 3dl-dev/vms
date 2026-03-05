#!/bin/bash
# TEST: SET DEFAULT changes working directory
# EXPECT: contains:SUBDIR
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR/subdir"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSET DEFAULT [.SUBDIR]\nSHOW DEFAULT\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
