#!/bin/bash
# TEST: F$TRNLNM translates logical names
# EXPECT: contains:TESTVALUE
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DEFINE TEST$LOG "TESTVALUE"\nX = F$TRNLNM("TEST$LOG")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
