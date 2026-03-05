#!/bin/bash
# TEST: SEARCH command finds text in files
# EXPECT: contains:FINDME_MARKER
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
printf "line one\nFINDME_MARKER here\nline three\n" > "/vms/$TDIR/searchfile.txt"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSEARCH searchfile.txt "FINDME_MARKER"\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
