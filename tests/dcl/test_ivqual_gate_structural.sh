#!/bin/bash
# TEST: Phase 1 keystone (vms-097) - %DCL-W-IVQUAL and %DCL-W-IVKEYW fire
#       STRUCTURALLY across the retrofit tranche, not just the SET TERMINAL
#       canary. Every verb below now carries a per-verb qualifier table
#       (struct dcl_verb.quals, src/vmsdcl/dcl_builtin.c); the parser
#       (dcl_validate_qualifiers(), src/vmsdcl/dcl_parser.c) rejects any
#       qualifier not in the verb's table BEFORE the handler runs.
#
# --- Unknown qualifiers on retrofit verbs -> %DCL-W-IVQUAL ($STATUS=2288) ---
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \BOGUS\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZZZ\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \FROBNICATE\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \NOTAREALQUAL\
# EXPECT: contains:$STATUS = 2288
# EXPECT_NOT: contains:$STATUS = 1
#
# --- Bad keyword on a keyword-typed qualifier -> %DCL-W-IVKEYW ($STATUS=2292) ---
# DIRECTORY/DATE is declared CDU_VT_KEYWORD; OVMX honours only the MODIFIED
# timestamp it can surface from stat(2), so any other keyword is rejected with
# the authentic IVKEYW rather than silently mapped to mtime (INV-DCL sec 3).
# EXPECT: contains:%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling - \CREATED\
# EXPECT: contains:%DCL-W-IVKEYW, unrecognized keyword - check validity and spelling - \BOGUSKW\
# EXPECT: contains:$STATUS = 2292
#
# --- Legitimate qualifiers on the SAME verbs still pass (not a blanket reject) ---
# EXPECT_NOT: regex:IVQUAL.*\\LOG\\
# EXPECT_NOT: regex:IVQUAL.*\\KEEP\\
# EXPECT_NOT: regex:IVQUAL.*\\REVERSE\\
# EXPECT_NOT: regex:IVKEYW.*\\MODIFIED\\
#
# THE FINDING THIS GATES (docs/design-dcl-fidelity.md sec 2-4). Before Phase 1,
# struct dcl_verb carried only the boolean CDU_F_QUALIFIER: parse_qualifier()
# accepted any /ANYTHING and %DCL-W-IVQUAL was structurally unreachable for all
# 54 verbs. Phase 0 (vms-6f4) fixed SET TERMINAL by hand as a canary. Phase 1
# makes it structural: revert the .quals wiring in builtin_verbs[] (or the
# dcl_validate_qualifiers() call in dcl_exec.c dispatch) and every IVQUAL/IVKEYW
# assertion below goes red, with $STATUS back to 1 on the bogus qualifiers.
#
# TRIPWIRE, not tautology: the EXPECT_NOT block proves the retrofit did not
# just blanket-reject qualifiers -- the real /LOG (COPY, PURGE), /KEEP=n
# (PURGE), /REVERSE (SORT) and /DATE=MODIFIED (DIRECTORY) qualifiers each verb
# actually implements are still accepted.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'TYPE/BOGUS NOSUCH.TXT\nSHOW SYMBOL $STATUS\n'          | $VMSDCL 2>&1
printf 'PURGE/ZZZ\nSHOW SYMBOL $STATUS\n'                      | $VMSDCL 2>&1
printf 'COPY/FROBNICATE A.TXT B.TXT\nSHOW SYMBOL $STATUS\n'    | $VMSDCL 2>&1
printf 'DELETE/NOTAREALQUAL X.TXT\nSHOW SYMBOL $STATUS\n'      | $VMSDCL 2>&1
printf 'DIRECTORY/DATE=CREATED\nSHOW SYMBOL $STATUS\n'         | $VMSDCL 2>&1
printf 'DIRECTORY/DATE=BOGUSKW\nSHOW SYMBOL $STATUS\n'         | $VMSDCL 2>&1
# Positive controls: real qualifiers must NOT trip IVQUAL/IVKEYW.
printf 'COPY/LOG NOSUCH_A.TXT NOSUCH_B.TXT\n'                  | $VMSDCL 2>&1
printf 'PURGE/KEEP=3\n'                                        | $VMSDCL 2>&1
printf 'SORT/REVERSE NOSUCH_IN.TXT NOSUCH_OUT.TXT\n'          | $VMSDCL 2>&1
printf 'DIRECTORY/DATE=MODIFIED\n'                             | $VMSDCL 2>&1
