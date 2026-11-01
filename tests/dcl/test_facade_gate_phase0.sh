#!/bin/bash
# TEST: Phase 0 gate (vms-6f4) - named DCL facades (ASSIGN/TABLE, SET AUDIT,
#       MOUNT of a nonexistent device) report an honest VMS error instead of
#       fake success
# EXPECT: contains:%SYSTEM-F-NOLOGTAB
# EXPECT: contains:%SET-W-NOTIMPL, security auditing is not implemented in OVMX
# EXPECT_NOT: contains:$STATUS == "%X00000001"
# EXPECT_NOT: contains:%MOUNT-I-MOUNTED
#
# THE FINDING THIS GATES (docs/design-dcl-fidelity.md sec 1, INV-DCL sec 3).
# ASSIGN/SYSTEM and SET AUDIT were named facades: each printed a plausible
# "-S-"/"-I-" success message while doing nothing that persisted or that any
# other process could observe --
#   - ASSIGN silently discarded /SYSTEM (and /JOB /GROUP /TABLE), writing
#     the equivalence into the DCL SYMBOL table regardless of which logical
#     name table the caller asked for, then returned SS$_NORMAL.
#   - SET AUDIT flipped a per-process bool (ctx->audit_enabled) that no
#     other process, reboot, or real audit trail could see, printed
#     "%SET-I-INTSET, auditing enabled", and returned SS$_NORMAL.
# INV-DCL: "a command that prints -S-/-I- while doing nothing is a worse
# tell than an honest error, because it appears to work."
#
# vms-263 (Phase 2) update: ASSIGN /SYSTEM, /JOB, /GROUP are no longer
# refused -- they now route to the real, executive-resident LNM$SYSTEM/
# GROUP/JOB tables the same way cmd_define() already did (lnm_create() ->
# vms_kif -> /dev/vms; src/vmsdcl/dcl_cmd_io.c cmd_assign()). The veracity
# test for the wired /PROCESS default path (and the fact that it is a REAL
# logical name, not a DCL symbol) is tests/dcl/test_assign_real_lnm_veracity.sh.
#
# rd vms-aae update: /TABLE=name is implemented too -- all three commands
# resolve the named table (dcl_lnm_table_of) and hand it to the logical-name
# manager, which reaches the executive table it names, the clusterwide tables
# included. So this canary's claim SHARPENS rather than disappears: the
# invocation below names a table that does NOT exist, and the honest answer
# is now %SYSTEM-F-NOLOGTAB -- the same refusal SHOW LOGICAL/TABLE= gives --
# with nothing written anywhere. The behaviour test for the tables that DO
# exist is tests/dcl/test_define_table_qualifier.sh.
#
# THIS IS THE TRIPWIRE. Make ASSIGN invent a table for a name nobody holds
# (or revert the cmd_set_audit() rewrite, src/vmsdcl/dcl_cmd_set.c) and this
# test goes red: the command prints its old "-S-"/"-I-" line and $STATUS goes
# back to 1, tripping the EXPECT_NOT.
#
# MOUNT is included as a REGRESSION control, not a fix made in this pass:
# re-derived during this audit, MOUNT already performs a real mount(2)
# through the executive (vms-651, cmd_mount() comment block) and already
# fails honestly here -- there is no /dev/vms in this ctest environment, so
# vms_kif_chkpriv() fails and cmd_mount() returns that failure verbatim
# (Rule 9: fail honestly when the executive is unreachable, never fake
# per-process success). The design doc's audit predates vms-651 and still
# lists MOUNT as a facade; this assertion is the re-derivation, and it would
# catch a regression back to the pre-vms-651 getcwd()-based facade, which
# printed %MOUNT-I-MOUNTED unconditionally regardless of the device.
#
# SCOPE: targeted refusals for exactly these two canary invocations, not the
# general fake-success sweep (SET ACCOUNTING/PASSWORD, PRODUCT HISTORY, SHOW
# LICENSE, ... are still open Phase 2 work, vms-6f4's design doc sec 5).
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'ASSIGN DUMMY_EQUIV DUMMY_LOGNAM /TABLE=BOGUS_TABLE\nSHOW SYMBOL $STATUS\nSET AUDIT /ENABLE\nSHOW SYMBOL $STATUS\nMOUNT VDA999: BOGUS\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
