#!/bin/bash
# TEST: SET TERMINAL /OVERSTRIKE is accepted, and does not make SHOW TERMINAL
#       report a terminal the executive never gave it
# EXPECT: contains:$STATUS = 1
# EXPECT_NOT: contains:Overstrike
# EXPECT_NOT: contains:Insert editing
# EXPECT_NOT: contains:Terminal Characteristics:
#
# WHAT CHANGED AND WHY (vms-d0b). This file used to require SHOW TERMINAL to
# print "Overstrike" and not "Insert" after SET TERMINAL /OVERSTRIKE, under
# ctest, with no /dev/vms. Both halves were struct dcl_context's own copy of a
# terminal: this process wrote a bit and then read it back. See
# tests/dcl/test_terminal.sh for the full account of why that round trip
# proved nothing about a terminal.
#
# There is a second reason this particular assertion could not survive as it
# stood. "Overstrike" was never oracle-pinned: the V7.3 capture
# (docs/oracle/vax73-terminal-device.md section 2) shows the characteristic
# only in its SET form, "Insert editing", and its own note records that this
# pair "is a different word" without recording which word. OVMX's reader
# therefore has NO spelling for the cleared state and prints that cell blank
# rather than inventing one (CLAUDE.md rule 10) -- so requiring the string
# "Overstrike" would be requiring the fabrication.
#
# WHAT IS STILL ASSERTED HERE: SET TERMINAL /OVERSTRIKE is parsed and ACCEPTED
# ($STATUS = 1), and no terminal display appears that the executive did not
# supply. $STATUS is the positive anchor -- a DCL that rejected the qualifier,
# or never ran at all, would satisfy the EXPECT_NOTs too.
#
# The KNOWN GAP is the same one recorded in tests/dcl/test_set_terminal_width.sh:
# SET TERMINAL is a writer with no executive-visible effect until $ASSIGN/$QIO
# reach the executive's device table.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SET TERMINAL /OVERSTRIKE\nSHOW SYMBOL $STATUS\nSHOW TERMINAL\n' | $VMSDCL 2>&1
