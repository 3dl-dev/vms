#!/bin/sh
# test_check_merge_gates.sh <repo-root> -- tools/ci/check_merge_gates.py passes
# the real workflows and goes red on each way a gate can stop covering its
# workflow: a job left out of `needs`, a gate without `if: always()`, a missing
# gate, and a pull_request paths filter. Copies of the workflows; no network.
set -u
ROOT=${1:?usage: test_check_merge_gates.sh <repo-root>}
TOOL="$ROOT/tools/ci/check_merge_gates.py"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }
fresh() { rm -rf "$T/w"; cp -r "$ROOT/.github/workflows" "$T/w"; }
run() { python3 "$TOOL" "$T/w" >"$T/out" 2>&1; echo $?; }

fresh
[ "$(run)" = 0 ] && ok "the real gating workflows each end in a complete gate" || { bad "real workflows -> 0"; cat "$T/out"; }

fresh; python3 - "$T/w/ci-vax.yml" <<'PY'
import sys, re
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"(  gate:\n(?:.*\n)*?    needs:\n)      - [^\n]+\n", r"\1", s, count=1)
open(p, "w").write(s)
PY
[ "$(run)" = 1 ] && grep -q "ci-vax.yml: gate does not need:" "$T/out" && ok "a job left out of the gate's needs is red" || { bad "dropped need -> 1"; cat "$T/out"; }

fresh; sed -i 's|^    if: \${{ always() }}$|    if: ${{ success() }}|' "$T/w/ci-alpha.yml"
[ "$(run)" = 1 ] && grep -q "ci-alpha.yml: gate is not .if: always()" "$T/out" && ok "a gate that is not if: always() is red" || { bad "not always -> 1"; cat "$T/out"; }

fresh; sed -i 's|^  gate:$|  notgate:|' "$T/w/ci-core-gates.yml"
[ "$(run)" = 1 ] && grep -q "ci-core-gates.yml: no .gate. job" "$T/out" && ok "a gating workflow without a gate is red" || { bad "no gate -> 1"; cat "$T/out"; }

fresh; python3 - "$T/w/ledger.yml" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace("  pull_request:\n    branches: [main]\n", "  pull_request:\n    branches: [main]\n    paths:\n      - 'docs/**'\n", 1)
open(p, "w").write(s)
PY
[ "$(run)" = 1 ] && grep -q "ledger.yml: pull_request has a paths filter" "$T/out" && ok "a paths filter on pull_request is red" || { bad "paths -> 1"; cat "$T/out"; }

echo "check_merge_gates: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
