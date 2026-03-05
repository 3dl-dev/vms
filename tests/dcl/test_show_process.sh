#!/bin/bash
# TEST: SHOW PROCESS displays VMS-style process information
# EXPECT: regex:(Process|PID|User|Priority|State)
# EXPECT_NOT: contains:bash
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW PROCESS" | $VMSDCL 2>&1
