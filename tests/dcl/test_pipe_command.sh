#!/bin/bash
# TEST: PIPE command chains DCL commands
# EXPECT: contains:PIPE_OUTPUT_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'PIPE WRITE SYS$OUTPUT "PIPE_OUTPUT_OK"\n' | $VMSDCL 2>&1
