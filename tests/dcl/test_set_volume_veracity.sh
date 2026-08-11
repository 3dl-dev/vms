#!/bin/bash
# TEST: vms-309 INV-DCL veracity gate - SET VOLUME no longer fakes success
#       for every invocation; it validates the device is a MOUNTED volume
#       (a real, cross-process /proc/mounts check, the same one MOUNT/
#       DISMOUNT use) and rejects a bogus qualifier structurally
#       (%DCL-W-IVQUAL), instead of the old unconditional SS$_NORMAL.
# EXPECT: contains:DCL-ALIVE
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \BOGUS\
# EXPECT: contains:$STATUS = 2288
# EXPECT: contains:%SET-E-DEVNOTMNT, device is not mounted - _DKA300:
# EXPECT: contains:$STATUS = 2688
# EXPECT_NOT: contains:%SET-I-NOTIMPL
# EXPECT_NOT: contains:SET VOLUME requires a mounted VMSFS volume
#
# THE FACADE THIS GATES (docs/dcl-verb-fidelity-scoreboard.md, "SET VOLUME
# - still open" before this fix). cmd_set_volume() (src/vmsdcl/dcl_cmd_set.c)
# used to print "%SET-I-NOTIMPL, SET VOLUME requires a mounted VMSFS volume"
# and return SS$_NORMAL UNCONDITIONALLY -- for a bogus qualifier, an
# unmounted device, ANY invocation at all. An -I- (success-toned) message
# for a total no-op is INV-DCL's banned class, worse than an honest error
# because it looks like it worked.
#
# THIS IS THE TRIPWIRE. Revert cmd_set_volume() to the old body and both
# assertions above fail: no %DCL-W-IVQUAL ever fires (the old handler took
# `(void)cmd` and never looked at qualifiers at all), and $STATUS after
# SET VOLUME DKA300: is 1 (SS$_NORMAL), not 2688 (SS$_DEVNOTMOUNT) --
# DKA300: is never mounted anywhere this suite runs (CLAUDE.md Rule 9: no
# /dev/vms on host ctest), so a real "is it mounted" check MUST fail here.
#
# WHY THIS IS HOST-TESTABLE WITHOUT /dev/vms (unlike tests/dcl/test_mount.sh's
# own reasoning, which this mirrors): mount_point_is_mounted() -- shared
# from dcl_cmd_misc.c via src/vmsdcl/include/dcl/dcl_cmd.h -- is a plain
# read of /proc/mounts, the kernel's own real mount table, with no
# executive dependency. On host ctest nothing is ever mounted there, so
# SET VOLUME's "not mounted" branch is exactly what every invocation here
# hits -- the authentic error the Dictionary requires ("device-name ...
# the name of one or more MOUNTED Files-11 volumes"), not the old
# blanket success. The paired POSITIVE -- a genuinely mounted volume, so
# the per-qualifier honest refusals below the mount check (including
# /LABEL) are reached -- is tests/qemu/test_mount_e2e.sh, which mounts a
# real DKA100: through vmsfs.ko and then issues SET VOLUME DKA100:/LABEL=…
# and SET VOLUME DKA100:/BOGUS against it.
#
# Qualifier grammar and /LABEL semantics grounded to the public OpenVMS
# DCL Dictionary SET VOLUME entry (<https://wiki.vmssoftware.com/SET_VOLUME>,
# <https://www.digiater.nl/openvms/doc/ia64-v8.3/opsys/vmsos83/9996/9996pro_225.html>,
# fetched for this fix) -- see src/vmsdcl/dcl_cmd_set.c's cmd_set_volume()
# header comment for the full citation and scope note (vmsfs has no
# volume-label write-back path for a live-mounted volume; /LABEL draws the
# same honest SS$_UNSUPPORTED refusal as every other unbacked qualifier,
# not a facade success).
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'WRITE SYS$OUTPUT "DCL-ALIVE"\nSET VOLUME DKA300:/BOGUS\nSHOW SYMBOL $STATUS\nSET VOLUME DKA300:/LABEL=NEWLABEL\nSHOW SYMBOL $STATUS\n' | $VMSDCL 2>&1
