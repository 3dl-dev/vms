#!/bin/bash
# TEST: SPAWN does not invoke Unix shell
# EXPECT_NOT: contains:/bin/sh
# EXPECT_NOT: contains:/bin/bash
# EXPECT: contains:SPAWN_CHECK_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SPAWN SHOW TIME\nWRITE SYS$OUTPUT "SPAWN_CHECK_OK"\n' | $VMSDCL 2>&1
