#!/bin/bash
# TEST: DUMP of a file that does not exist is %RMS-E-FNF, not an empty dump (vms-5303e control)
# EXPECT: contains:%RMS-E-FNF
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DUMP NO_SUCH_FILE_%s.DAT\n' "$$" | $VMSDCL 2>&1
