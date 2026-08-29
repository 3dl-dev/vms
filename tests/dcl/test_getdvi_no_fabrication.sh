#!/bin/bash
# TEST: F$GETDVI fabricates no device attribute when it cannot read the executive
#
# WHAT THIS GATES, AND WHY IT ASSERTS AN ABSENCE (vms-050).
#
# F$GETDVI used to answer from src/vmsdcl/dcl_lexical.c's OWN idea of a device,
# never the executive's:
#   - EXISTS returned "TRUE" for EVERY name, existent or not;
#   - DEVCLASS/DEVTYPE were guessed from a substring of the name
#     ("OPA0"/"TT" -> terminal 66/112, else disk 1/44);
#   - VOLNAM returned "OVMXSYS" (or "VOLUME") for a device nobody had mounted;
#   - MOUNTCNT returned the literal "1";
#   - DEVNAM echoed the caller's own text back as "_<whatever-you-typed>:";
#   - FREEBLOCKS/MAXBLOCK came from statvfs("/") -- the Linux root, not a VMS
#     volume at all.
# Every one of those is a user-visible VMS command inventing its own answer,
# the exact defect CLAUDE.md Rule 11 / INV-6 exist to kill.
#
# F$GETDVI is now a READER of the executive's I/O database: it resolves the
# device through vms_kif_getdvi_devnam() (the same reader F$DEVICE and SHOW
# DEVICE use) and reads volume state through vms_kif_getvol() (the ACP
# mounted-volume table). When the executive cannot be reached -- as under ctest,
# which runs on a host with no /dev/vms and never will have one (the only OVMX
# runtime is the kernel/QEMU path, Rule 9) -- it FAILS HONESTLY: EXISTS answers
# the real Boolean "FALSE", every other item emits %SYSTEM-W-NOSUCHDEV and
# leaves the value empty. It never invents an attribute.
#
# WHAT CAN BE PROVEN HERE, AND ONLY HERE, is the thing that used to be wrong:
# with no executive to read, F$GETDVI("<any device>","EXISTS") is "FALSE", and
# none of the fabricated markers (OVMXSYS, the "_SYS$SYSDEVICE:" DEVNAM echo,
# the substring-guessed 44/112 device types, the literal MOUNTCNT "1") appear.
# Restore ANY of the deleted fabrication branches and a marker reappears -- so
# these are discriminating assertions about dcl_lexical.c's own code, not
# decoration. In particular, revert EXISTS to its old unconditional "TRUE" and
# the `E = "FALSE"` assertion fails and the `E = "TRUE"` guard trips: the gate
# goes RED. (Verified by that mutation while writing this test.)
#
# This is deliberately NOT an endorsement of running OVMX without an executive;
# it is the statement that when the reader has nothing to read, it invents
# nothing. The POSITIVE proof -- a real device returning EXISTS=TRUE and its
# real VOLNAM/DEVCLASS from the executive, and a bogus device honest FALSE --
# runs against a real /dev/vms in the DCL/SHOW acceptance battery
# (tests/qemu/lib/dcl_acceptance_battery.sh, the F$GETDVI block), where the
# same function is proven reading REAL rows. One claims a VMS state; the other
# claims the code has no invented-attribute branch left in it.
#
# EXPECT: contains:E = "FALSE"
# EXPECT: contains:EX = "FALSE"
# EXPECT: contains:%SYSTEM-W-NOSUCHDEV
# EXPECT_NOT: contains:E = "TRUE"
# EXPECT_NOT: contains:OVMXSYS
# EXPECT_NOT: contains:_SYS$SYSDEVICE:
# EXPECT_NOT: contains:V = "VOLUME"
# EXPECT_NOT: contains:DT = "44"
# EXPECT_NOT: contains:DT = "112"
# EXPECT_NOT: contains:MC = "1"
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'E = F$GETDVI("ZZZ999:","EXISTS")\nSHOW SYMBOL E\nEX = F$GETDVI("SYS$SYSDEVICE","EXISTS")\nSHOW SYMBOL EX\nV = F$GETDVI("SYS$SYSDEVICE","VOLNAM")\nSHOW SYMBOL V\nDN = F$GETDVI("SYS$SYSDEVICE","DEVNAM")\nSHOW SYMBOL DN\nDT = F$GETDVI("OPA0:","DEVTYPE")\nSHOW SYMBOL DT\nMC = F$GETDVI("SYS$SYSDEVICE","MOUNTCNT")\nSHOW SYMBOL MC\n' | $VMSDCL 2>&1
