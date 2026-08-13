#!/bin/bash
# TEST: DCL runs MMK's persistent-subprocess marker protocol (vms-b23)
#
# MMK drives builds by feeding a persistent DCL these exact commands over a
# mailbox (tests/corpus/tier3-mmk/build_target.c). Reproducing that stream here
# proves the five DCL fidelity behaviours MMK's protocol depends on, each of
# which was a real gap fixed under vms-b23:
#   1. verb-position symbol substitution: MMK___OPEN/MMK___SET/MMK___WRITE are
#      symbols whose value is a verb ("OPEN"/"SET"/"WRITE"); used as the first
#      token they run that verb (was %DCL-E-IVVERB).
#   2. OPEN of SYS$OUTPUT: connects to the process output stream, not an RMS
#      file (was %RMS-E-FNF).
#   3. an unquoted "!" comment is stripped from an assignment RHS (so
#      MMK___OPEN = "OPEN" !'F$VERIFY(0,0)' stores OPEN, not the comment).
#   4. WRITE ch "lit",sym evaluates the bare symbol arg and concatenates with no
#      separator, so the marker carries the STATUS value.
#   5. F$INTEGER($STATUS) evaluates its bare-symbol argument (an odd success
#      status, not 0).
#
# The independent oracle is the DCL-computed OVMXB23:42 (6*7) and the
# MMK____status=1 end-of-command marker the drive keys completion on.
#
# EXPECT: contains:OVMXB23:42
# EXPECT: contains:MMK____status=1
# EXPECT_NOT: contains:%DCL-E-IVVERB
# EXPECT_NOT: contains:%RMS-E-FNF
# EXPECT_NOT: contains:MMK____status=0
VMSDCL="${VMSDCL:-vmsdcl}"

$VMSDCL 2>&1 <<'DCLEOF'
MMK___OPEN = "OPEN" !'F$VERIFY(0,0)'
MMK___SET  = "SET" !'F$VERIFY(0,0)'
MMK___SET NOON
MMK___OPEN/WRITE MMK___OUTPUT SYS$OUTPUT:
ANSWER = 6 * 7
WRITE SYS$OUTPUT "OVMXB23:''ANSWER'"
MMK____status = F$INTEGER($STATUS) !'F$VERIFY(0,0)'
MMK___WRITE = "WRITE"
MMK___WRITE MMK___OUTPUT "MMK____status=",MMK____status
DCLEOF
