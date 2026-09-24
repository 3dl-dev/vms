#!/bin/sh
#
# test_workflow_size_guard_negctl.sh - negative controls for the workflow-size
# gate (rd vms-1af).
#
# WHY THIS EXISTS. test_workflow_size_guard.sh currently reports PASS for the
# real .github/workflows/*.yml. A check that always reports PASS, regardless
# of what it is handed, proves nothing -- this is the ANTI-LARP requirement:
# "if your tested guard isn't actually checked against real drift, it's just
# prose". These controls run tools/ci/check_workflow_sizes.py against
# SANDBOX fixture files (never the tracked workflows) and require it to go
# red, for the right reason, on an oversized file.
#
# Usage: test_workflow_size_guard_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
CHECK="$SRC_ROOT/tools/ci/check_workflow_sizes.py"

status=0
passed=0
failed=0

command -v python3 >/dev/null 2>&1 || { echo "FAIL: python3 not available"; exit 1; }
[ -f "$CHECK" ] || { echo "FAIL: $CHECK missing"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "Workflow-size gate negative controls: oversized files must be caught, not certified"

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. A small, real-shaped workflow file must PASS at the
# default threshold. Without this, a FAIL below could mean the checker is
# just broken (always red), not that it correctly caught the oversized case.
# ---------------------------------------------------------------------------
mkdir -p "$WORK/small"
printf 'name: tiny\non:\n  push: {}\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n      - run: echo hi\n' > "$WORK/small/tiny.yml"

out=$(python3 "$CHECK" --dir "$WORK/small" --threshold 450000 2>&1)
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "  PASS: positive control - a small real-shaped workflow passes at the default threshold"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - a small workflow FAILED the checker, so no negative"
    echo "        control below can attribute a RED to being genuinely oversized"
    echo "$out" | sed 's/^/          | /'
    failed=$((failed + 1))
    status=1
fi

# ---------------------------------------------------------------------------
# NEGATIVE CONTROL: a file over the threshold in raw bytes must be caught,
# and the checker must exit non-zero and name the file. This is the exact
# mechanism vms-1af hit (ci.yml crossed a raw-byte ceiling and GitHub
# silently ran zero jobs) -- the guard checks raw size, not a parsed or
# comment-stripped approximation, so this control uses a plain oversized
# file of ordinary (non-comment) content.
# ---------------------------------------------------------------------------
mkdir -p "$WORK/big"
{
    printf 'name: big\non:\n  push: {}\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n'
    i=0
    while [ "$i" -lt 2000 ]; do
        printf '      - run: echo "this is step number %d doing real work"\n' "$i"
        i=$((i + 1))
    done
} > "$WORK/big/big.yml"

big_size=$(wc -c < "$WORK/big/big.yml")
out=$(python3 "$CHECK" --dir "$WORK/big" --threshold 1000 2>&1)
rc=$?
if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -qF "big.yml"; then
    echo "  PASS: negative control - an oversized workflow file (raw $big_size bytes) is caught (rc=1, names the file)"
    passed=$((passed + 1))
else
    echo "  FAIL: negative control - expected rc=1 naming big.yml (raw $big_size bytes), got rc=$rc:"
    echo "$out" | sed 's/^/          | /'
    failed=$((failed + 1))
    status=1
fi

# ---------------------------------------------------------------------------
# NEGATIVE CONTROL 2: a file just at the threshold passes, one byte over
# fails -- confirms the boundary is inclusive/exclusive as documented (">",
# not ">="), not an off-by-one that either always trips or never does.
# ---------------------------------------------------------------------------
mkdir -p "$WORK/boundary"
python3 - "$WORK/boundary/at.yml" "$WORK/boundary/over.yml" <<'EOF'
import sys
at_path, over_path = sys.argv[1], sys.argv[2]
base = b'name: b\non:\n  push: {}\njobs:\n  a:\n    runs-on: ubuntu-latest\n    steps:\n      - run: echo hi\n'
pad_at = b'#' + b'x' * (1000 - len(base) - 2) + b'\n'
open(at_path, 'wb').write(base + pad_at)
open(over_path, 'wb').write(base + pad_at + b'#\n')
EOF
at_size=$(wc -c < "$WORK/boundary/at.yml")
over_size=$(wc -c < "$WORK/boundary/over.yml")
rm -f "$WORK/boundary/over.yml.bak" 2>/dev/null
mkdir -p "$WORK/boundary_at" "$WORK/boundary_over"
mv "$WORK/boundary/at.yml" "$WORK/boundary_at/"
mv "$WORK/boundary/over.yml" "$WORK/boundary_over/"

out_at=$(python3 "$CHECK" --dir "$WORK/boundary_at" --threshold "$at_size" 2>&1)
rc_at=$?
out_over=$(python3 "$CHECK" --dir "$WORK/boundary_over" --threshold "$at_size" 2>&1)
rc_over=$?

if [ "$rc_at" -eq 0 ] && [ "$rc_over" -eq 1 ]; then
    echo "  PASS: negative control - exactly-at-threshold ($at_size bytes) passes, one byte over ($over_size bytes) fails"
    passed=$((passed + 1))
else
    echo "  FAIL: boundary control - expected at-threshold rc=0 (got $rc_at) and over-threshold rc=1 (got $rc_over)"
    echo "$out_at" | sed 's/^/          at  | /'
    echo "$out_over" | sed 's/^/          over| /'
    failed=$((failed + 1))
    status=1
fi

echo ""
echo "=== Workflow-size gate negative controls: $passed passed, $failed failed ==="
exit "$status"
