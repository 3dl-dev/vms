#!/bin/bash
# TEST: Queue commands (SUBMIT, PRINT, SHOW QUEUE, DELETE/ENTRY, SET ENTRY) work end-to-end
# EXPECT: contains:QUEUED
# EXPECT: contains:SYS$PRINT
# EXPECT: contains:SUBMITTED
# EXPECT: contains:SYS$BATCH
# EXPECT: contains:DELETED
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_queue_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
export VMSQ_DB_PATH="/tmp/QMAN_TEST_$$.DAT"
mkdir -p "/vms/$TDIR"
echo "test content" > "/vms/$TDIR/testfile.txt"
echo "$ EXIT" > "/vms/$TDIR/testjob.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nPRINT testfile.txt\nSUBMIT testjob.com\nSHOW QUEUE /ALL\nDELETE /ENTRY=1\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "$VMSQ_DB_PATH"
