#!/bin/sh
# test_fast_sh.sh <repo-root> -- tools/ci/fast.sh's cheap path (no build, no
# QEMU): it reports what the queue will owe for a diff, and exits nonzero on a
# bad argument. The host/QEMU legs are the existing harnesses it calls.
set -u
ROOT=${1:?usage: test_fast_sh.sh <repo-root>}
cd "$ROOT" || exit 1
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }
bash -n tools/ci/fast.sh && ok "fast.sh parses" || bad "fast.sh syntax"
out=$(tools/ci/fast.sh --no-host --no-qemu --base HEAD 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q "the queue will run these heavy tiers" \
  && echo "$out" | grep -q "fast.sh: GREEN" && ok "tier report, GREEN with nothing to run" \
  || { bad "tier report (rc=$rc)"; echo "$out"; }
tools/ci/fast.sh --jobs 0 >/dev/null 2>&1 && bad "--jobs 0 accepted" || ok "--jobs 0 refused"
tools/ci/fast.sh --bogus >/dev/null 2>&1 && bad "unknown flag accepted" || ok "unknown flag refused"
echo "fast_sh: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
