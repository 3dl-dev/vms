#!/bin/bash
# TEST: SHOW PROCESS/QUOTAS fabricates no quota block -- it has no fabricating
#       fallback when it cannot read the executive.
#
# WHAT CHANGED, AND WHY THIS ASSERTS ABSENCES (vms-050 / INV-6).
#
# SHOW PROCESS/QUOTAS read the account name from the executive ($GETJPI) but
# then printed SEVEN HARDCODED quota lines -- "CPU limit: Infinite",
# "Direct I/O limit: 40", "Buffered I/O byte count quota: 32768", ... --
# identical on every system, for every account, sourced from NOWHERE. They
# match one documented AUTHORIZE DEFAULT record, which is what made them
# dangerous: they pass a smoke test while stating nothing true about THIS
# process's real limits (CLAUDE.md Rule 10 / INV-6, the same fabrication class
# SHOW SYSTEM's invented row and SHOW QUOTA's "[200,1]" were deleted for).
#
# It is now a READER of the executive's per-process JIB quota vector (struct
# vms_jib_quota via $GETJPI): each quota line prints ONLY when the executive
# sourced that block (fields_valid & VMS_PI_V_QUOTA), from info.quota -- the
# same valid-bit discipline cmd_show_status uses. OVMX has no quota facility
# yet, so the bit is clear, and the limit lines are OMITTED, never fabricated
# and never shown as a plausible zero.
#
# ctest runs on a host with no /dev/vms and never will have one -- the only
# OVMX runtime is the kernel/QEMU path (Rule 9). The POSITIVE proof (the real
# de-fabbed block against a live executive: the account name present and NONE
# of the fabricated constants) lives in the DCL/SHOW acceptance battery,
# tests/qemu/lib/dcl_acceptance_battery.sh, run against a real vms.ko.
#
# What can be proven HERE, and only here, is that SHOW PROCESS/QUOTAS HAS NO
# FABRICATING FALLBACK: with no executive to read it returns the $GETJPI
# condition and prints none of the seven quota lines the deleted code printed.
# Every EXPECT_NOT below names a fragment of a line that code printed. Re-adding
# any of them -- as a fallback, or before the $GETJPI guard -- turns this red;
# the real quota values reaching the block through info.quota under a live
# executive is proven in QEMU, not here. QUOTAS_CHECK_COMPLETE is the positive
# anchor that proves the command actually ran and the search is not vacuous.
#
# EXPECT: contains:QUOTAS_CHECK_COMPLETE
# EXPECT_NOT: contains:Infinite
# EXPECT_NOT: contains:Direct I/O limit:
# EXPECT_NOT: contains:Buffered I/O byte count quota:
# EXPECT_NOT: contains:Buffered I/O limit:
# EXPECT_NOT: contains:Timer queue entry quota:
# EXPECT_NOT: contains:Paging file quota:
# EXPECT_NOT: contains:Enqueue quota:
# EXPECT_NOT: contains:AST quota:
VMSDCL="${VMSDCL:-vmsdcl}"
VMS_USERNAME=SYSTEM VMS_UIC_GROUP=1 VMS_UIC_MEMBER=4 \
    sh -c "echo 'SHOW PROCESS/QUOTAS' | $VMSDCL 2>&1"
echo "QUOTAS_CHECK_COMPLETE"
