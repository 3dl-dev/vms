#!/bin/bash
# TEST: vms-f89 - SYS$PRINT / SYS$BATCH are live default-queue logicals (config
#       from logicals, parent vms-704). PRINT / SUBMIT translate the logical at
#       point of use, so DEFINE redirects a bare PRINT / SUBMIT live.
#
# --- DEFINE SYS$PRINT SYS$BATCH then a bare PRINT queues to SYS$BATCH, not the
#     literal SYS$PRINT. On origin/main the default was hardcoded "SYS$PRINT",
#     so the DEFINE had no effect and the job still went to SYS$PRINT.
# EXPECT: regex:PRINT-S-QUEUED,.*queue SYS.BATCH
# --- DEFINE SYS$BATCH SYS$PRINT then a bare SUBMIT queues to SYS$PRINT.
# EXPECT: regex:SUBMIT-S-SUBMITTED,.*queue SYS.PRINT
#
# THE FINDING THIS GATES (docs/design-vms-parity-map.md; VSI OpenVMS DCL
# Dictionary, PRINT /QUEUE default SYS$PRINT and SUBMIT /QUEUE default
# SYS$BATCH): SYS$PRINT and SYS$BATCH are logical names for the default
# print/batch queues; a site DEFINEs them to redirect. cmd_print()/cmd_submit()
# hardcoded the literal, so the redirection did nothing. TRIPWIRE: revert the
# point-of-use dcl_translate_logical() read in src/vmsdcl/dcl_cmd_process.c and
# both jobs land on their literal default queues, so both EXPECTs fail.
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_f89q_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
export VMSQ_DB_PATH="/tmp/QMAN_F89Q_$$.DAT"
mkdir -p "/vms/$TDIR"
echo "print me" > "/vms/$TDIR/doc.txt"
echo "\$ EXIT" > "/vms/$TDIR/job.com"
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nDEFINE SYS$PRINT SYS$BATCH\nPRINT doc.txt\nDEFINE SYS$BATCH SYS$PRINT\nSUBMIT job.com\n' "$VDIR" | $VMSDCL 2>&1
rm -rf "/vms/$TDIR" "$VMSQ_DB_PATH"
