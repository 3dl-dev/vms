#!/bin/bash
# TEST: Qualifier parsing with /QUALIFIER syntax
# EXPECT: contains:Process Name
# EXPECT: contains:PID
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW PROCESS/ALL\n' | $VMSDCL 2>&1
