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
# What can be proven HERE, and only here, is that SHOW PROCESS HAS NO
# FABRICATING FALLBACK: with no executive row to read it reports the VMS
# condition it got and prints nothing else. Every EXPECT_NOT below names a line
# the deleted code printed unconditionally.
#
# WHAT THESE ABSENCES DO AND DO NOT CATCH -- MEASURED, not assumed. Simply
# re-adding a "Privileges:" line to the success path does NOT turn this test
# red, because on a host with no /dev/vms the command returns on the $GETJPI
# failure and never reaches it. That mutation is caught in QEMU
# (tests/qemu/test_syssvc_showproc.c, against a real executive). What this test
# catches is the mutation the QEMU suite CANNOT reach: adding a fallback that
# prints those fields when the executive is unreachable, which is the silent
# userspace fallback CLAUDE.md Rule 9 forbids and the exact defect class this
# item deletes. Verified by injecting one: printing Terminal:/Base priority:/
# Privileges: on the failure path turns this test red, and the unmutated run
# still passes.
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
VMS_USERNAME=SYSTEM VMS_PRIVILEGES=ALL VMS_UIC_GROUP=1 VMS_UIC_MEMBER=4 \
    sh -c "echo 'SHOW PROCESS' | $VMSDCL 2>&1"
echo "SHOW_PROCESS_CHECK_COMPLETE"
