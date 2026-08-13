#!/bin/bash
# TEST: vms-e9e INV-DCL veracity gate - SET PASSWORD no longer fakes success
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \BOGUS\
# EXPECT: contains:$STATUS = "%X000008F0"
# EXPECT: contains:%DCL-W-NOTIMPL, secondary passwords are not implemented in OVMX - no state changed
# EXPECT: contains:%DCL-W-NOTIMPL, /GENERATE password generation is not implemented in OVMX - no state changed
# EXPECT: contains:$STATUS = "%X000008F8"
# EXPECT: contains:%DCL-E-MAXPARM, too many parameters
# EXPECT: contains:$STATUS = "%X00000014"
# EXPECT: contains:%UAF-E-NOSUCHUSER, no such user
# EXPECT_NOT: contains:not fully implemented
# EXPECT_NOT: contains:%SET-I-PASSWORD
# EXPECT_NOT: contains:Full SYSUAF.DAT rewrite is planned
#
# THE FACADE THIS GATES (docs/design-dcl-fidelity.md sec 1;
# docs/dcl-verb-fidelity-scoreboard.md "SET PASSWORD - Still open"). Before
# this fix, cmd_set_password() (src/vmsdcl/dcl_cmd_set.c) ignored every
# qualifier and parameter, unconditionally printed
#   "%SET-I-PASSWORD, password change not fully implemented"
#   "Full SYSUAF.DAT rewrite is planned for a future release."
# and returned SS$_NORMAL ($STATUS = 1) without touching SYSUAF at all --
# an -I- (informational-success-toned) lie for a total no-op, the exact
# class INV-DCL bans: "a command that prints -S-/-I- while doing nothing
# is a worse tell than an honest error, because it appears to work."
#
# THIS IS THE TRIPWIRE. Revert cmd_set_password() to the old body and this
# test goes red: every invocation below would print the two facade lines
# above and "SHOW SYMBOL $STATUS" would report 1 (SS$_NORMAL) after EVERY
# one of them -- a bogus qualifier, an extra parameter, /SECONDARY,
# /GENERATE would all "succeed" identically. The EXPECT_NOT lines catch the
# revert directly; the specific $STATUS values below catch a shallower
# revert that keeps some output text but goes back to faking success.
#
# WHAT THIS DOES NOT COVER: the positive path (old password verified, new
# hash actually persisted to SYSUAF, login flips from old to new password)
# needs a real identity (ctx->username), which on bare host ctest -- no
# /dev/vms -- stays empty by design (Rule 9/INV-6: no per-process identity
# fake; src/vmsdcl/dcl_main.c's comment on vms_kif_getjpi_self()). That
# mechanism -- sysuaf_lookup -> sysuaf_authenticate -> sysuaf_write_record,
# the exact calls cmd_set_password() makes -- is proven end-to-end by
# tests/libvms/test_sysuaf_write_veracity.c, which does not need an
# identity because it drives the SYSUAF layer directly. This script proves
# the DCL SURFACE stopped lying; that one proves the WRITER underneath it
# is real.
#
# %UAF-E-NOSUCHUSER here is the CORRECT, honest behaviour for a host ctest
# run with no established identity, not an oversight: with ctx->username
# empty, cmd_set_password() honestly refuses rather than looking up "" and
# silently proceeding. It is asserted here as a regression guard on ITS
# OWN honesty -- it must never become $STATUS = 1.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SET PASSWORD/BOGUS\nSHOW SYMBOL $STATUS\nSET PASSWORD/SECONDARY\nSET PASSWORD/GENERATE\nSHOW SYMBOL $STATUS\nSET PASSWORD FOO\nSHOW SYMBOL $STATUS\nSET PASSWORD\n' | $VMSDCL 2>&1
