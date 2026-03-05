#!/bin/bash
# TEST: SET CONTROL=(Y) and SET NOCONTROL=(Y) accepted
# EXPECT_NOT: contains:unrecognized
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SET CONTROL=(Y)\nSET NOCONTROL=(Y)\nSHOW TIME\n' | $VMSDCL 2>&1
