#!/bin/bash
# TEST: F$TYPE returns symbol type
# EXPECT: contains:INTEGER
# EXPECT: contains:STRING
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'N = 42\nT = F$TYPE(N)\nSHOW SYMBOL T\n' | $VMSDCL 2>&1
printf 'S = "hello"\nT = F$TYPE(S)\nSHOW SYMBOL T\n' | $VMSDCL 2>&1
