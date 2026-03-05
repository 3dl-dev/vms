#!/bin/bash
# TEST: SHOW SYSTEM displays VMS-style system information
# EXPECT: regex:(OpenVMS|OVMX|Runnable|process|PID)
# EXPECT_NOT: regex:(kworker|systemd|/usr/|/bin/)
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
