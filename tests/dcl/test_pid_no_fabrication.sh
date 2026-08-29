#!/bin/bash
# TEST: F$PID fabricates no process ID when it cannot read the executive
#
# WHAT THIS GATES, AND WHY IT ASSERTS AN ABSENCE (vms-050).
#
# F$PID used to answer from the Linux substrate, never the VMS executive
# (src/vmsdcl/dcl_lexical.c, populate_pid_list()):
#   - it opendir("/proc")'d and took EVERY numeric entry as a "PID", then
#     printed it %08X -- so F$PID returned the Linux kernel's task pids dressed
#     as VMS process IDs;
#   - on an opendir() failure it fell back to getpid() -- the DCL process's own
#     Linux pid, fabricated as the one "VMS process".
# A VMS pid is EXECUTIVE-allocated (the process-table row's vms_pid, distinct
# per process since #883/vms-d4ef), not the substrate task pid. F$PID is the DCL
# Dictionary's window onto the VMS process list -- so answering it from /proc is
# the exact defect CLAUDE.md Rule 9/11 / INV-6 exist to kill.
#
# F$PID is now a READER of the executive process table: it walks
# vms_kif_procscan() -- the SAME source SHOW SYSTEM and SHOW USERS read -- and
# returns each row's vms_pid in cursor order, "" when the list is exhausted.
# When the executive cannot be reached -- as under ctest, which runs on a host
# with no /dev/vms and never will have one (the only OVMX runtime is the
# kernel/QEMU path, Rule 9) -- the first procscan fails, the list is empty, and
# F$PID returns "" WITHOUT inventing anything: no /proc read, no getpid().
#
# WHAT CAN BE PROVEN HERE, AND ONLY HERE, is the thing that used to be wrong:
# with no executive to read, F$PID returns "" and NONE of the fabricated pids
# (a /proc entry, or getpid()) appear. Restore EITHER deleted fabrication branch
# -- opendir("/proc") or the getpid() fallback -- and a nonzero 8-hex pid
# reappears in A/B and the `A = "<hex>"` / `B = "<hex>"` guards trip: the gate
# goes RED. (Verified by that mutation while writing this test.)
#
# This is deliberately NOT an endorsement of running OVMX without an executive;
# it is the statement that when the reader has nothing to read, it invents
# nothing. The POSITIVE proof -- F$PID's pids MATCHING SHOW SYSTEM's VMS pid set
# (the SYSTEM login pid among them), read from the same executive -- runs
# against a real /dev/vms in the DCL/SHOW acceptance battery
# (tests/qemu/lib/dcl_acceptance_battery.sh, the F$PID block), which a
# userspace-only ctest cannot stand in for (CLAUDE.md Rule 9).
#
# EXPECT: contains:A = ""
# EXPECT: contains:B = ""
# EXPECT_NOT: regex:A = "[0-9A-Fa-f]
# EXPECT_NOT: regex:B = "[0-9A-Fa-f]
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'CTX = ""\nA = F$PID("CTX")\nSHOW SYMBOL A\nB = F$PID("CTX")\nSHOW SYMBOL B\n' | $VMSDCL 2>&1
