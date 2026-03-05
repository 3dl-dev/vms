#!/bin/bash
# TEST: F$GETJPI returns process information
# EXPECT: regex:[A-Z]+
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$GETJPI("","USERNAME")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
