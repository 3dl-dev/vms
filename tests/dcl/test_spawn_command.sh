#!/bin/bash
# TEST: SPAWN executes DCL command in subprocess
# EXPECT: contains:SPAWN_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SPAWN WRITE SYS$OUTPUT "SPAWN_OK"\n' | $VMSDCL 2>&1
