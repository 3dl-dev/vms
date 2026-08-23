#!/bin/bash
# TEST: SHOW STATUS shows process accounting, not the $STATUS condition value
# EXPECT: contains:DCL-ALIVE
# EXPECT_NOT: contains:Condition value
# EXPECT_NOT: contains:Status at
# EXPECT_NOT: regex:Message: %[A-Z]+-[A-Z]-NORMAL
#
# vms-df9c: SHOW STATUS used to print the last command's $STATUS condition
# value ("Condition value: %X...", "Message: ...") -- that is F$STATUS, not
# what VMS SHOW STATUS reports. It now reads the current process's resource
# accounting from the executive's $GETJPI row (vms_kif_getjpi_self), the same
# source SHOW PROCESS uses.
#
# WHAT THIS ASSERTS ON A HOST WITH NO EXECUTIVE (CLAUDE.md Rule 9): the
# NEGATIVE regression -- the old $STATUS-condition output is gone. With no
# /dev/vms the $GETJPI read fails honestly and SHOW STATUS prints no
# accounting (INV-6: nothing fabricated), so the POSITIVE proof of real
# elapsed CPU / page faults / working-set output is under QEMU against a real
# /dev/vms, not here. DCL-ALIVE proves the deck ran to completion.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW STATUS\nWRITE SYS$OUTPUT "DCL-ALIVE"\n' | $VMSDCL 2>&1
