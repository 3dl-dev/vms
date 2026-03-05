#!/bin/bash
# TEST: SHOW MEMORY displays memory information in VMS format
# EXPECT: regex:(Memory|Pages|pagelets|bytes)
# EXPECT_NOT: contains:MemTotal
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW MEMORY" | $VMSDCL 2>&1
