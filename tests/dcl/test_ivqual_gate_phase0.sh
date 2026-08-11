#!/bin/bash
# TEST: Phase 0 gate (vms-6f4) - SET TERMINAL rejects an unknown qualifier
#       with the authentic VMS error instead of silently succeeding
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \FDAFS\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \BOGUS\
# EXPECT: contains:$STATUS = 2288
# EXPECT_NOT: contains:$STATUS = 1
#
# THE FINDING THIS GATES (docs/design-dcl-fidelity.md sec 2, INV-DCL sec 3).
# struct dcl_verb (src/vmsdcl/include/dcl/cdu.h) carries only a boolean
# CDU_F_QUALIFIER flag -- no per-verb declaration of legal qualifiers exists
# anywhere. parse_qualifier() (dcl_parser.c) upcases and stores whatever
# token followed '/' and never consults the verb, so it always accepts.
# MEASURED before this fix: SET TERMINAL/FDAFS returned SS$_NORMAL, no error,
# no effect -- %DCL-W-IVQUAL was structurally unreachable for all 54 builtin
# verbs, not just this one.
#
# THIS IS THE TRIPWIRE, not a tautology (design doc: "a metric that cannot
# fail is not a gate"). Revert the terminal_known_qualifiers[] validation
# loop added to cmd_set_terminal() (src/vmsdcl/dcl_cmd_set.c) and this test
# goes red: both invocations below would go back to $STATUS = 1 with no
# %DCL-W-IVQUAL anywhere in the output, exactly as measured pre-fix.
#
# SCOPE: this is a TARGETED fix for SET TERMINAL only, against the exact
# qualifier list already implemented in that one handler -- not the general
# per-verb qualifier-table retrofit (Phase 1, vms-097), which is what makes
# IVQUAL reachable everywhere. Legitimate qualifiers stay accepted --
# tests/dcl/test_set_terminal.sh (/OVERSTRIKE) and
# tests/dcl/test_set_terminal_width.sh (/WIDTH=132) cover that positive case
# and are not touched by this change.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SET TERMINAL/FDAFS\nSHOW SYMBOL $STATUS\nSET TERMINAL/BOGUS\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
