#!/bin/bash
# TEST: MOUNT/DISMOUNT commands create and remove VMS device mappings
# EXPECT: contains:%MOUNT-I-MOUNTED, TESTDISK mounted on _DUA0:
# EXPECT: contains:DUA0:
# EXPECT: contains:TESTDISK
# EXPECT: contains:%DISMOUNT-I-DISMOUNTED, _DUA0: dismounted
# EXPECT: contains:%MOUNT-I-MOUNTED, WORK mounted on _DJA0:
# EXPECT: contains:DJA0:
# EXPECT_NOT: contains:%DCL-E-
#
# NOTE (vms-b9f C1): the second MOUNT target used to be DKA0:. DKA0: is now boot-registered
# and already Mounted (SHOW DEVICE must list OVMX's real system disk from session start --
# vms-b9f), so a fresh MOUNT DKA0: now correctly returns %MOUNT-E-DEVMOUNT (already mounted),
# not a first-time-mount success. Swapped to DJA0: (also a real, recognized VMS disk class --
# see known_device_classes, dcl_builtin.c) to keep testing the exact same MOUNT/DISMOUNT
# round-trip across two distinct device classes without colliding with the boot device.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
printf 'MOUNT DUA0: TESTDISK\nSHOW DEVICE\nDISMOUNT DUA0:\nMOUNT DJA0: WORK\nSHOW DEVICE\n' | $VMSDCL 2>&1
