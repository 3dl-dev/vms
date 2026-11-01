#!/bin/bash
# TEST: TCPIP refuses a qualifier its sub-command does not read (vms-097).
# EXPECT: contains:%DCL-W-IVQUAL
# EXPECT: contains: \ZQONE\
# EXPECT: contains: \OUTPUT\
# EXPECT: contains: \ZQTWO\
# EXPECT: contains:%TCPIP-E-NOKEYW
# EXPECT_NOT: contains: \DEFAULT\
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'TCPIP SET ROUTE/ZQONE\nTCPIP SHOW VERSION/OUTPUT=X.LIS\nTCPIP SHOW INTERFACE/ZQTWO\nTCPIP SET ROUTE/DEFAULT\n' | $VMSDCL 2>&1
