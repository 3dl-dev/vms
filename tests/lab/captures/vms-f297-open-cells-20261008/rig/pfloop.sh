#!/bin/bash
# pfloop.sh <art> <tag> <n> -- rd vms-f297: n arms of the highest-SCSSYSTEMID-OVMX scenario, alternating
# runP (OVMXA coordinates OVMXB's admission with the real VAXC a member) and runPK (the same, then OVMXB
# is killed: the VAX must report losing a MEMBER). Graded from every console: BUG CHECK anywhere = FAIL.
ART=$1; TAG=$2; N=$3; R=/lab/run-dlm2; L=$R/loop-$TAG.log; : > $L
pass=0; fail=0
for i in $(seq 1 $N); do
  if [ $((i % 2)) -eq 1 ]; then S=runP.sh; else S=runPK.sh; fi
  T=$TAG-$i
  bash $R/$S $ART $T > $R/$T.run.log 2>&1
  D=$R/runs/$T
  bug=$(cat $D/*.console.log 2>/dev/null | grep -ac 'BUG CHECK\|BUGCHECK\|bugcheck')
  bmem=$(grep -c 'OVMXB member at' $R/$T.run.log)
  lost=NA; [ $S = runPK.sh ] && lost=$(grep -c 'VAXC: lost connection to system OVMXB' $R/$T.run.log)
  if [ "$bug" = 0 ] && [ "$bmem" = 1 ] && { [ "$lost" = NA ] || [ "$lost" = 1 ]; }; then v=PASS; pass=$((pass+1)); else v=FAIL; fail=$((fail+1)); fi
  echo "[$T] $v script=$S bugchecks=$bug B-member=$bmem vax-lost-B=$lost" | tee -a $L
done
echo "===== $TAG done: $pass PASS / $fail FAIL =====" | tee -a $L
