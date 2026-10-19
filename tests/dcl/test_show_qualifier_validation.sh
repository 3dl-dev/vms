#!/bin/bash
# TEST: SHOW refuses a qualifier its sub-verb does not read, per sub-verb (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQONE\
# EXPECT: contains: \OUTPUT\
# EXPECT: contains: \ZQTWO\
# EXPECT: contains: \ZQFOUR\
# EXPECT_NOT: contains: \FULL\
# EXPECT_NOT: contains: \IDENTIFICATION\
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW DEVICE/ZQONE\nSHOW LOGICAL/OUTPUT=X.LIS NOSUCH\nSHOW PROCESS/ZQTWO\nSHOW DEFAULT/ZQFOUR\nSHOW LOGICAL/FULL NOSUCH\nSHOW PROCESS/ID=1\n' | $VMSDCL 2>&1
