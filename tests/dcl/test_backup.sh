#!/bin/bash
# TEST: BACKUP creates, lists, and restores savesets
# EXPECT: contains:%BACKUP-S-COPIED
# EXPECT: contains:%BACKUP-S-SAVESET
# EXPECT: contains:%BACKUP-S-RESTORED
# EXPECT: contains:BACKUP_CONTENT_1
# EXPECT: contains:BACKUP_CONTENT_2
# EXPECT: contains:FILE1.TXT
# EXPECT_NOT: contains:error
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_backup_test_$$"
RDIR="dcl_backup_restore_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
VRDIR="$(echo "$RDIR" | tr a-z A-Z)"
mkdir -p "/vms/$VDIR" "/vms/$VRDIR"
echo "BACKUP_CONTENT_1" > "/vms/$VDIR/file1.txt"
echo "BACKUP_CONTENT_2" > "/vms/$VDIR/file2.txt"

# Step 1: Create saveset with /SAVE_SET
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nBACKUP *.TXT TEST.BCK /SAVE_SET\n' "$VDIR" | $VMSDCL 2>&1

# Step 2: List saveset contents
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nBACKUP /LIST TEST.BCK\n' "$VDIR" | $VMSDCL 2>&1

# Step 3: Restore saveset to new directory
printf 'BACKUP SYS$SYSDEVICE:[%s]TEST.BCK SYS$SYSDEVICE:[%s] /SAVE_SET\n' "$VDIR" "$VRDIR" | $VMSDCL 2>&1

# Step 4: Verify restored files have correct content
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nTYPE FILE1.TXT\nTYPE FILE2.TXT\n' "$VRDIR" | $VMSDCL 2>&1

# Cleanup
rm -rf "/vms/$VDIR" "/vms/$VRDIR"
