#!/bin/bash
# TEST: F$GETQUI honours the caller's queue selection and fabricates no
#       SYS$BATCH data for a queue the caller did not ask about
#
# WHAT THIS GATES, AND WHY (vms-050).
#
# F$GETQUI's DISPLAY_QUEUE handler (src/vmsdcl/dcl_lexical.c, lex_getqui) used to
# read real userspace queue state via vmsq_show_queue() -- BUT it pinned the
# queue name to a hardcoded "SYS$BATCH" and DISCARDED the caller's object-id
# argument entirely:
#
#     const char *qname = "SYS$BATCH";
#     int rc = vmsq_show_queue(qname, &qinfo);   /* caller's queue ignored */
#
# So F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","SYS$PRINT") answered "SYS$BATCH", and
# a query for a queue that does not exist answered "SYS$BATCH" too. Right
# facility, wrong/ignored selection -- a fabrication: it reports SYS$BATCH's data
# as if it were the requested queue. That is the exact defect CLAUDE.md Rule 11 /
# INV-6 exist to kill (F$GETQUI is the DCL Dictionary's window onto a SPECIFIED
# queue, VSI OpenVMS DCL Dictionary; HELPLIB.HLP's own example passes the queue
# name as the object-id: F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","SYS$BATCH")).
#
# F$GETQUI now HONOURS the object-id: it looks the requested queue up in the real
# queue state (the same vmsq_show_queue the SUBMIT/PRINT/SHOW QUEUE verbs read),
# after ensure_queue_init() has brought SYS$BATCH + SYS$PRINT up as a running VMS
# always has them. A queue that genuinely exists returns its own real values; a
# queue that does not (and a call with no queue named -- OVMX does not implement
# the wildcard-context walk) returns the empty value and sets $STATUS to the
# honest no-such-queue status. It never returns SYS$BATCH's data for another ask.
#
# THE DISCRIMINATING ASSERTIONS. OVMX has BOTH SYS$BATCH and SYS$PRINT, so the
# selection is observable: a SYS$PRINT query MUST answer "SYS$PRINT", never
# "SYS$BATCH". Reinstate the pinned `qname = "SYS$BATCH"` discard and:
#   - P (the SYS$PRINT query) becomes "SYS$BATCH"   -> `P = "SYS$PRINT"` FAILS
#                                                       and the EXPECT_NOT trips;
#   - Z (a bogus-queue query) becomes "SYS$BATCH"   -> the `Z = ""` EXPECT FAILS
#                                                       and the EXPECT_NOT trips.
# So these are assertions about lex_getqui's own code, not decoration. (Verified
# RED by that mutation while writing this test.) B proves the honest positive:
# a SYS$BATCH query still returns SYS$BATCH's real name.
#
# VMSQ_DB_PATH points the queue manager at a throwaway file so the gate neither
# reads nor writes any shared queue database.
#
# EXPECT: contains:B = "SYS$BATCH"
# EXPECT: contains:P = "SYS$PRINT"
# EXPECT: contains:Z = ""
# EXPECT: contains:E = ""
# EXPECT_NOT: contains:P = "SYS$BATCH"
# EXPECT_NOT: contains:Z = "SYS$BATCH"
# EXPECT_NOT: contains:E = "SYS$BATCH"
VMSDCL="${VMSDCL:-vmsdcl}"
export VMSQ_DB_PATH="/tmp/QMAN_GETQUI_NF_$$.DAT"
printf 'B = F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","SYS$BATCH")\nSHOW SYMBOL B\nP = F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","SYS$PRINT")\nSHOW SYMBOL P\nZ = F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","BOGUS$NOSUCHQUE")\nSHOW SYMBOL Z\nE = F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME")\nSHOW SYMBOL E\n' | $VMSDCL 2>&1
rm -f "$VMSQ_DB_PATH"
