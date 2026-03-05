#!/bin/bash
# TEST: Qualifier parsing with /QUALIFIER syntax
# EXPECT: regex:Process ID:
# EXPECT: contains:User
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW PROCESS/ALL\n' | $VMSDCL 2>&1
