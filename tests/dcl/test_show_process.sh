#!/bin/bash
# TEST: SHOW PROCESS fabricates no identity when it cannot read the executive
#
# WHAT THIS GATES, AND WHY IT ASSERTS AN ABSENCE (vms-2b8).
#
# SHOW PROCESS used to report an identity the PROCESS supplied: the user
# name came from getenv("VMS_USERNAME") and fell back to the literal
# "SYSTEM"; the UIC fell back to the caller's own gid/uid; and the
# privilege line was printed straight out of getenv("VMS_PRIVILEGES"),
# defaulting to a hard-coded "TMPMBX NETMBX". Any process could therefore
# tell SHOW PROCESS what to say about it. It is now a READER of the
# executive's process table (src/kernel/vms_proctab.c, via
# vms_kif_getjpi_self) -- CLAUDE.md Rule 11: a user-visible VMS command
# reads an executive facility, it does not fabricate its own answer.
#
# ctest runs on a host with no /dev/vms and never will have one: the only
# OVMX runtime is the kernel/QEMU path (Rule 9). So the positive proof --
# SHOW PROCESS reporting the identity the executive holds, and NOT the one
# planted in the environment -- lives in tests/qemu/test_syssvc_ident.c,
# against a real executive.
#
# What CAN be proven here, and only here, is the thing that used to be
# wrong: with no executive to read, SHOW PROCESS reports NOTHING. Each
# EXPECT_NOT below is tied to one deleted fabrication, so restoring any
# one of them turns exactly one line red:
#   "User:"  -> the identity header printed unconditionally
#   "SYSTEM" -> the user-name fallback
#   "TMPMBX" -> the hard-coded default privilege list
# The environment is deliberately POISONED with the three variables the
# deleted code read, so a reader that came back would be visible here and
# not merely silent.
#
# This is deliberately NOT an endorsement of running OVMX without an
# executive; it is the statement that when the reader has nothing to read,
# it invents nothing.
#
# EXPECT: contains:SHOW_PROCESS_CHECK_COMPLETE
# EXPECT_NOT: contains:User:
# EXPECT_NOT: contains:SYSTEM
# EXPECT_NOT: contains:TMPMBX
# EXPECT_NOT: contains:bash
VMSDCL="${VMSDCL:-vmsdcl}"
VMS_USERNAME=SYSTEM VMS_PRIVILEGES=ALL VMS_UIC_GROUP=1 VMS_UIC_MEMBER=4 \
    sh -c "echo 'SHOW PROCESS' | $VMSDCL 2>&1"
echo "SHOW_PROCESS_CHECK_COMPLETE"
