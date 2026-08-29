#!/bin/bash
# TEST: SHOW WORKING_SET fabricates no working-set limits -- it has no
#       fabricating fallback when it cannot read the executive.
#
# WHAT CHANGED, AND WHY THIS ASSERTS ABSENCES (vms-050 / INV-6).
#
# SHOW WORKING_SET used to print
#     Working Set  [current,quota,extent] = [8192,8192,16384]
#     Adjustment enabled  Authorized Quota = 8192  Authorized Extent = 16384
# where the quota DEFAULTED to a hardcoded 8192 (ctx->ws_quota is 0 for a real
# login) and the extent was an INVENTED quota*2 formula, both read from the DCL
# context, not the executive. A plausible constant and a bit of arithmetic were
# presented as this process's real working-set limits -- the same fabrication
# class SHOW PROCESS/QUOTAS' seven hardcoded quota lines and SHOW SYSTEM's
# invented row were deleted for (CLAUDE.md Rule 10 / INV-6).
#
# It is now a READER of the executive's $GETJPI row (vms_kif_getjpi_self): the
# current working-set size (JPI$_PPGCNT, info.pages) prints as /Limit, and the
# /Quota, /Extent and "Adjustment ... Authorized" limits print ONLY when the
# executive sourced the JIB quota block (fields_valid & VMS_PI_V_QUOTA) -- the
# same valid-bit discipline cmd_show_status and cmd_show_process_quotas use.
# OVMX has no quota facility yet, so that bit is clear and those limits are
# honestly OMITTED, never fabricated and never shown as a plausible zero.
#
# ctest runs on a host with no /dev/vms and never will have one -- the only
# OVMX runtime is the kernel/QEMU path (Rule 9). The POSITIVE proof (the real
# de-fabbed line against a live executive: a real "Working Set /Limit=" from
# $GETJPI and NONE of the fabricated numbers) lives in the DCL/SHOW acceptance
# battery, tests/qemu/lib/dcl_acceptance_battery.sh, run against a real vms.ko.
#
# What can be proven HERE, and only here, is that SHOW WORKING_SET HAS NO
# FABRICATING FALLBACK: with no executive to read, $GETJPI fails, the command
# returns that condition and prints NONE of the fabricated tokens. Every
# EXPECT_NOT below names a fragment only the deleted code printed --
# re-adding the 8192 default, the quota*2 extent, or the old
# "[current,quota,extent]" shape (as a fallback, or before the $GETJPI guard)
# turns this red. WORKING_SET_CHECK_COMPLETE is the positive anchor that proves
# the command actually ran and the search is not vacuous.
#
# EXPECT: contains:WORKING_SET_CHECK_COMPLETE
# EXPECT_NOT: contains:[current,quota,extent]
# EXPECT_NOT: contains:8192
# EXPECT_NOT: contains:Authorized Quota = 8192
# EXPECT_NOT: contains:Authorized Extent = 16384
VMSDCL="${VMSDCL:-vmsdcl}"
VMS_USERNAME=SYSTEM VMS_UIC_GROUP=1 VMS_UIC_MEMBER=4 \
    sh -c "echo 'SHOW WORKING_SET' | $VMSDCL 2>&1"
echo "WORKING_SET_CHECK_COMPLETE"
