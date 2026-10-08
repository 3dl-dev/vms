#!/usr/bin/env bash
# dcl-gate-loop.sh - run the Release E2E DCL/SHOW acceptance gate PAR at a time, ROUNDS times, on the rail
# (vms-330 reproduction under CPU contention). Prints every FAIL line per run.
set -u
PAR=${PAR:-3}; ROUNDS=${ROUNDS:-3}
if ! docker buildx version >/dev/null 2>&1; then   # the rail image ships docker without buildx
  mkdir -p ~/.docker/cli-plugins
  curl -fsSL --retry 5 -o ~/.docker/cli-plugins/docker-buildx https://github.com/docker/buildx/releases/download/v0.17.1/buildx-v0.17.1.linux-amd64 \
    && chmod +x ~/.docker/cli-plugins/docker-buildx
fi
for try in 1 2 3; do
  DOCKER_BUILDKIT=1 docker build --progress=plain -f distro/Dockerfile.bootable -t ovmx-boot:latest . > /tmp/build.log 2>&1 && break
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
    else fail=$((fail+1)); echo "round $r #$p: rc=$rc"; grep -E "^  FAIL:|NOTE:" /tmp/gate-$r-$p.log | head -12
      for n in $(grep -an '^  FAIL:' /tmp/gate-$r-$p.log | cut -d: -f1 | head -4); do
        echo "--- console before FAIL at line $n ---"; sed -n "$((n>16?n-16:1)),$((n))p" /tmp/gate-$r-$p.log | cat -v | cut -c1-220
      done; echo "--- end ---"; fi
  done
done
echo "RESULT dcl-gate: pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
