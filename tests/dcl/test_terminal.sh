#!/bin/bash
# TEST: SHOW TERMINAL displays VMS-style terminal characteristics
# EXPECT: contains:Terminal:
# EXPECT: contains:Device_Type:
# EXPECT: contains:Terminal Characteristics:
# EXPECT: contains:Width:
# EXPECT: contains:Page:
# EXPECT: contains:Insert
# EXPECT: contains:Echo
# EXPECT: contains:Wrap
# EXPECT: contains:Interactive
# EXPECT: contains:Line_Editing
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW TERMINAL" | $VMSDCL 2>&1
