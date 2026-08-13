#!/bin/bash
# TEST: Engine A rollout tranche 2 (vms-7543) - %DCL-W-IVQUAL is now
#       STRUCTURALLY reachable for the in-process verbs Phase 1 left as legacy
#       accept-all. Each verb below now carries a per-verb qualifier table
#       (struct dcl_verb.quals, src/vmsdcl/dcl_builtin.c); dcl_validate_qualifiers()
#       (src/vmsdcl/dcl_parser.c) rejects any qualifier not in the table BEFORE
#       the handler runs, with the authentic %DCL-W-IVQUAL ($STATUS = 2288).
#
# --- Unknown qualifiers on the newly-tabled verbs -> %DCL-W-IVQUAL ---
# Distinct bogus tokens so each verb is proven individually (populated tables:
# ASSIGN/DEASSIGN/DEFINE/OPEN/SPAWN/CONVERT/INQUIRE/RECALL; empty tables:
# WAIT/CLOSE).
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQASSIGN\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQDEASSIGN\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQDEFINE\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQOPEN\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQSPAWN\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQCONVERT\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQINQUIRE\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQRECALL\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQWAIT\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQCLOSE\
# EXPECT: contains:$STATUS = "%X000008F0"
# EXPECT_NOT: contains:$STATUS = "%X00000001"
#
# --- Real qualifiers on the SAME verbs still pass (not a blanket reject) ---
# The positive-control lines below use each verb's real qualifier and must NOT
# trip IVQUAL. ASSIGN/SYSTEM, OPEN/READ, RECALL/ALL, CONVERT/FDL are honoured;
# SPAWN/NOWAIT exercises the parser's NO-undo path (declared as literal
# "NOWAIT" in q_spawn) - it must resolve, not reject.
# EXPECT_NOT: regex:IVQUAL.*\\SYSTEM\\
# EXPECT_NOT: regex:IVQUAL.*\\READ\\
# EXPECT_NOT: regex:IVQUAL.*\\ALL\\
# EXPECT_NOT: regex:IVQUAL.*\\FDL\\
# EXPECT_NOT: regex:IVQUAL.*\\WAIT\\
#
# THE FINDING THIS GATES (docs/design-vms-parity-map.md sec 3). Phase 1
# (vms-097) wired 15/54 verbs; the other 39 stayed accept-all, so IVQUAL was
# structurally unreachable for them. This tranche (vms-7543) adds tables to the
# in-process, self-parsing verbs. TRIPWIRE: strip the .quals wiring for any
# verb below in builtin_verbs[] and its IVQUAL assertion goes red with $STATUS
# back to 1. NOT a tautology: the EXPECT_NOT block proves real qualifiers still
# pass.
VMSDCL="${VMSDCL:-vmsdcl}"
# Bogus qualifiers -> IVQUAL (each prints $STATUS).
printf 'ASSIGN/ZQASSIGN A B\nSHOW SYMBOL $STATUS\n'    | $VMSDCL 2>&1
printf 'DEASSIGN/ZQDEASSIGN A\nSHOW SYMBOL $STATUS\n'  | $VMSDCL 2>&1
printf 'DEFINE/ZQDEFINE A B\nSHOW SYMBOL $STATUS\n'    | $VMSDCL 2>&1
printf 'OPEN/ZQOPEN X.TXT\nSHOW SYMBOL $STATUS\n'      | $VMSDCL 2>&1
printf 'SPAWN/ZQSPAWN\nSHOW SYMBOL $STATUS\n'          | $VMSDCL 2>&1
printf 'CONVERT/ZQCONVERT A B\nSHOW SYMBOL $STATUS\n'  | $VMSDCL 2>&1
printf 'INQUIRE/ZQINQUIRE SYM\nSHOW SYMBOL $STATUS\n'  | $VMSDCL 2>&1
printf 'RECALL/ZQRECALL\nSHOW SYMBOL $STATUS\n'        | $VMSDCL 2>&1
printf 'WAIT/ZQWAIT\nSHOW SYMBOL $STATUS\n'            | $VMSDCL 2>&1
printf 'CLOSE/ZQCLOSE\nSHOW SYMBOL $STATUS\n'          | $VMSDCL 2>&1
# Positive controls: real qualifiers must NOT trip IVQUAL (no $STATUS printed).
printf 'ASSIGN/SYSTEM AEQV ALOGNAME\n'                 | $VMSDCL 2>&1
printf 'OPEN/READ NOSUCH_OPEN.TXT\n'                   | $VMSDCL 2>&1
printf 'RECALL/ALL\n'                                  | $VMSDCL 2>&1
printf 'CONVERT/FDL=X NOSUCH_A.DAT NOSUCH_B.DAT\n'     | $VMSDCL 2>&1
printf 'SPAWN/NOWAIT SHOW TIME\n'                      | $VMSDCL 2>&1
