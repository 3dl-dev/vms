#!/bin/bash
# TEST: TYPE command displays file contents
# EXPECT: contains:HELLO_VMS_TYPE_TEST
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
mkdir -p "/vms/$TDIR"
echo "HELLO_VMS_TYPE_TEST" > "/vms/$TDIR/testfile.txt"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nTYPE testfile.txt\n' "$(echo "$TDIR" | tr a-z A-Z)" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
