#!/bin/bash
# TEST: SHOW USERS displays VMS terminal device names and process info
# EXPECT: regex:_[A-Z]{2,3}[0-9]+:
# EXPECT: regex:[0-9A-F]{8}
# EXPECT: contains:Username
# EXPECT_NOT: contains:Node
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'SHOW USERS' | $VMSDCL 2>&1
