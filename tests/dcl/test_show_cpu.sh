#!/bin/bash
# TEST: SHOW CPU reports the primary CPU and the active + configured sets from real system data
# EXPECT: contains:PRIMARY CPU =
# EXPECT: regex:Active CPUs: +[0-9]
# EXPECT: regex:Configured CPUs: +[0-9]
# EXPECT: regex:Multiprocessing is (ENABLED|DISABLED)
# EXPECT_NOT: contains:IVKEYW
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW CPU" | $VMSDCL 2>&1
