#!/bin/bash
# TEST: F$VERIFY returns verification state
# EXPECT: contains:0
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SET NOVERIFY\nX = F$VERIFY()\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
