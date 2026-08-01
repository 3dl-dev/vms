#!/bin/bash
# TEST: SHOW SYSTEM prints the oracle-pinned column heading and no others
# EXPECT: regex:(OpenVMS|Uptime)
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
# THE HEADING IS NOW PINNED BYTE FOR BYTE (vms-6a7), not merely checked for
# the presence of three words.
#
# vms-8019 round 4 deleted "---" placeholders from the State, Pri, I/O, Page
# flts and Pages columns -- CLAUDE.md Rule 10 allows two answers, reproduce
# what VMS prints or do not expose the thing, and a not-available marker is
# neither -- and left the column set for vms-6a7 to pin. vms-6a7 booted VAX1
# (OpenVMS VAX V7.3), captured SHOW SYSTEM through `cat -A` and counted the
# columns; the transcript and the full geometry table are in
# docs/oracle/vax73-show-system-process.md Sections 1.1 and 5.1. VMS prints:
#
#   "  Pid    Process Name    State  Pri      I/O       CPU       Page flts  Pages"
#
# with Pid at column 0 (no leading space), Process Name a 15-column field at
# 9-23, and the CPU field at 44-59. OVMX can source Pid, Process Name and CPU
# from the executive's row and none of the other five, so the sourceable
# columns keep VMS's own widths and spelling and the rest are removed WHOLE --
# heading text and field together -- which moves the centred "CPU" heading from
# column 51 to column 32.
#
# The regex below is anchored at both ends, so it fails if the heading gains a
# column, loses one, or drifts by a single space. That is the point: the
# previous "contains:Pid / contains:Process Name / contains:CPU" trio passed on
# any heading containing those three words in any arrangement, including the
# unmeasured one this item replaced.
# EXPECT: regex:^  Pid    Process Name           CPU$
#
# The host has no /dev/vms, so SHOW SYSTEM prints its banner and heading and
# no rows at all. The heading is therefore the only thing this test can guard
# -- the row-level geometry is guarded in QEMU against a real executive, by
# tests/qemu/test_syssvc_showproc.c, and the absent-CPU-for-a-redacted-row
# rule by tests/qemu/test_syssvc_procnam.c block P12. All three are needed;
# none covers the others.
# EXPECT_NOT: contains:State
# EXPECT_NOT: contains:Page flts
# EXPECT_NOT: contains:---
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
