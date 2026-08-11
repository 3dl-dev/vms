#!/bin/bash
# TEST: SET quick-win subcommands produce correct VMS-format output
# EXPECT: contains:%SET-I-NOTAVAIL, DECnet is not available on this system
# EXPECT: contains:%SET-W-NOTIMPL, security auditing is not implemented in OVMX
# EXPECT: contains:security auditing is currently disabled.
# EXPECT: contains:%SET-I-INTSET, accounting enabled
# EXPECT: contains:Accounting is currently enabled.
# EXPECT: contains:%SET-I-INTSET, accounting disabled
# EXPECT: contains:%DCL-E-NODEVICE, no device specified
#
# WHAT CHANGED AND WHY (vms-6f4 Phase 0, docs/design-dcl-fidelity.md sec 5).
# SET AUDIT used to be a named facade: it flipped a per-process bool
# (ctx->audit_enabled) and printed "%SET-I-INTSET, auditing enabled/disabled"
# while returning SS$_NORMAL -- fake success (INV-DCL), since OVMX has no
# security-auditing subsystem behind it. It now refuses honestly
# (SS$_UNSUPPORTED, %SET-W-NOTIMPL) and touches no state, so SHOW AUDIT
# afterward reports the untouched default ("currently disabled") instead of
# the lie SET AUDIT /ENABLE used to write into it. Both /ENABLE and /DISABLE
# now print the same honest refusal, so this file only has one AUDIT
# EXPECT line rather than two. The negative gate for this fix is
# tests/dcl/test_facade_gate_phase0.sh. SET ACCOUNTING/SET HOST are
# unchanged here -- Phase 0 is scoped to the named canaries only; the rest
# of this facade class is Phase 2's job.
#
# SET VOLUME's bare-no-device case (the last line of the transcript) moved
# off the OLD facade text ("%SET-I-NOTIMPL ... requires a mounted VMSFS
# volume", an unconditional SS$_NORMAL success for every invocation) to a
# real "%DCL-E-NODEVICE, no device specified" / SS$_BADPARAM refusal —
# vms-309, docs/dcl-verb-fidelity-scoreboard.md's "SET VOLUME" section.
# The mounted-device / per-qualifier honest-refusal paths this fix also
# added are exercised by tests/dcl/test_set_volume_veracity.sh (device not
# mounted, bogus qualifier IVQUAL) and tests/qemu/test_mount_e2e.sh (a
# genuinely mounted volume, /LABEL and an unknown qualifier).
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'SET HOST\nSET AUDIT /ENABLE\nSHOW AUDIT\nSET AUDIT /DISABLE\nSET ACCOUNTING /ENABLE\nSHOW ACCOUNTING\nSET ACCOUNTING /DISABLE\nSET VOLUME\n' | $VMSDCL 2>&1
