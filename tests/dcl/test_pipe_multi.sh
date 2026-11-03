#!/bin/bash
# DCL_OUTPUT: stdio vms-380f an image/PIPE segment DCL runs does not yet write the parent's file SYS$OUTPUT
# TEST: PIPE with multiple segments
# EXPECT: contains:FOUND
VMSDCL="${VMSDCL:-vmsdcl}"
# Test multi-segment: last segment output goes to stdout
# First segment writes to stdout (piped away), second writes FOUND to stdout
printf 'PIPE WRITE SYS$OUTPUT "HIDDEN" | WRITE SYS$OUTPUT "FOUND"\n' | $VMSDCL 2>&1
