#!/bin/bash
# TEST: SHOW QUEUE and SHOW INTRUSION work
# EXPECT: contains:SYS$BATCH
# EXPECT: regex:(NOINTRUSION|Intrusion)
VMSDCL="${VMSDCL:-vmsdcl}"
export VMSQ_DB_PATH="/tmp/QMAN_TEST_$$.DAT"
TDIR="dcl_sqe_test_$$"
mkdir -p "/vms/$TDIR"
echo "$ EXIT" > "/vms/$TDIR/testjob.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSUBMIT testjob.com\nSHOW QUEUE /ALL\nSHOW INTRUSION\n' "$(echo $TDIR | tr a-z A-Z)" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "$VMSQ_DB_PATH"
