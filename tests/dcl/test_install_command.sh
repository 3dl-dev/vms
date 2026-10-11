#!/bin/bash
# TEST: DCL's INSTALL builtin reaches the real SYS$SYSTEM:INSTALL.EXE
# (src/install/install.c), and with no executive that utility fails honestly
# (rd vms-220).
#
# The known file list is the executive's (VMS_IOCTL_KFE): there is no on-disk
# database any more (the former SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT). On this host
# there is no /dev/vms, so the real utility answers every request with
# %INSTALL-E-FAIL naming -SYSTEM-W-NOSUCHDEV -- text only INSTALL.EXE's
# executive request path prints, which is what proves DCL reached it (the old
# DCL stub printed "%INSTALL-I-ADDED" on its own). It must claim no result
# and write no substitute database. The executive side is proven against a
# real /dev/vms by tests/qemu/test_syssvc_kfe.c.
#
# EXPECT: contains:failed to CREATE entry for SYS$SHARE:TESTLIB913$SHR.EXE
# EXPECT: contains:NOSUCHDEV
# EXPECT: contains:failed to LIST entry
# EXPECT: contains:failed to REMOVE entry for SYS$SHARE:TESTLIB913$SHR.EXE
# EXPECT: contains:KFE-NO-DISK-DB-OK
# EXPECT_NOT: contains:INSTALL-I-ADDED
# EXPECT_NOT: contains:INSTALL-I-REMOVED
# EXPECT_NOT: contains:NOIMAGES
# EXPECT_NOT: contains:Segmentation
# EXPECT_NOT: contains:KFE-DISK-DB-WRITTEN

VMSDCL="${VMSDCL:-vmsdcl}"

SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
DB="$SYSEXE/VMS\$KNOWN_IMAGES.DAT"
IMG="$SYSLIB/TESTLIB913\$SHR.EXE"

mkdir -p "$SYSLIB" "$SYSEXE"
rm -f "$DB"
echo "dummy shareable image" > "$IMG"

printf 'INSTALL ADD SYS$SHARE:TESTLIB913$SHR.EXE /OPEN /SHARED\nINSTALL LIST\nINSTALL REMOVE SYS$SHARE:TESTLIB913$SHR.EXE\nEXIT\n' | "$VMSDCL" 2>&1

if [ -e "$DB" ]; then
    echo "KFE-DISK-DB-WRITTEN"
else
    echo "KFE-NO-DISK-DB-OK"
fi

rm -f "$DB" "$IMG"
