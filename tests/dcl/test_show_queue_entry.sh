#!/bin/bash
# TEST: SHOW QUEUE works, and SHOW INTRUSION refuses honestly (no intrusion database in the executive)
# EXPECT: contains:SYS$BATCH
# EXPECT: contains:%SYSTEM-E-UNSUPPORTED, unsupported operation or function
# EXPECT: contains:$STATUS == "%X00000E4A"
# EXPECT_NOT: contains:NOINTRUSION
#
# SHOW INTRUSION used to read a host file nothing writes and always print
# "%SHOW-I-NOINTRUSION" with success. The executive has no $SHOW_INTRUSION,
# so the command must return SS$_UNSUPPORTED and claim no empty database.
VMSDCL="${VMSDCL:-vmsdcl}"
export VMSQ_DB_PATH="/tmp/QMAN_TEST_$$.DAT"
TDIR="dcl_sqe_test_$$"
mkdir -p "/vms/$TDIR"
echo "$ EXIT" > "/vms/$TDIR/testjob.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSUBMIT testjob.com\nSHOW QUEUE /ALL\nSHOW INTRUSION\nSHOW SYMBOL $STATUS\n' "$(echo $TDIR | tr a-z A-Z)" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "$VMSQ_DB_PATH"
