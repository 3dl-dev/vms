#!/bin/bash
# TEST: Comment handling with ! and ; markers
# EXPECT: contains:VISIBLE_LINE
# EXPECT: contains:ALSO_VISIBLE
# EXPECT_NOT: contains:HIDDEN
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'WRITE SYS$OUTPUT "VISIBLE_LINE"\n! HIDDEN comment line\n; Another HIDDEN comment\nWRITE SYS$OUTPUT "ALSO_VISIBLE"\n' | $VMSDCL 2>&1
