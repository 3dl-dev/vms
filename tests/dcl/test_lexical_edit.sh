#!/bin/bash
# TEST: F$EDIT performs string transformations
# EXPECT: contains:HELLO
# EXPECT: contains:hello
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$EDIT("hello","UPCASE")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
printf 'Y = F$EDIT("HELLO","LOWERCASE")\nSHOW SYMBOL Y\n' | $VMSDCL 2>&1
printf 'Z = F$EDIT("  trimme  ","TRIM")\nSHOW SYMBOL Z\n' | $VMSDCL 2>&1
