#!/bin/bash
# TEST: F$GETSYI returns system information
# The VMS-compat version is TRUE-TO-ARCH (INV-1): V9.2-x on x86-64, OVMX's
# own version where the arch has no VMS lineage. Assert a well-formed token
# rather than a literal, and assert the pre-INV-1 hardcode is gone.
#
# vms-28a/vms-a5d: real VMS F$GETSYI("VERSION") is the FIXED 8-char SPACE-PADDED
# field (SYI$_VERSION; e.g. "V7.3    ", "V9.2-3  "), byte-confirmed on the live
# oracle -- OVMX now emits that faithful padded field on every arch, not the
# trimmed token. So (1) assert the version VALUE is well-formed, tolerating the
# trailing pad, and (2) POSITIVELY assert the field is exactly 8 chars wide --
# the faithful padding is the point, so the test asserts it rather than merely
# ignoring it. (Connects to vms-f5d, F$GETSYI surface-fidelity.)
# EXPECT: regex:X = "V[0-9]+\.[0-9]+(-[0-9]+)? *"
# EXPECT: regex:X = "[^"]{8}"
# EXPECT_NOT: contains:V7.3
# EXPECT_NOT: contains:%DCL-
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'X = F$GETSYI("VERSION")\nSHOW SYMBOL X\n' | $VMSDCL 2>&1
