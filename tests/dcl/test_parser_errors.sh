#!/bin/bash
# TEST: Error recovery for invalid command verb
# EXPECT: contains:IVVERB
# EXPECT: contains:XYZZY_BADCMD
# EXPECT: contains:RECOVERY_OK
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'XYZZY_BADCMD\nWRITE SYS$OUTPUT "RECOVERY_OK"\n' | $VMSDCL 2>&1
