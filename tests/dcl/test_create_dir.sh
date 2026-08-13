#!/bin/bash
# TEST: CREATE/DIRECTORY creates a new directory
#
# CREATE/DIRECTORY makes a new subdirectory; the VMS-authentic proof is that it
# then appears as a "NEWDIR.DIR;1" entry when its PARENT directory is listed (a
# directory is a .DIR file within its parent). An absolute target spec is used
# so the new directory lands deterministically under the parent, and the parent
# listing is a real, non-empty "Total of 1 file." — NOT the empty-directory
# "%DIRECT-W-NOFILES, no files found" result that listing the freshly created
# (still empty) directory itself would authentically produce (see vms-1c6 /
# test_directory_wildcards.sh). The previous form listed the new directory
# directly and relied on the old, non-authentic "header + Total of 0 files."
# output for an empty directory.
# Grounded (clean-room, Rule 8): VSI OpenVMS DCL Dictionary, DIRECTORY — a
# subdirectory is listed within its parent as name.DIR;1.
# EXPECT: contains:NEWDIR.DIR
# EXPECT: regex:Total of 1 file\.
# EXPECT_NOT: contains:mkdir:
# EXPECT_NOT: contains:%DIRECT-W-NOFILES
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"
printf 'CREATE/DIRECTORY SYS$SYSDEVICE:[%s.NEWDIR]\nDIRECTORY SYS$SYSDEVICE:[%s]\n' "$VDIR" "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR"
