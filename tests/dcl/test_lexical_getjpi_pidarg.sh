#!/bin/bash
# TEST: F$GETJPI HONORS its pid argument -- it must never answer about the caller
# EXPECT: contains:GETJPI_PIDARG_VERDICT=HONEST_DEGRADED
# EXPECT_NOT: contains:GETJPI_PIDARG_VERDICT=FACADE_CALLER_PID
#
# vms-9e2. The facade this catches: origin/main's F$GETJPI parses its pid
# argument and DISCARDS it, answering PID/USERNAME/PRCNAM out of the CALLER's
# own DCL context. So F$GETJPI("7FFFFFFF","PID") returned the CALLER's PID --
# a confident wrong answer about a different process (CLAUDE.md Rule 11).
#
# The verdict below discriminates the fix from the facade with NO knowledge
# of any pid value:
#   * FACADE_CALLER_PID -- F$GETJPI(<a pid that is not us>,"PID") came back
#     NON-EMPTY and EQUAL to our own PID: the argument was ignored and the
#     caller's PID was fabricated. This is origin/main, and it fails this test.
#   * HONEST_DEGRADED   -- the read reached the executive and honestly found
#     no such process, OR (on a host with no /dev/vms, where this suite runs)
#     the executive is unreachable: INV-6 says fail honestly, so the value is
#     EMPTY. Never the caller's PID. This is the fix.
#   * OTHER_PROC        -- pid 0x7FFFFFFF happened to resolve to a real, other
#     process: also a non-facade outcome (the argument was honored).
#
# The executive-sourced POSITIVE proof -- F$GETJPI(<another live process's
# pid>,"PRCNAM"/"USERNAME"/"PID") returning THAT process's real values, not
# the caller's -- runs against a real /dev/vms under QEMU
# (tests/qemu/test_syssvc_getjpi_pidarg.c); ctest has no executive (Rule 9),
# so here we prove only that the argument is no longer discarded into a
# caller-fabricated answer.
VMSDCL="${VMSDCL:-vmsdcl}"
$VMSDCL 2>&1 <<'DCLEOF'
SELF = F$GETJPI("","PID")
OTHER = F$GETJPI("7FFFFFFF","PID")
VERDICT = "FACADE_CALLER_PID"
IF OTHER .EQS. "" THEN VERDICT = "HONEST_DEGRADED"
IF OTHER .NES. "" .AND. OTHER .NES. SELF THEN VERDICT = "OTHER_PROC"
WRITE SYS$OUTPUT "GETJPI_PIDARG_VERDICT=''VERDICT'"
DCLEOF
