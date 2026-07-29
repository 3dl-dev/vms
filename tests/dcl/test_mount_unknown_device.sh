#!/bin/bash
# TEST: MOUNT rejects a device name that isn't a recognized VMS device class
# EXPECT: contains:%MOUNT-E-NOSUCHDEV
# EXPECT_NOT: contains:%MOUNT-I-MOUNTED
#
# Root cause (vms-b9f, docs/design-authenticity-roadmap.md §2.3): cmd_mount() accepted ANY
# string >= 2 chars as a device name and always reported success ("MOUNT DKA100: ... accepts
# anything"). Real VMS requires the device to be a real, autoconfigured unit; OVMX has no
# physical controllers, so it validates the device name against a known set of VMS device-
# class mnemonics (2-letter code + controller letter + unit number -- OpenVMS I/O device
# naming convention, public documentation) instead of accepting arbitrary strings.
#
# "ZZQ0:" does not match any known OVMX/VMS device class (ZZ is not a disk or tape
# controller mnemonic) and must be rejected with SS$_NOSUCHDEV, not silently mounted.
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
echo "MOUNT ZZQ0: BOGUS" | $VMSDCL 2>&1
