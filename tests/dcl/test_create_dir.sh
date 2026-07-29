#!/bin/bash
# TEST: CREATE/DIRECTORY creates a new directory
# EXPECT: regex:NEWDIR_[0-9]+\.DIR
# EXPECT_NOT: contains:mkdir:
#
# vms-b9f T2, round 5: this test used to SET DEFAULT to its own scratch directory
# (SYS$SYSDEVICE:[dcl_test_$$]) and then CREATE/DIRECTORY [.NEWDIR] -- expecting NEWDIR
# to land inside that scratch directory. It never did. Proven: after the fix below,
# `SHOW DEFAULT` correctly reports the scratch directory as current (SET DEFAULT itself
# is not the bug), but CREATE/DIRECTORY [.NEWDIR] still created /vms/NEWDIR at the VMS
# root every time, regardless of SET DEFAULT.
#
# Root cause (verified in src/vmsdcl/dcl_filespec.c dcl_resolve_path(), NOT fixed here):
# dcl_resolve_path() only prepends ctx->default_dir (the SET DEFAULT'd directory) for a
# BARE filename with no brackets/colon. A relative directory spec like "[.NEWDIR]" that
# carries brackets but no device is passed straight through, unmodified, to
# vmsfs_to_linux_path() (src/vmsfs/vmsfs_translate.c) -- which never sees
# ctx->default_dir at all. That function's own "no device" branch then resolves any
# directory component that doesn't itself start with "." (and after
# vmsfs_translate_directory() strips the leaf ".NEWDIR"'s leading dot down to "NEWDIR",
# it no longer does) unconditionally against vms_default_root, the VMS filesystem root
# -- i.e. always /vms/NEWDIR, never wherever SET DEFAULT pointed. This is PRE-EXISTING
# (confirmed: `git log <base>..HEAD -- src/vmsdcl/dcl_filespec.c
# src/vmsfs/vmsfs_translate.c` shows no vms-b9f commit ever touched either file) and
# affects every DCL command that takes a bare "[.SUB]" relative directory spec, not just
# CREATE/DIRECTORY -- well outside SHOW DEVICE/MOUNT/DISMOUNT authenticity, this item's
# actual scope. Reported, not fixed, here (see oracle_pins_for_signoff /
# unresolved_constraints in the vms-b9f round-5 report) -- it needs its own item.
#
# The actual, in-scope defect this round fixes: because CREATE/DIRECTORY [.NEWDIR]
# ALWAYS lands at the fixed path /vms/NEWDIR regardless of SET DEFAULT, the test's
# cleanup (which only removed its own scratch dir, /vms/$TDIR) left /vms/NEWDIR behind
# on the SHARED host /vms tree forever -- observed leaking into tests/dcl/
# test_dismount_system_disk.sh's positive assertion in an earlier round (fixed there,
# round 4, by giving that test its own marker instead of relying on this leftover). Since
# /vms is not worktree-isolated (multiple concurrently-dispatched agents on this machine
# observably share it -- /vms/NEWDIR was already present here before this round's first
# command ran), a fixed name is also a cross-agent race hazard, not just a leftover.
# Fixed by using a PID-suffixed marker name (matching the EXPECT regex above) instead of
# the literal "NEWDIR", and cleaning it up at the location it ACTUALLY lands
# (/vms/NEWDIR_$$) in addition to the scratch dir, so this test can no longer leak state
# for any other test or concurrent agent on this machine.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
MARKER="NEWDIR_$$"
mkdir -p "/vms/$TDIR"
rm -rf "/vms/$MARKER"
# List DKA0:[000000] (the VMS root -- where CREATE/DIRECTORY [.marker] actually lands,
# per the root-cause note above), not [.marker] itself, so the assertion is a positive
# catalog entry ("NEWDIR_nnn.DIR" listed as a file) rather than an empty sub-listing that
# would pass even if CREATE/DIRECTORY silently did nothing.
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nCREATE/DIRECTORY [.%s]\nDIRECTORY DKA0:[000000]\n' "$VDIR" "$MARKER" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "/vms/$MARKER"
