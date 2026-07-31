#!/bin/bash
# TEST: F$ENVIRONMENT("TERMINAL") does not invent a terminal device name
# EXPECT: contains:TERM =
# EXPECT_NOT: contains:/dev/
# EXPECT_NOT: contains:_FTA
# EXPECT_NOT: regex:TERM = "_[A-Z]{2,3}[0-9]+:"
#
# COVERAGE CHANGED HERE, DELIBERATELY, AND WHAT WAS LOST (vms-fb9).
#
# This file used to read:
#   # TEST: F$ENVIRONMENT("TERMINAL") returns a VMS terminal device name
#   # EXPECT: regex:_[A-Z]{2,3}[0-9]+:
# and it passed because DCL handed ITSELF a terminal name -- out of the
# VMS_TERMINAL environment variable its parent set, or out of a private
# "_FTA" pool file, or (when both were absent, as under ctest) out of a
# "_FTA0:" default compiled into vms_terminal_init(). Every DCL process on
# the system therefore claimed the same terminal, and no other process could
# see, verify or contradict the claim.
#
# So the old assertion was not coverage of a working feature -- it was the
# thing REQUIRING the feature to be fake. It could only ever be satisfied by
# a fabricated name, and it is why the fabrication had three separate
# sources: delete one and the assertion still passed on the next.
#
# The capability itself is NOT deleted and NOT abandoned. On VMS this lexical
# reports the terminal the executive recorded for this job; OVMX cannot
# answer it yet because the executive has a device table
# (src/kernel/vms_devtab.c) but no process table to bind a job to a device.
# Until then the honest answer is no answer -- CLAUDE.md rule 10: match VMS,
# or report nothing; never invent a plausible-looking value.
#
# WHAT IS ASSERTED NOW, and why each line is not vacuous:
#   contains:TERM =        the lexical still evaluates and SHOW SYMBOL still
#                          runs, so an empty result is DCL answering, not DCL
#                          crashing or the command silently vanishing.
#   NOT contains:/dev/     never leak a Linux device path into VMS output.
#                          This check is inherited unchanged and is
#                          independent of everything else here.
#   NOT contains:_FTA      the specific fabrications this item deleted: the
#                          "_FTA" pool prefix and the "_FTA0:" default.
#   NOT regex:TERM = "_..." any OTHER invented VMS device name, so a future
#                          "helpful" default under a different prefix (_TTA0:,
#                          _OPA0:) is caught too. Reverting any one of the
#                          three deleted sources trips this.
VMSDCL="${VMSDCL:-vmsdcl}"
# Use symbol assignment since WRITE does not inline-evaluate lexicals
cat <<'EOF' | $VMSDCL 2>&1
$ TERM = F$ENVIRONMENT("TERMINAL")
$ SHOW SYMBOL TERM
EOF
