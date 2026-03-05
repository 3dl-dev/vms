#!/bin/bash
# TEST: DIFFERENCES command compares two files
# EXPECT: regex:(Difference|differ|Number of|CHANGED_LINE)
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
printf "line one\nline two\nline three\n" > "/vms/$TDIR/file1.txt"
printf "line one\nCHANGED_LINE\nline three\n" > "/vms/$TDIR/file2.txt"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nDIFFERENCES file1.txt file2.txt\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
