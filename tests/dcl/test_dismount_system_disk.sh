#!/bin/bash
# TEST: DISMOUNT must refuse to dismount OVMX's own system device, and the device
#       must remain fully usable afterward
# EXPECT: contains:%DISM-F-SYSDEV, The system device cannot be dismounted
# EXPECT: contains:DKA0:
# EXPECT: contains:Mounted
# EXPECT: contains:OVMXSYS
# EXPECT: contains:B9FCHKDIR.DIR
# EXPECT_NOT: contains:%DISMOUNT-I-DISMOUNTED
# EXPECT_NOT: contains:Dismounted
#
# Root cause (vms-b9f R1 -- a REGRESSION introduced by the R1-preceding rework, not a
# pre-existing bug): registering the boot system disk (DKA0:) into vms_device_table
# (dcl_main.c setup_session()) so SHOW DEVICE could see it also made it, for the first
# time, a target DISMOUNT would accept -- `DISMOUNT DKA0:` started printing
# "%DISMOUNT-I-DISMOUNTED, _DKA0: dismounted" and SUCCEEDING. Worse, after that
# "successful" dismount, `DIRECTORY DKA0:[000000]` still listed every file: vmsfs's OWN
# device table (the one vmsfs_to_linux_path() actually reads to resolve DKA0: to a Linux
# path) is a SEPARATE structure from vms_device_table, and DISMOUNT never touched it --
# so SHOW DEVICE would report a dismounted state that was not true. Real OpenVMS refuses
# to dismount the system device outright. No existing test exercised DISMOUNT of the
# system disk, so the suite could not see this regression when it was introduced.
#
# Fix (dcl_cmd_misc.c cmd_dismount()): reject DISMOUNT of the system device (DKA0: or its
# SYS$SYSDEVICE logical) before any table lookup. Message pinned live against the oracle
# (OpenVMS VAX 7.3, ~/vax/cluster/vax1, 2026-07-29): DISMOUNT SYS$SYSDEVICE: on that
# system produced the byte-identical "%DISM-F-SYSDEV, The system device cannot be
# dismounted" -- facility DISM (not DISMOUNT), severity F.
#
# POSITIVE assertions pair the negative check per the standing rule (a negative-only
# test that just checks DISMOUNT fails would pass even if OVMX had simply broken
# DISMOUNT/DKA0: entirely -- gutted, not fixed): DKA0: must still show "Mounted" with its
# "OVMXSYS" label in SHOW DEVICE, and DIRECTORY DKA0:[000000] must still list its files,
# proving the device is genuinely untouched, not just silently refused-and-broken.
#
# vms-b9f S2, round 4: the previous positive file-listing assertion
# (`EXPECT: contains:NEWDIR.DIR`) was ORDER-DEPENDENT on shared /vms filesystem state --
# it passed only because tests/dcl/test_create_dir.sh happens to run before this test
# (run_dcl_tests.sh globs test_*.sh alphabetically, "create" < "dismount") and leaks
# /vms/NEWDIR: that test's `SET DEFAULT SYS$SYSDEVICE:[dcl_test_$$]` silently fails, so
# its `CREATE/DIRECTORY [.NEWDIR]` lands at DKA0:[000000] instead of inside its own
# scratch directory, and its cleanup only removes its own scratch dir, not the
# mis-placed NEWDIR (test_create_dir.sh's bug, out of scope for this item). PROVEN: with
# /vms/NEWDIR removed and this test run alone, the assertion failed ("Total of 1 file",
# SYS0.DIR only). Fixed by making THIS test self-contained: create its own
# uniquely-named marker directory directly on DKA0:'s root before the DISMOUNT attempt,
# assert on that marker (not on another test's leftovers, and not authored to match a
# filter this test also writes), and remove it again afterward -- both before and after,
# idempotently -- so this test cannot itself become a future test's ordering dependency.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

MARKER_DIR="/vms/B9FCHKDIR"
rm -rf "$MARKER_DIR"
mkdir -p "$MARKER_DIR"

printf 'DISMOUNT DKA0:\nSHOW DEVICE\nDIRECTORY DKA0:[000000]\n' | $VMSDCL 2>&1

rm -rf "$MARKER_DIR"
