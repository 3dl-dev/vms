#!/bin/bash
# TEST: F$STRING converts integer to string
# EXPECT: contains:42
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$STRING(42)\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
