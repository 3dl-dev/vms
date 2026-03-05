#!/bin/bash
# TEST: OPEN/READ/CLOSE file I/O operations work
# EXPECT: contains:FILE_IO_TEST_LINE
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
echo "FILE_IO_TEST_LINE" > "/vms/$TDIR/iotest.txt"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nOPEN/READ INFILE iotest.txt\nREAD INFILE REC\nCLOSE INFILE\nSHOW SYMBOL REC\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
