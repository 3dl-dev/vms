#!/bin/bash
# TEST: F$INTEGER converts string to integer
# EXPECT: contains:42
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$INTEGER("42")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
