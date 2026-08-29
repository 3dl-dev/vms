#!/bin/bash
# TEST: SET VERIFY / SET NOVERIFY change verification state, observed the
#       OpenVMS-faithful way -- via a lexical, NOT a "SHOW VERIFY" command
#       (OpenVMS has no such SHOW keyword).
#
# Oracle (vms-050 UX-fidelity, captured 2026-08-29 on OpenVMS VAX V7.3 /
# lab-2 vaxlab-0 and Alpha V8.4 / lab-Alpha alphalab-0):
#     $ SET NOVERIFY $ ..F$ENVIRONMENT("VERIFY_PROCEDURE") -> FALSE
#     $ SET VERIFY   $ ..F$ENVIRONMENT("VERIFY_PROCEDURE") -> TRUE
#     $ SHOW VERIFY  -> %DCL-W-IVKEYW, unrecognized keyword ... \VERIFY\
#   On real VMS the verify state is read with a lexical; "SHOW VERIFY" is an
#   unrecognized SHOW keyword. A former OVMX "SHOW VERIFY -> VERIFY = ON/OFF"
#   was a fabrication (dcl_cmd_show.c cmd_show_verify) and has been removed.
#
# NOTE: the probe uses F$ENVIRONMENT("VERIFY_PROCEDURE"), which OVMX tracks
# faithfully. F$VERIFY() is the other VMS-faithful probe (oracle: 1/0), but
# OVMX's F$VERIFY() currently returns 0 after SET VERIFY -- a separate,
# pre-existing gap tracked out of band; do not conflate it with this surface.
#
# EXPECT: contains:VP_OFF=FALSE
# EXPECT: contains:VP_ON=TRUE
# EXPECT: contains:%DCL-W-IVKEYW
# EXPECT_NOT: contains:VERIFY = ON
# EXPECT_NOT: contains:VERIFY = OFF
VMSDCL="${VMSDCL:-vmsdcl}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"

# 1. SET NOVERIFY / SET VERIFY observed via F$ENVIRONMENT -- the faithful probe.
printf 'SET NOVERIFY\nWRITE SYS$OUTPUT "VP_OFF="+F$ENVIRONMENT("VERIFY_PROCEDURE")\nSET VERIFY\nWRITE SYS$OUTPUT "VP_ON="+F$ENVIRONMENT("VERIFY_PROCEDURE")\nSET NOVERIFY\n' | $VMSDCL 2>&1

# 2. SHOW VERIFY must be an unrecognized keyword, exactly as on real VMS.
printf 'SHOW VERIFY\n' | $VMSDCL 2>&1
