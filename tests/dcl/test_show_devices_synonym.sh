#!/bin/bash
# TEST: SHOW DEVICES (plural) is accepted as a synonym for SHOW DEVICE
# EXPECT: contains:DCL-ALIVE
# EXPECT_NOT: contains:IVKEYW
# EXPECT_NOT: contains:unrecognized SHOW keyword
#
# vms-9344a: the SHOW dispatcher matched only "DEVICE"; dcl_match_command
# rejects "DEVICES" (longer than "DEVICE"), so "SHOW DEVICES" fell through to
# the %DCL-E-IVKEYW fallback. VMS accepts the plural. This routes to the same
# cmd_show_device() as the singular.
#
# WHAT THIS ASSERTS ON A HOST WITH NO EXECUTIVE (CLAUDE.md Rule 9): the
# NEGATIVE property that "DEVICES" is a recognized keyword -- it must not
# raise IVKEYW. With no /dev/vms the device SCAN produces no rows (proven
# elsewhere: test_show_device.sh), but the keyword must dispatch. DCL-ALIVE is
# a trailing marker proving the session ran the whole deck without aborting.
VMSDCL="${VMSDCL:-vmsdcl}"
printf 'SHOW DEVICES\nWRITE SYS$OUTPUT "DCL-ALIVE"\n' | $VMSDCL 2>&1
