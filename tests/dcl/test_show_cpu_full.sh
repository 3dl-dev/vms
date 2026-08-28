#!/bin/bash
# TEST: SHOW CPU/FULL reports per-CPU RUN state and the derivable PRIMARY/QUORUM/RUN capabilities
#
# LAYOUT IS DCL-DICTIONARY-PINNED (vms-SHOWFID). The public OpenVMS DCL
# Dictionary states /BRIEF and /FULL each "produce information from the summary
# display", so the "Multiprocessing is ..." line prints in /FULL too (it did
# not before this item) and the per-CPU "CPU nn is in RUN state" lines sit at
# COLUMN ZERO (VMS uses no 8-space indent). Both are anchored below.
# EXPECT: regex:^Multiprocessing is (ENABLED|DISABLED)\.$
# EXPECT: regex:^CPU 0*[0-9]+ is in RUN state
# EXPECT: contains:Capabilities of this CPU:
# EXPECT: contains:PRIMARY QUORUM RUN
# EXPECT_NOT: regex:^ +CPU 0
# EXPECT_NOT: contains:IVKEYW
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW CPU/FULL" | $VMSDCL 2>&1
