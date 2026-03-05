#!/bin/bash
# TEST: SHOW SYSTEM displays VMS-style system information without Linux processes
# EXPECT: regex:(OpenVMS|Uptime)
# EXPECT: contains:Process Name
# EXPECT: contains:State
# EXPECT_NOT: contains:kworker
# EXPECT_NOT: contains:systemd
# EXPECT_NOT: contains:sshd
# EXPECT_NOT: contains:bash
# EXPECT_NOT: contains:KWORKER
# EXPECT_NOT: contains:SYSTEMD
# EXPECT_NOT: contains:SSHD
# EXPECT_NOT: contains:BASH
# EXPECT_NOT: regex:(kworker|systemd|/usr/|/bin/)
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
