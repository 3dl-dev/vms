#!/bin/bash
# TEST: PRINT/SUBMIT qualifier coverage (vms-7543) - /NAME and /HOLD do REAL
#       work against the queue manager, and an unbacked qualifier (/COPIES)
#       draws the authentic %DCL-W-IVQUAL. Grounded in the public VSI OpenVMS
#       DCL Dictionary PRINT and SUBMIT entries.
#
# --- /NAME=job-name sets the real queue-entry job name (visible in the
#     submission message AND in SHOW QUEUE) ---
# EXPECT: contains:MYPRINT
# EXPECT: contains:MYBATCH
#
# --- /HOLD places the entry in HOLDING state via vmsq_hold_entry() -> the
#     submission line reports "holding", not "queued" ---
# EXPECT: regex:PRINT-S-QUEUED, job MYPRINT .* holding
#
# --- /COPIES has no backing queue-entry field -> %DCL-W-IVQUAL, not silently
#     dropped (structural, q_print) ---
# EXPECT: contains:%DCL-W-IVQUAL, unrecognized qualifier - check validity, spelling, and placement - \COPIES\
#
# THE FINDING THIS GATES (docs/design-vms-parity-map.md sec 3): PRINT carried 1
# of ~40 Dictionary qualifiers and accepted any other silently. TRIPWIRE: drop
# the /NAME override or /HOLD call in cmd_print()/cmd_submit()
# (src/vmsdcl/dcl_cmd_process.c) and the job name / "holding" state disappears;
# drop /COPIES's absence from q_print (src/vmsdcl/dcl_builtin.c) - i.e. add it -
# and it stops being rejected. /NAME and /HOLD are honoured for real because
# they map onto vms_queue_entry.job_name and vmsq_hold_entry(); /COPIES has no
# field, so it is honestly rejected (over-restriction), not faked.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_pq_test_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
export VMSQ_DB_PATH="/tmp/QMAN_PQ_$$.DAT"
mkdir -p "/vms/$TDIR"
echo "print me" > "/vms/$TDIR/doc.txt"
echo "\$ EXIT" > "/vms/$TDIR/job.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nPRINT/NAME=MYPRINT/HOLD doc.txt\nSUBMIT/NAME=MYBATCH job.com\nPRINT/COPIES=2 doc.txt\nSHOW QUEUE /ALL\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "$VMSQ_DB_PATH"
