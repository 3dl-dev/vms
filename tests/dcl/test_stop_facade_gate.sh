#!/bin/bash
# TEST: STOP facade kill (vms-1a8, docs/design-dcl-fidelity.md sec 5 Phase 2)
#       - the process-target forms (name parameter, /IDENTIFICATION=pid) no
#       longer claim success without ever looking at the target, and bare
#       STOP (no target) is unchanged.
#
# THE FACADE THIS GATES. Before this item, cmd_stop() (dcl_cmd_process.c)
# was `(void)cmd; ctx->exit_requested = 1; return SS$_NORMAL;` -- it read
# NEITHER the process-name parameter NOR /IDENTIFICATION, and unconditionally
# reported SS$_NORMAL (=1) while self-exiting the CURRENT session instead of
# ever looking up the named target. "STOP AUDIT_SERVER" and
# "STOP/IDENTIFICATION=99999999" both claimed success for a process that was
# never resolved.
#
# THE TRIPWIRE, AND ITS SCOPE. This host has no real /dev/vms (ctest never
# does -- see tests/qemu/CMakeLists.txt's header comment for why), so a
# target-form STOP cannot ACTUALLY delete anything here; the positive proof
# (a real named target process, really gone from the executive afterward) is
# tests/qemu/test_syssvc_delprc.c, which drives sys$delprc -- the same
# service cmd_stop now calls -- against a real, insmod'd vms.ko under QEMU.
# What THIS host-side test proves is the thing the OLD code got wrong
# unconditionally, with or without an executive present: it read $STATUS=1
# for a target form NO MATTER WHAT, before ever consulting cmd->params[0] or
# /IDENTIFICATION. The new code fails HONESTLY here (the executive-absent
# status every other executive-reading command in this suite already fails
# with -- see "SHOW PROCESS reports the executive's row, and fabricates
# nothing..." above in the same run) instead of fabricating success -- which
# is exactly INV-DCL's bar, and is provable without a kernel module.
#
# --- process-target forms: never $STATUS = 1 (the old unconditional lie),
#     and never a fabricated %STOP-S- success line ---
# EXPECT_NOT: regex:\$STATUS = 1[^0-9]
# EXPECT_NOT: contains:%STOP-S-
# --- malformed /IDENTIFICATION: honest parse-time refusal, not silently
#     accepted (the old code would have accepted ANY qualifier value,
#     including this one, and still returned $STATUS = 1) ---
# EXPECT: contains:%DCL-E-IVIDENT, invalid value - ZZZZ - for /IDENTIFICATION qualifier
# --- a STOP qualifier this file does not implement (/QUEUE belongs to the
#     SEPARATE STOP/QUEUE Dictionary entry, never implemented here) draws
#     the authentic IVQUAL rather than silent acceptance ---
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \QUEUE\
# --- bare STOP (no target): UNCHANGED -- still cleanly ends the session,
#     and nothing after it executes ---
# EXPECT: contains:before-bare-stop
# EXPECT_NOT: contains:after-bare-stop-should-not-run
VMSDCL="${VMSDCL:-vmsdcl}"

# Process-name target form.
printf 'STOP AUDIT_SERVER\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1

# /IDENTIFICATION=pid target form.
printf 'STOP/IDENTIFICATION=99999999\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1

# Malformed /IDENTIFICATION value: rejected before any executive call.
printf 'STOP/IDENTIFICATION=ZZZZ\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1

# Unimplemented qualifier: the authentic IVQUAL, not silent acceptance.
printf 'STOP/QUEUE=SYS$BATCH\n' | $VMSDCL 2>&1

# Bare STOP: still just ends the session (unchanged).
printf 'WRITE SYS$OUTPUT "before-bare-stop"\nSTOP\nWRITE SYS$OUTPUT "after-bare-stop-should-not-run"\n' | $VMSDCL 2>&1
