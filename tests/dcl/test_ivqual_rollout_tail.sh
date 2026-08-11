#!/bin/bash
# TEST: Engine A rollout TAIL (vms-332) - %DCL-W-IVQUAL is now STRUCTURALLY
#       reachable for the remaining discrete, non-umbrella verbs that tranche 2
#       (vms-7543) left as legacy accept-all. Each verb below now carries a
#       per-verb qualifier table (struct dcl_verb.quals, src/vmsdcl/dcl_builtin.c);
#       dcl_validate_qualifiers() (src/vmsdcl/dcl_parser.c) rejects any qualifier
#       not in the table BEFORE the handler runs, with the authentic
#       %DCL-W-IVQUAL ($STATUS = 2288).
#
# --- Unknown qualifiers on the newly-tabled verbs -> %DCL-W-IVQUAL ---
# Distinct bogus tokens so each verb is proven individually. Populated tables:
# ANALYZE/LINK/PRODUCT/MOUNT/BACKUP. Empty tables (q_none): DISMOUNT/EDIT/
# REQUEST/ACCOUNTING/MONITOR/SYSGEN/SYSMAN. Validation short-circuits before the
# handler, so no MOUNT/DISMOUNT/exec side effect is reached on these lines.
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQANALYZE\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQLINK\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQPRODUCT\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQMOUNT\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQBACKUP\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQDISMOUNT\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQEDIT\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQREQUEST\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQACCOUNTING\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQMONITOR\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQSYSGEN\
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \ZQSYSMAN\
# EXPECT: contains:$STATUS = 2288
# EXPECT_NOT: contains:$STATUS = 1
#
# --- Real qualifiers on the SAME verbs still pass (not a blanket reject) ---
# The positive-control lines use each populated verb's real DCL Dictionary
# qualifier and must NOT trip IVQUAL. These do not print $STATUS (so the
# EXPECT_NOT above stays scoped to the bogus lines).
# EXPECT_NOT: regex:IVQUAL.*\\IMAGE\\
# EXPECT_NOT: regex:IVQUAL.*\\MAP\\
# EXPECT_NOT: regex:IVQUAL.*\\SOURCE\\
# EXPECT_NOT: regex:IVQUAL.*\\SYSTEM\\
# EXPECT_NOT: regex:IVQUAL.*\\SAVE_SET\\
#
# THE FINDING THIS GATES (docs/design-vms-parity-map.md sec 3, vms-8ad). This
# tranche finishes the discrete verbs: 45/54 now carry qualifier tables.
# TRIPWIRE: strip the .quals wiring for any verb below in builtin_verbs[] and
# its IVQUAL assertion goes red with $STATUS back to 1 (the pre-rollout
# accept-all state). NOT a tautology: the EXPECT_NOT block proves the real
# qualifiers still parse.
VMSDCL="${VMSDCL:-vmsdcl}"
# Bogus qualifiers -> IVQUAL (each prints $STATUS).
printf 'ANALYZE/ZQANALYZE NOSUCH.EXE\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
printf 'LINK/ZQLINK NOSUCH.OBJ\nSHOW SYMBOL $STATUS\n'       | $VMSDCL 2>&1
printf 'PRODUCT SHOW PRODUCT/ZQPRODUCT\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
printf 'MOUNT/ZQMOUNT DKA100:\nSHOW SYMBOL $STATUS\n'        | $VMSDCL 2>&1
printf 'BACKUP/ZQBACKUP A B\nSHOW SYMBOL $STATUS\n'          | $VMSDCL 2>&1
printf 'DISMOUNT/ZQDISMOUNT DKA100:\nSHOW SYMBOL $STATUS\n'  | $VMSDCL 2>&1
printf 'EDIT/ZQEDIT NOSUCH.TXT\nSHOW SYMBOL $STATUS\n'       | $VMSDCL 2>&1
printf 'REQUEST/ZQREQUEST "hello"\nSHOW SYMBOL $STATUS\n'    | $VMSDCL 2>&1
printf 'ACCOUNTING/ZQACCOUNTING\nSHOW SYMBOL $STATUS\n'      | $VMSDCL 2>&1
printf 'MONITOR/ZQMONITOR\nSHOW SYMBOL $STATUS\n'            | $VMSDCL 2>&1
printf 'SYSGEN/ZQSYSGEN\nSHOW SYMBOL $STATUS\n'              | $VMSDCL 2>&1
printf 'SYSMAN/ZQSYSMAN\nSHOW SYMBOL $STATUS\n'             | $VMSDCL 2>&1
# Positive controls: real qualifiers must NOT trip IVQUAL (no $STATUS printed).
printf 'ANALYZE/IMAGE NOSUCH.EXE\n'                          | $VMSDCL 2>&1
printf 'LINK/MAP NOSUCH.OBJ\n'                               | $VMSDCL 2>&1
printf 'PRODUCT INSTALL FOO/SOURCE=NOSUCH.PCSI\n'            | $VMSDCL 2>&1
printf 'MOUNT/SYSTEM DKA100:\n'                              | $VMSDCL 2>&1
printf 'BACKUP/SAVE_SET NOSUCH_A.TXT NOSUCH_B.BCK\n'         | $VMSDCL 2>&1
