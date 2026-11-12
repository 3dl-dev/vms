#!/bin/sh
# test_fast_sh.sh <repo-root> -- tools/ci/fast.sh's cheap path (no build, no
# QEMU): it reports what the queue will owe for a diff, and exits nonzero on a
# bad argument. The host/QEMU legs are the existing harnesses it calls.
# It also proves fast.sh runs the negative-control fixtures self-test by
# default and goes RED on it: a scratch tree carrying one defect whose
# mutation no longer injects (the #2070/#2071 class) must turn fast.sh RED
# with the self-test's own FAIL line for that defect.
set -u
ROOT=${1:?usage: test_fast_sh.sh <repo-root>}
cd "$ROOT" || exit 1
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }
bash -n tools/ci/fast.sh && ok "fast.sh parses" || bad "fast.sh syntax"
out=$(tools/ci/fast.sh --no-fixtures --no-host --no-qemu --base HEAD 2>&1); rc=$?
[ $rc -eq 0 ] && echo "$out" | grep -q "the queue will run these heavy tiers" \
  && echo "$out" | grep -q "fast.sh: GREEN" && ok "tier report, GREEN with nothing to run" \
  || { bad "tier report (rc=$rc)"; echo "$out"; }

# -- fast.sh runs facility_defects.sh selftest by default, and a red one reds it.
scratch=$(mktemp -d "${TMPDIR:-/tmp}/ovmx-fastsh-test.XXXXXX") || exit 1
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/tests" "$scratch/tools"
cp -a src "$scratch/src" && cp -a tests/qemu "$scratch/tests/qemu" && cp -a tools/ci "$scratch/tools/ci" \
  || { bad "copy a scratch tree"; }
d=$(cd "$scratch/tests/qemu/defects.d" && LC_ALL=C ls *.defect | head -1 | sed 's/\.defect$//')
find "$scratch/tests/qemu/defects.d" -name '*.defect' ! -name "$d.defect" -delete
# The mutation becomes a no-op: its anchor "no longer matches the source tree".
printf '\ndefect_apply() { :; }\n' >> "$scratch/tests/qemu/defects.d/$d.defect"
( cd "$scratch" && git init -q . && git add -A \
  && git -c user.name=t -c user.email=t@t commit -qm scratch ) || bad "scratch git tree"
out=$(cd "$scratch" && tools/ci/fast.sh --no-host --no-qemu --base HEAD 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "FAIL: negative-control fixtures self-test" \
  && echo "$out" | grep -q "FAIL: $d: its sed anchor no longer matches" \
  && echo "$out" | grep -q "fast.sh: RED" \
  && ok "fast.sh runs the fixtures self-test by default; a non-injecting defect ($d) reds it" \
  || { bad "fixtures self-test not run / not fatal (rc=$rc)"; echo "$out"; }

tools/ci/fast.sh --jobs 0 >/dev/null 2>&1 && bad "--jobs 0 accepted" || ok "--jobs 0 refused"
tools/ci/fast.sh --bogus >/dev/null 2>&1 && bad "unknown flag accepted" || ok "unknown flag refused"
echo "fast_sh: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
