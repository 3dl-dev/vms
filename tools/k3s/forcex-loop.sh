#!/usr/bin/env bash
# forcex-loop.sh - boot the kernel-executive guest N times with ONLY corpus program
# $PROG enabled and report each CORPUS-RT verdict + the program's output on failure.
# Build/test tooling for the k3s rail (run it through tools/k3s/run-on-rail.sh --dind).
set -u
N=${N:-20}; PROG=${PROG:-sys_forcex}
docker build -q -f tests/qemu/Dockerfile -t ovmx-ktest:latest . >/dev/null || { echo "image build failed"; exit 2; }
ALL=$(docker run --rm --entrypoint sh ovmx-ktest:latest -c 'ls /tests/corpus_rt 2>/dev/null | grep -v "\.args$"')
SKIP=$(printf '%s\n' $ALL | grep -vx "$PROG" | paste -sd, -)
echo "programs in image: $(printf '%s\n' $ALL | wc -l); running only: $PROG"
pass=0; fail=0
for i in $(seq 1 "$N"); do
  out=$(docker run --rm --device /dev/kvm -e KE_WALL_TIMEOUT=900 -e OVMX_CORPUS_RT=1 -e OVMX_CORPUS_SKIP="$SKIP" ovmx-ktest:latest 2>&1 | tr -d '\r')
  line=$(printf '%s\n' "$out" | grep "^CORPUS-RT $PROG rc=")
  echo "run $i: ${line:-NO-VERDICT}"
  case "$line" in *"rc=0 "*) pass=$((pass+1));; *) fail=$((fail+1)); printf '%s\n' "$out" | grep -n "CORPUS-RT-LOG $PROG\|vms: .*$PROG" | head -200; printf '%s\n' "$out" > "/tmp/forcex-fail-$i.log";; esac
done
echo "RESULT $PROG: pass=$pass fail=$fail of $N"
[ "$fail" -eq 0 ]
