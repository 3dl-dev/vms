#!/bin/bash
# TEST: Line continuation with dash at end of line
# EXPECT: contains:CONTINUED_OUTPUT
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WRITE -\nSYS$OUTPUT -\n"CONTINUED_OUTPUT"\n' | $VMSDCL 2>&1
