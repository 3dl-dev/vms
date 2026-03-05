#!/bin/bash
# TEST: SET TERMINAL /OVERSTRIKE changes Insert to Overstrike in SHOW output
# EXPECT: contains:Overstrike
# EXPECT: contains:Width:
# EXPECT_NOT: contains:Insert
VMSDCL="${VMSDCL:-vmsdcl}"
printf "SET TERMINAL /OVERSTRIKE\nSHOW TERMINAL\n" | $VMSDCL 2>&1
