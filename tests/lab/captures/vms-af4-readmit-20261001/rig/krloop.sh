#!/bin/bash
# krloop.sh <art> <prefix> <n> : n kill+reboot arms in sequence, graded lines to kr-<prefix>.log
cd /lab/run-af4rig
: > kr-$2.log
for i in $(seq 1 $3); do bash kr.sh $1 $2-$i >> kr-$2.log 2>&1; done
echo "== $2 done: $(grep -c '\] PASS' kr-$2.log) PASS / $(grep -c '\] FAIL' kr-$2.log) FAIL / $(grep -c '\] NOFAULT' kr-$2.log) NOFAULT" >> kr-$2.log
