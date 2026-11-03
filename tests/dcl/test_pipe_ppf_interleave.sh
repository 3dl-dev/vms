#!/bin/bash
# TEST: a PIPE segment writes its parent's file SYS$OUTPUT as RMS records, in order with the parent's own (rd vms-380f)
# EXPECT: contains:PIPE_PPF_ORDER_OK
# EXPECT_NOT: contains:PIPE_PPF_ORDER_BAD
# EXPECT_NOT: contains:HIDDEN_MID_SEGMENT
#
# On VMS a PIPE segment is a subprocess whose SYS$OUTPUT is its creator's
# process-permanent file: when DCL's SYS$OUTPUT is a log file, the segment's
# records land in that same file, between the records DCL wrote before and
# after the PIPE. Here DCL's SYS$OUTPUT is an RMS file (the harness's
# dcl_rms_run): the parent opens it shared (SHRPUT), each segment DCL is
# created with that file as its own SYS$OUTPUT/SYS$ERROR, and every $PUT --
# the parent's or a segment's -- lands at the file's end as it is then, so
# nothing a segment put is overwritten by the parent's next record.
#
# The order is checked here, over the records read back through RMS: P1..P6
# must appear once each, in sequence. A middle segment's output goes down the
# pipe to the next segment, never to the file (HIDDEN_MID_SEGMENT). SYS$ERROR
# (the IVVERB of a failing segment) is the same file under 2>&1.
VMSDCL="${VMSDCL:-vmsdcl}"
out=$(printf '%s\n' \
    'WRITE SYS$OUTPUT "P1_PARENT_BEFORE"' \
    'PIPE WRITE SYS$OUTPUT "P2_SEGMENT"' \
    'WRITE SYS$OUTPUT "P3_PARENT_BETWEEN"' \
    'PIPE WRITE SYS$OUTPUT "HIDDEN_MID_SEGMENT" | WRITE SYS$OUTPUT "P4_LAST_SEGMENT"' \
    'PIPE NO_SUCH_VERB_PPF' \
    'WRITE SYS$OUTPUT "P5_PARENT_AFTER_ERROR"' \
    'PIPE WRITE SYS$OUTPUT "P6_SEGMENT_AGAIN"' |
    $VMSDCL 2>&1)
rc=$?
printf '%s\n' "$out"
[ "$rc" = 70 ] && { echo "PIPE_PPF_ORDER_BAD: records not read back through RMS"; exit 1; }
seq=$(printf '%s\n' "$out" | grep -oE 'P[1-6]_[A-Z_]+|NO_SUCH_VERB_PPF' | tr '\n' ' ')
want='P1_PARENT_BEFORE P2_SEGMENT P3_PARENT_BETWEEN P4_LAST_SEGMENT NO_SUCH_VERB_PPF P5_PARENT_AFTER_ERROR P6_SEGMENT_AGAIN '
if [ "$seq" = "$want" ]; then
    echo PIPE_PPF_ORDER_OK
else
    echo "PIPE_PPF_ORDER_BAD: got [$seq]"
fi
