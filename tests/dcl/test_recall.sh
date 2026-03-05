#!/bin/bash
# TEST: RECALL/ALL displays command history
# EXPECT: regex:(SHOW TIME|RECALL|No commands)
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW TIME\nRECALL/ALL\n' | $VMSDCL 2>&1
