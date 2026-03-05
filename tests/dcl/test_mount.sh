#!/bin/bash
# TEST: MOUNT/DISMOUNT commands create and remove VMS device mappings
# EXPECT: contains:%MOUNT-I-MOUNTED, TESTDISK mounted on _DUA0:
# EXPECT: contains:DUA0:
# EXPECT: contains:TESTDISK
# EXPECT: contains:%DISMOUNT-I-DISMOUNTED, _DUA0: dismounted
# EXPECT: contains:%MOUNT-I-MOUNTED, WORK mounted on _DKA0:
# EXPECT: contains:DKA0:
# EXPECT_NOT: contains:%DCL-E-
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'MOUNT DUA0: TESTDISK\nSHOW DEVICE\nDISMOUNT DUA0:\nMOUNT DKA0: WORK\nSHOW DEVICE\n' | $VMSDCL 2>&1
