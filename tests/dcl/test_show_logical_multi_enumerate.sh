#!/bin/bash
# TEST: SHOW LOGICAL/PROCESS (the enumerate-all path, no name given) lists
#       every equivalence string of a search-list logical too, not just the
#       first (vms-420 -- lnm_enumerate()/vms_kif_lnm_enumerate() used to
#       carry only equivalence index 0 out of the executive arena).
# EXPECT: contains:"FOO" = "BAR" (LNM$PROCESS_TABLE)
# EXPECT: contains:        = "BAZ"
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'DEFINE FOO BAR,BAZ\nSHOW LOGICAL/PROCESS\n' | $VMSDCL 2>&1
