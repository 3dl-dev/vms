#!/bin/bash
# TEST: SHOW CPU/FULL reports per-CPU RUN state and the derivable PRIMARY/QUORUM/RUN capabilities
# EXPECT: regex:CPU 0*[0-9]+ is in RUN state
# EXPECT: contains:Capabilities of this CPU:
# EXPECT: contains:PRIMARY QUORUM RUN
# EXPECT_NOT: contains:IVKEYW
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW CPU/FULL" | $VMSDCL 2>&1
