#!/bin/bash
# TEST: SHOW SYSTEM displays VMS-style system information without Linux processes
# EXPECT: regex:(OpenVMS|Uptime)
# EXPECT: contains:Pid
# EXPECT: contains:Process Name
# EXPECT: contains:CPU
# EXPECT_NOT: contains:kworker
# EXPECT_NOT: contains:systemd
# EXPECT_NOT: contains:sshd
# EXPECT_NOT: contains:bash
# EXPECT_NOT: contains:KWORKER
# EXPECT_NOT: contains:SYSTEMD
# EXPECT_NOT: contains:SSHD
# EXPECT_NOT: contains:BASH
# EXPECT_NOT: regex:(kworker|systemd|/usr/|/bin/)
#
# THE "State" EXPECTATION WAS REMOVED, AND THE "---" ONE ADDED, ON PURPOSE
# (vms-8019 round 4, operator ruling). SHOW SYSTEM used to print State, Pri,
# I/O, Page flts and Pages headings with "---" in every one of them, because
# OVMX cannot source any of those five from the executive's process table.
# CLAUDE.md Rule 10 allows two answers -- reproduce what VMS prints, or do
# not expose the thing -- and a not-available marker is neither: it is a
# display for a condition VMS never faces, in a user-visible VMS command.
# So the five columns are ABSENT until vms-6a7 pins the real column set to
# the oracle. This test now guards that they stay absent.
#
# The host has no /dev/vms, so SHOW SYSTEM prints its banner and heading and
# no rows at all. The heading is therefore the only thing this test can guard
# -- the row-level marker is guarded in QEMU against a real executive, by
# tests/qemu/test_syssvc_procnam.c block P12 ("the UNREADABLE row fabricates
# NO CPU figure at all"), which fails if anything but padding follows the
# process name. Both are needed; neither covers the other.
# EXPECT_NOT: contains:State
# EXPECT_NOT: contains:Page flts
# EXPECT_NOT: contains:---
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
