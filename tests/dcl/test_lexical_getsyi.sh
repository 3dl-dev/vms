#!/bin/bash
# TEST: F$GETSYI returns system information
# EXPECT: contains:V7.3
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$GETSYI("VERSION")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
