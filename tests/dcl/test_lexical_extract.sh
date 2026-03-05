#!/bin/bash
# TEST: F$EXTRACT returns correct substrings
# EXPECT: contains:Hel
# EXPECT: contains:ll
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$EXTRACT(0,3,"Hello")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
printf 'Y = F$EXTRACT(2,2,"Hello")\nSHOW SYMBOL Y\n' | $VMSDCL 2>&1
