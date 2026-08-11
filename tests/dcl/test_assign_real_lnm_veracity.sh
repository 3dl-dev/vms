#!/bin/bash
# TEST: vms-263 INV-DCL veracity gate - ASSIGN creates a REAL logical name
#       (LNM$PROCESS), not a DCL symbol, and DEASSIGN removes it
# EXPECT: contains:"FOO" = "BAR" (LNM$PROCESS_TABLE)
# EXPECT: contains:X = "BAR"
# EXPECT: contains:%DCL-W-NOLCL, no symbol "FOO" found
# EXPECT: contains:%DCL-W-NOLOG, no logical name match
#
# THE FINDING THIS GATES (docs/design-dcl-fidelity.md sec 1, TOP LIE #1;
# rd vms-263). cmd_assign() (src/vmsdcl/dcl_cmd_io.c) used to write its
# equivalence string into the DCL SYMBOL table via dcl_sym_set() for every
# invocation, including the /PROCESS default -- the command's own prior
# comment said so ("ASSIGN does not yet reach the logical-name manager at
# all"). ASSIGN FOO's equivalence was visible to SHOW SYMBOL but invisible
# to F$TRNLNM/SHOW LOGICAL -- the wrong subsystem entirely, dressed up in
# ASSIGN's real output shape (INV-DCL: implement the real semantics or
# refuse honestly, never fake the shape of one subsystem while writing to
# another).
#
# THIS IS THE TRIPWIRE, not a tautology. Revert cmd_assign()'s lnm_create()
# call back to the old dcl_sym_set(upper_name, equiv, DCL_SYM_GLOBAL) and
# this goes red exactly as follows, MEASURED against the pre-fix code:
#   - SHOW LOGICAL FOO (right after ASSIGN) prints
#     "%DCL-W-NOLOG, no logical name match" instead of
#     "FOO" = "BAR" (LNM$PROCESS_TABLE)" -- first EXPECT fails, and the
#     LAST EXPECT (which is supposed to appear only after DEASSIGN) is now
#     ALSO present after the very first SHOW LOGICAL, one line early --
#     nothing was ever created in the LNM manager.
#   - F$TRNLNM("FOO") returns "" (undefined logical), so SHOW SYMBOL X
#     prints "  X = """ instead of "  X = "BAR"" -- second EXPECT fails.
#   - SHOW SYMBOL FOO finds the equivalence as a GLOBAL DCL symbol and
#     prints "  FOO = "BAR"" instead of refusing with %DCL-W-NOLCL --
#     third EXPECT fails. This is the crux: the facade's write landed in
#     the symbol table, so THIS is where the old code's data actually
#     shows up.
# Measured PASSING against the fix in this same commit (cmd_assign() calls
# lnm_create(mgr, LNM_PROCESS_TABLE, ...) -- see cmd_define()'s identical
# LNM path in the same file for the pattern this mirrors).
#
# ORDER (clean-room, public OpenVMS DCL Dictionary, ASSIGN entry):
# "ASSIGN equivalence-name logical-name" -- equivalence-name FIRST,
# logical-name SECOND (the opposite order from DEFINE's
# "DEFINE logical-name equivalence-string"). ASSIGN BAR FOO therefore
# means equivalence="BAR", logical-name="FOO", and F$TRNLNM("FOO") must
# translate to "BAR".
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'ASSIGN BAR FOO\nSHOW LOGICAL FOO\nX = F$TRNLNM("FOO")\nSHOW SYMBOL X\nSHOW SYMBOL FOO\nDEASSIGN FOO\nSHOW LOGICAL FOO\n' | $VMSDCL 2>&1
