#!/bin/bash
# TEST: CONTINUE command exists and doesn't crash
# EXPECT_NOT: contains:unrecognized command
# EXPECT: regex:(CONTINUE|no interrupted)
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'CONTINUE' | $VMSDCL 2>&1
