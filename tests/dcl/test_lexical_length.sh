#!/bin/bash
# TEST: F$LENGTH returns string length
# EXPECT: contains:5
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$LENGTH("Hello")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
