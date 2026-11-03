#!/bin/bash
# DCL_OUTPUT: stdio vms-380f an image/PIPE segment DCL runs does not yet write the parent's file SYS$OUTPUT
# TEST: PIPE command chains DCL commands
# EXPECT: contains:PIPE_OUTPUT_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'PIPE WRITE SYS$OUTPUT "PIPE_OUTPUT_OK"\n' | $VMSDCL 2>&1
