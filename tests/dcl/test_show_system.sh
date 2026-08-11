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
# 9-23, and the CPU field at 44-59. vms-a7e made the executive source CPU
# time, PAGE FAULTS and RESIDENT PAGES from the Linux task it pins
# (fill_proc_acct, kernel/vms_proctab.c), so OVMX now sources Pid, Process
# Name, CPU, Page flts and Pages -- five columns kept at VMS's own widths and
# spelling. State, Pri and the I/O split remain unsourceable (no VMS
# scheduler, no VMS priority, no direct/buffered I/O split) and are removed
# WHOLE. The survivors close up: Pid 0-7, Name 9-23, CPU 25-40, Page flts
# 42-51, Pages 53-59 (docs/oracle/vax73-show-system-process.md §5.1's rule,
# now with more survivors).
#
# The regex below is anchored at both ends, so it fails if the heading gains a
# column, loses one, or drifts by a single space. That is the point: the
# previous "contains:Pid / contains:Process Name / contains:CPU" trio passed on
# any heading containing those three words in any arrangement, including the
# unmeasured one this item replaced.
# EXPECT: regex:^  Pid    Process Name          CPU         Page flts   Pages$
#
# The host has no /dev/vms, so SHOW SYSTEM prints its banner and heading and
# no rows at all. The heading is therefore the only thing this test can guard
# -- the row-level geometry is guarded in QEMU against a real executive, by
# tests/qemu/test_syssvc_showproc.c, and the accounting-on-a-redacted-row
# rule by tests/qemu/test_syssvc_procnam.c block P12. All three are needed;
# none covers the others.
#
# State, Pri and the I/O split stay ABSENT -- no heading word for any of them.
# ("Page flts" and "Pages" are now PRESENT, so they are no longer EXPECT_NOT.)
# EXPECT_NOT: contains:State
#
# THE `---` GUARD BELOW WAS INERT UNTIL vms-6a7 ROUND 2, AND THAT IS WORTH
# KNOWING BEFORE YOU TRUST ANY EXPECT_NOT IN THIS TREE. run_dcl_tests.sh ran
# `grep -qF "$needle"` with no `--`, so `---` was parsed as a grep OPTION:
# grep exited 2, the harness read that as "did not match", and the assertion
# passed unconditionally. A build printing "PLACEHOLDER      ---   ---   ---"
# from cmd_show_system() reported 90 passed, 0 failed. The harness now passes
# `--` and treats grep's exit >= 2 as a hard error, and tests/dcl/
# selftest_harness.sh (ctest: dcl-harness-selftest) drives that fix against
# synthetic cases so it cannot silently regress. Re-measured after the fix:
# the same placeholder build reddens this file.
# EXPECT_NOT: contains:---
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW SYSTEM" | $VMSDCL 2>&1
