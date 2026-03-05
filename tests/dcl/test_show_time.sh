#!/bin/bash
# TEST: SHOW TIME displays VMS date format (DD-MMM-YYYY HH:MM:SS)
# EXPECT: regex:[0-9]{1,2}-[A-Z]{3}-[0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2}
# EXPECT_NOT: contains:/
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW TIME" | $VMSDCL 2>&1
