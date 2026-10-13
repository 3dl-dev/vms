#!/bin/bash
# TEST: SHOW LOGICAL/TABLE= a table the executive does not hold is %SYSTEM-F-NOLOGTAB (rd vms-94a; the
# message a real V7.3 gives for a DEFINE into a missing table, lab94a)
# EXPECT: contains:%SYSTEM-F-NOLOGTAB
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW LOGICAL/TABLE=OVMX$NO_SUCH_TABLE\n' | $VMSDCL 2>&1
