#!/bin/bash
# TEST: F$ELEMENT extracts delimited elements
# EXPECT: contains:beta
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$ELEMENT(1,",","alpha,beta,gamma")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
