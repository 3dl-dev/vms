#!/bin/bash
# TEST: SHOW DEFAULT displays current default directory
# EXPECT: regex:[A-Za-z0-9_]
# EXPECT_NOT: contains:/home/
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW DEFAULT" | $VMSDCL 2>&1
