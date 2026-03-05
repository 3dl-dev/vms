#!/bin/bash
# TEST: F$LOCATE finds substring position
# EXPECT: contains:6
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$LOCATE("World","Hello World")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
