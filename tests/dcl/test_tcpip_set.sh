#!/bin/bash
# TEST: TCPIP SET HOST adds entry visible in SHOW HOST
# EXPECT: contains:TESTHOST1
# EXPECT: contains:10.99.99.99
# EXPECT: contains:added
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'TCPIP SET HOST TESTHOST1 /ADDRESS=10.99.99.99\nTCPIP SHOW HOST\n' | $VMSDCL 2>&1
