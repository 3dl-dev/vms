#!/bin/bash
# TEST: SHOW USERS reports process info without inventing a terminal name
# EXPECT: contains:Username
# EXPECT: regex:[0-9A-F]{8}
# EXPECT_NOT: contains:Node
# EXPECT_NOT: contains:_FTA
# EXPECT_NOT: regex:_[A-Z]{2,3}[0-9]+:
#
# COVERAGE CHANGED HERE, DELIBERATELY, AND WHAT WAS LOST (vms-fb9).
#
# This file used to read:
#   # TEST: SHOW USERS displays VMS terminal device names and process info
#   # EXPECT: regex:_[A-Z]{2,3}[0-9]+:
# and it passed because SHOW USERS printed
#   ctx->terminal.device_name[0] ? ctx->terminal.device_name : "_FTA0:"
# -- i.e. on any system where DCL did not know its terminal (which is every
# system, since knowing it required the VMS_TERMINAL environment handoff this
# item deletes) it printed the literal "_FTA0:". The assertion was therefore
# satisfied by a hardcoded string, not by a terminal.
#
# So the old EXPECT was not coverage of a working feature; it was what kept
# the fallback alive. It is inverted here rather than deleted: the same
# pattern now has to be ABSENT, so reintroducing any invented device name
# under any prefix trips it.
#
# The capability is not abandoned. A VMS terminal name identifies a device in
# the executive's device table (src/kernel/vms_devtab.c), and which device a
# given job is on is executive process-table state OVMX does not have. Until
# then the column is empty, which is the honest report (CLAUDE.md rule 10).
#
# WHY EACH SURVIVING LINE IS NOT VACUOUS:
#   contains:Username    the report header printed, so the command ran and
#                        produced its table -- an empty terminal column is
#                        DCL answering, not DCL failing to run.
#   regex:[0-9A-F]{8}    the PID column, a real value, still printed. Paired
#                        with the header this pins that the ROW exists; the
#                        EXPECT_NOTs then pin what may not be in it.
#   NOT contains:Node    inherited unchanged and independent of the above.
VMSDCL="${VMSDCL:-vmsdcl}"
echo 'SHOW USERS' | $VMSDCL 2>&1
