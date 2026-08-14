#!/bin/bash
# TEST: Queue/submit error paths emit REAL VSI OpenVMS JBC message idents, not
#       the invented QMANERR/SUBMITERR/PRINTERR/ENTNOTFND ones (vms-916,
#       authenticity Tier-0). Idents grounded to public VSI OpenVMS docs — see
#       docs/audit-message-idents-vms-916.md for the ident-by-ident citations.
#
# --- queue manager unavailable: an unreachable QMAN$MASTER.DAT path makes
#     ensure_queue_init() fail; real VMS says the queue manager is not running
#     (%JBC-E-JOBQUEDIS), NOT "queue manager initialization failed" ---
# EXPECT: contains:%JBC-E-JOBQUEDIS, system job queue manager is not running
# EXPECT_NOT: contains:QMANERR
#
# --- SUBMIT/PRINT to a queue that does not exist -> %JBC-E-NOSUCHQUE, not the
#     invented SUBMITERR/PRINTERR ---
# EXPECT: regex:%JBC-E-NOSUCHQUE, no such queue - NOSUCHQ
# EXPECT_NOT: contains:SUBMITERR
# EXPECT_NOT: contains:PRINTERR
#
# --- DELETE/ENTRY of a nonexistent entry -> the faithful two-line VMS chain
#     (%DELETE-W-SEARCHFAIL primary + -JBC-E-NOSUCHENT secondary), NOT the
#     invented ENTNOTFND ---
# EXPECT: regex:%DELETE-W-SEARCHFAIL, error searching for 999
# EXPECT: contains:-JBC-E-NOSUCHENT, no such entry
# EXPECT_NOT: contains:ENTNOTFND
VMSDCL="${VMSDCL:-vmsdcl}"

# --- Part 1: queue manager unavailable. Point the queue DB at a path inside a
#     directory that does not exist so vmsq_init() fails -> ensure_queue_init()
#     returns failure and every queue command takes the JOBQUEDIS path. ---
export VMSQ_DB_PATH="/nonexistent-ovmx-dir-$$/QMAN.DAT"
printf 'SHOW QUEUE\n' | $VMSDCL 2>&1

# --- Parts 2 & 3: a working queue DB (default SYS$BATCH/SYS$PRINT created), so
#     we reach the no-such-queue and no-such-entry conditions. ---
TDIR="dcl_qmsg_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
export VMSQ_DB_PATH="/tmp/QMAN_QMSG_$$.DAT"
mkdir -p "/vms/$TDIR"
echo "content" > "/vms/$TDIR/doc.txt"
echo "\$ EXIT" > "/vms/$TDIR/job.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nSUBMIT/QUEUE=NOSUCHQ job.com\nPRINT/QUEUE=NOSUCHQ doc.txt\nDELETE/ENTRY=999\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "$VMSQ_DB_PATH"
