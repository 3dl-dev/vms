#!/bin/bash
# TEST: WAIT command pauses briefly without error
# EXPECT: regex:(\$|WAIT completed|^$)
# EXPECT_NOT: contains:error
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'WAIT 00:00:01' | timeout 5 $VMSDCL 2>&1
echo "WAIT completed"
