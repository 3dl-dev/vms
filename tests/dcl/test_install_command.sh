#!/bin/bash
# TEST: DCL's INSTALL builtin reaches the real SYS$SYSTEM:INSTALL.EXE
# (src/install/install.c, bead vms-913.5) instead of the flat-text
# SYS$MANAGER:INSTALL_LIST.DAT stub cmd_install used to maintain on its own
# (bead vms-913.7 fix). That stub was never read by anything: IMGACT.EXE's
# known-image search path (src/imgact/known_images.c) mmaps the binary KFE
# database at SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT, which only INSTALL.EXE writes.
# So `INSTALL ADD` issued from SYSTARTUP_VMS.COM was silently landing
# somewhere IMGACT.EXE never looked.
#
# Ground truth checked here is the KFE binary database's magic bytes on
# disk (KFE_MAGIC = 0x4B464521, little-endian on disk = "!EFK" —
# src/imgact/known_images.h), not just DCL's text output: the old stub could
# print a matching "%INSTALL-I-ADDED" line too, so the text alone would not
# distinguish "reached the real utility" from "reimplemented it badly".
#
# EXPECT: contains:INSTALL-I-ADDED
# EXPECT: contains:TESTLIB913$SHR.EXE
# EXPECT: contains:KFE-DB-MAGIC-OK
# EXPECT: contains:INSTALL-I-REMOVED
# EXPECT: contains:KFE-DB-EMPTY-AFTER-REMOVE-OK
# EXPECT_NOT: contains:Segmentation
# EXPECT_NOT: contains:KFE-DB-MAGIC-MISSING
# EXPECT_NOT: contains:KFE-DB-STILL-LISTED-AFTER-REMOVE

VMSDCL="${VMSDCL:-vmsdcl}"

SYSLIB=/vms/SYS0/SYSCOMMON/SYSLIB
SYSEXE=/vms/SYS0/SYSCOMMON/SYSEXE
DB="$SYSEXE/VMS\$KNOWN_IMAGES.DAT"
IMG="$SYSLIB/TESTLIB913\$SHR.EXE"

mkdir -p "$SYSLIB" "$SYSEXE"
rm -f "$DB"
echo "dummy shareable image" > "$IMG"

printf 'INSTALL ADD SYS$SHARE:TESTLIB913$SHR.EXE /OPEN /SHARED\nINSTALL LIST\nEXIT\n' | "$VMSDCL" 2>&1

if [ -f "$DB" ] && head -c4 "$DB" | grep -qa '!EFK'; then
    echo "KFE-DB-MAGIC-OK"
else
    echo "KFE-DB-MAGIC-MISSING"
fi

printf 'INSTALL REMOVE SYS$SHARE:TESTLIB913$SHR.EXE\nINSTALL LIST\nEXIT\n' | "$VMSDCL" 2>&1

if [ -f "$DB" ] && head -c4 "$DB" | grep -qa '!EFK'; then
    if grep -qa 'TESTLIB913\$SHR\.EXE' "$DB"; then
        echo "KFE-DB-STILL-LISTED-AFTER-REMOVE"
    else
        echo "KFE-DB-EMPTY-AFTER-REMOVE-OK"
    fi
else
    echo "KFE-DB-MAGIC-MISSING"
fi

rm -f "$DB" "$IMG"
