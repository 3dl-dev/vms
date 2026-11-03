#!/bin/bash
# TEST: PIPE with multiple segments
# EXPECT: contains:FOUND
VMSDCL="${VMSDCL:-vmsdcl}"
# Test multi-segment: last segment output goes to stdout
# First segment writes to stdout (piped away), second writes FOUND to stdout
printf 'PIPE WRITE SYS$OUTPUT "HIDDEN" | WRITE SYS$OUTPUT "FOUND"\n' | $VMSDCL 2>&1
