#!/bin/bash
# TEST: WAIT honours hundredths of a second -- 0:0:0.60 waits about 0.6 s, not 0 (rd vms-ef03)
# EXPECT: contains:WAIT-HUNDREDTHS-OK
# EXPECT_NOT: contains:WAITED
VMSDCL="${VMSDCL:-vmsdcl}"
s=$(date +%s%N)
echo 'WAIT 0:0:0.60' | timeout 5 $VMSDCL >/dev/null 2>&1
e=$(date +%s%N)
ms=$(( (e - s) / 1000000 ))
if [ "$ms" -ge 550 ] && [ "$ms" -lt 1000 ]; then
    echo "WAIT-HUNDREDTHS-OK"
else
    echo "WAITED ${ms} ms for WAIT 0:0:0.60"
fi
