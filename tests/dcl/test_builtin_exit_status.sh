#!/bin/bash
# TEST: $STATUS symbol reflects last command status
# EXPECT: regex:\$STATUS = 1
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'WRITE SYS$OUTPUT "ok"\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
