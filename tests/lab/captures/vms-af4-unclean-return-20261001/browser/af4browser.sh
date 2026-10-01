#!/bin/bash
# af4browser.sh : rd vms-af4 browser repro. Run 1 to CN=3, then KILL the browser
# (SIGKILL the harness and every chromium) with all three nodes members -- the
# unclean teardown -- then a fresh run 2. Both graded by visitor-gate's own JSON.
cd /work
CASE=page-order OUT_DIR=/out/af4-r1 RUN_MS=1500000 setsid node visitor-gate.mjs > /out/af4-r1.log 2>&1 &
H=$!
for i in $(seq 1 120); do
  grep -q '"cn3": *true' /out/af4-r1/*/result.json 2>/dev/null && break
  grep -qE 'added=\{"A":true,"B":true' /out/af4-r1.log 2>/dev/null && grep -q 'vaxcAdm=\["OVMXA","OVMXB"\]\|vaxcAdm=\["OVMXB","OVMXA"\]' /out/af4-r1.log && break
  sleep 10
done
echo "run1 state at kill: $(tail -1 /out/af4-r1.log)" > /out/af4-kill.txt
pkill -9 -f visitor-gate.mjs; pkill -9 -f chrom; sleep 5
echo "killed at $(date -u +%H:%M:%S)" >> /out/af4-kill.txt
CASE=page-order OUT_DIR=/out/af4-r2 RUN_MS=1500000 node visitor-gate.mjs > /out/af4-r2.log 2>&1
echo done >> /out/af4-kill.txt
