#!/bin/sh
# safe_merge.sh -- THE way to merge a PR into main.
#
# usage: tools/ci/safe_merge.sh <pr-number> [--allow 'WORKFLOW::JOB::FIXED-BY' ...] [--dry-run]
#
# Refuses (exit 1) unless ALL of these hold, then squash-merges pinned to the
# head SHA it checked (gh pr merge --match-head-commit), so nothing that lands
# was not checked:
#   1. FRESH  -- tools/ci/merge_freshness.sh origin/main <head>: main has not
#                changed a file this PR changes since its checks ran.
#   2. ROLLUP -- the PR's own checks: none failed, none still pending.
#   3. HEAVY  -- tools/ci/merge_heavy_gates.py: every workflow the PR SKIPPED
#                passed in a workflow_dispatch run on the head, or failed only in
#                jobs mapped (--allow) to the PR/item that fixes them.
# The mapping table (from step 3) is posted as the merge comment.
#
# Exit 0 merged (or --dry-run passed), 1 refused, 2 usage/tool error.
set -u
here=$(cd "$(dirname "$0")" && pwd)
pr=""
dry=0
gate_args=""
while [ $# -gt 0 ]; do
    case "$1" in
        --allow) [ $# -ge 2 ] || { echo "usage: $0 <pr> [--allow 'W::J::FIX' ...] [--dry-run]" >&2; exit 2; }
                 gate_args="$gate_args
$2"; shift 2 ;;
        --dry-run) dry=1; shift ;;
        *) case "$1" in *[!0-9]*|"") echo "usage: $0 <pr> [--allow 'W::J::FIX' ...] [--dry-run]" >&2; exit 2 ;; esac
           pr=$1; shift ;;
    esac
done
[ -n "$pr" ] || { echo "usage: $0 <pr> [--allow 'W::J::FIX' ...] [--dry-run]" >&2; exit 2; }

sha=$(gh pr view "$pr" --json headRefOid --jq .headRefOid) || exit 2
git fetch -q origin || exit 2
git fetch -q origin "$sha" 2>/dev/null || git fetch -q origin "pull/$pr/head" || exit 2

echo "== 1. freshness (head $sha)"
sh "$here/merge_freshness.sh" origin/main "$sha"
fr=$?
[ $fr -eq 0 ] || { echo "REFUSED: not fresh against main (merge_freshness exit $fr)"; exit 1; }

echo "== 2. the PR's own checks"
bad=$(gh pr view "$pr" --json statusCheckRollup | python3 -c '
import json, sys
for c in json.load(sys.stdin).get("statusCheckRollup") or []:
    st = c.get("conclusion") or c.get("status") or "PENDING"
    if st not in ("SUCCESS", "SKIPPED", "NEUTRAL"):
        print("%s: %s" % (c.get("name"), st))
') || exit 2
if [ -n "$bad" ]; then
    echo "REFUSED: PR checks not green:"; printf '%s\n' "$bad" | sed 's/^/  /'; exit 1
fi
echo "  ok: no failed or pending PR check"

echo "== 3. heavy gates the PR skipped"
table=$(mktemp)
trap 'rm -f "$table"' EXIT
set -- "$pr" --table "$table"
old_ifs=$IFS; IFS='
'
for a in $gate_args; do [ -n "$a" ] && set -- "$@" --allow "$a"; done
IFS=$old_ifs
python3 "$here/merge_heavy_gates.py" "$@"
hg=$?
[ $hg -eq 0 ] || { echo "REFUSED: heavy gates (merge_heavy_gates exit $hg)"; exit 1; }

# The head must not have moved while we checked.
now=$(gh pr view "$pr" --json headRefOid --jq .headRefOid) || exit 2
[ "$now" = "$sha" ] || { echo "REFUSED: the PR head moved during the checks ($sha -> $now)"; exit 1; }

if [ $dry -eq 1 ]; then
    echo "DRY RUN: would merge #$pr at $sha"; cat "$table"; exit 0
fi
{
    echo "Merged with tools/ci/safe_merge.sh at head $sha: fresh against main, PR checks green, and every skipped heavy workflow green on this head or red only in jobs mapped to their fix:"
    echo
    cat "$table"
} | gh pr comment "$pr" --body-file - >/dev/null || exit 2
gh pr merge "$pr" --squash --match-head-commit "$sha" || exit 1
echo "MERGED #$pr at $sha"
