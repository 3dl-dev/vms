#!/bin/bash
# TEST: SHOW PROCESS reports the executive's row, and fabricates nothing when
#       it cannot read one
#
# WHAT CHANGED, AND WHY THIS TEST NOW ASSERTS ABSENCES (vms-6a7).
#
# SHOW PROCESS used to describe the calling process out of values that process
# had written about ITSELF -- the DCL context's process_name, getpid(),
# getgid()/getuid(), a privilege list from the VMS_PRIVILEGES environment
# variable, and seven hardcoded lines of quota numbers identical on every
# system. It accepted no target at all: "SHOW PROCESS AUDIT_SERVER" printed the
# caller. The old assertion here (regex "(Process|PID|User|Priority|State)")
# passed on exactly that fabrication, and would have passed no matter which
# process the user asked about.
#
# It is now a reader of the executive process table through $GETJPI, which
# resolves a target BY NAME and BY PID (CLAUDE.md Rule 11: a VMS command reads
# an executive facility, it does not fabricate its own answer).
#
# ctest runs on a host with no /dev/vms and never will have one -- the only
# OVMX runtime is the kernel/QEMU path (Rule 9). So the POSITIVE proof (the
# oracle-pinned layout, a target other than the caller, the by-name and by-pid
# selectors, and the two different refusals) lives in
# tests/qemu/test_syssvc_showproc.c, against a real executive.
#
# What can be proven HERE, and only here, is the thing that used to be wrong:
# with no executive row to read, SHOW PROCESS prints NONE of the fabricated
# fields. Every EXPECT_NOT below names a line the deleted code printed
# unconditionally, so restoring any of them turns this test red. That makes
# this an assertion about dcl_cmd_show.c's own source, not a decoration.
#
# EXPECT: regex:%SYSTEM-[A-Z]-
# EXPECT_NOT: contains:Terminal:
# EXPECT_NOT: contains:Base priority:
# EXPECT_NOT: contains:Privileges:
# EXPECT_NOT: contains:Process quotas:
# EXPECT_NOT: contains:_FTA0:
# EXPECT_NOT: contains:User Identifier:
# EXPECT_NOT: contains:Default file spec:
# EXPECT_NOT: contains:bash
VMSDCL="${VMSDCL:-vmsdcl}"
echo "SHOW PROCESS" | $VMSDCL 2>&1
