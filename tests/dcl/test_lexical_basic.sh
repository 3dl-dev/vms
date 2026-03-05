#!/bin/bash
# TEST: F$LENGTH lexical function returns string length
# EXPECT: contains:5
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$LENGTH("HELLO")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
