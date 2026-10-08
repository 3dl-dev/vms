#!/usr/bin/env bash
# dcl-gate-loop.sh - run the Release E2E DCL/SHOW acceptance gate PAR at a time, ROUNDS times, on the rail
# (vms-330 reproduction under CPU contention). Prints every FAIL line per run.
set -u
PAR=${PAR:-3}; ROUNDS=${ROUNDS:-3}
for try in 1 2 3; do
  docker build -f distro/Dockerfile.bootable -t ovmx-boot:latest . > /tmp/build.log 2>&1 && break
  echo "build attempt $try failed"; tail -n 15 /tmp/build.log; [ "$try" -lt 3 ] || exit 2; sleep 30
done
pass=0; fail=0
for r in $(seq 1 "$ROUNDS"); do
  for p in $(seq 1 "$PAR"); do
    ( OVMX_QEMU_FULL_E2E=1 OVMX_BOOT_IMAGE=ovmx-boot:latest tests/qemu/run_dcl_acceptance_e2e.sh > /tmp/gate-$r-$p.log 2>&1; echo $? > /tmp/gate-$r-$p.rc ) &
  done
  wait
  for p in $(seq 1 "$PAR"); do
    rc=$(cat /tmp/gate-$r-$p.rc)
    if [ "$rc" = 0 ]; then pass=$((pass+1)); echo "round $r #$p: PASS"
    else fail=$((fail+1)); echo "round $r #$p: rc=$rc"; grep -E "^  FAIL:|NOTE:" /tmp/gate-$r-$p.log | head -12; fi
  done
done
echo "RESULT dcl-gate: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
