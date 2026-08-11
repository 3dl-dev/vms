#!/bin/bash
# TEST: vms-17d INV-DCL veracity gate - SET ACCOUNTING no longer toggles a
#       dead per-process bool; it flips a real, persisted, system-wide flag
#       SHOW ACCOUNTING reads back, and /ENABLE=(class) granularity draws
#       an honest refusal instead of silent acceptance
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \BOGUS\
# EXPECT: contains:$STATUS = 2288
# EXPECT: contains:%SET-W-NOTIMPL, per-class accounting (/ENABLE=(class,...) or /DISABLE=(class,...)) is not implemented in OVMX - no state changed
# EXPECT: contains:$STATUS = 2296
# EXPECT: contains:%SET-I-INTSET, accounting disabled
# EXPECT: contains:Accounting is currently disabled.
# EXPECT: contains:%SET-I-INTSET, accounting enabled
# EXPECT: contains:Accounting is currently enabled.
# EXPECT: contains:%SET-I-INTSET, accounting is enabled
# EXPECT_NOT: contains:accounting is %s
#
# THE FACADE THIS GATES (docs/design-dcl-fidelity.md sec 1;
# docs/dcl-verb-fidelity-scoreboard.md "SET ACCOUNTING - Still open").
# Before this fix, cmd_set_accounting() (src/vmsdcl/dcl_cmd_set.c) set
# ctx->accounting_enabled -- a PER-DCL-CONTEXT bool, freshly zeroed at the
# start of every vmsdcl process and invisible to every other process --
# and printed "%SET-I-INTSET, accounting enabled/disabled" as if it had
# taken effect, while the real writer, ovmx_accounting_record_login()
# (src/libvms/rtl/ovmx_accounting.c, called from login/SSH), recorded
# UNCONDITIONALLY. SET ACCOUNTING controlled NOTHING: a success-toned
# INV-DCL lie.
#
# THIS IS THE TRIPWIRE. Revert cmd_set_accounting()/cmd_show_accounting()
# to read/write ctx->accounting_enabled and this script still prints
# "accounting enabled"/"disabled" and "Accounting is currently ..." lines
# that LOOK identical -- proving text alone can't catch the facade, which
# is exactly why tests/libvms/test_accounting_veracity.c exists alongside
# this script: it proves ovmx_accounting_record_login() ITSELF now honours
# the flag (no login record written while disabled), the property text
# output can never demonstrate. What THIS script proves is the DCL SURFACE
# stopped silently accepting /ENABLE=(class) granularity nobody implements,
# and that SHOW ACCOUNTING agrees with SET ACCOUNTING within one session
# (a bare per-process bool would also pass that narrower check, which is
# why the class-granularity and IVQUAL assertions above matter as much as
# the enable/disable text does).
#
# Qualifiers grounded to the public OpenVMS DCL Dictionary SET ACCOUNTING
# entry (<https://wiki.vmssoftware.com/SET_ACCOUNTING>, fetched for this
# fix): /ENABLE[=(class[,...])] and /DISABLE[=(class[,...])], per-class
# resource tracking (IMAGE, LOGIN_FAILURE, MESSAGE, PRINT, PROCESS). OVMX
# has no per-class accounting -- only the single system-wide login record
# ovmx_accounting.c already writes -- so a class list on /ENABLE or
# /DISABLE is honestly refused (SS$_UNSUPPORTED / %SET-W-NOTIMPL) rather
# than silently accepted and ignored.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'SET ACCOUNTING/BOGUS\nSHOW SYMBOL $STATUS\nSET ACCOUNTING/ENABLE=(IMAGE)\nSHOW SYMBOL $STATUS\nSET ACCOUNTING/DISABLE\nSHOW ACCOUNTING\nSET ACCOUNTING/ENABLE\nSHOW ACCOUNTING\nSET ACCOUNTING\n' | $VMSDCL 2>&1
