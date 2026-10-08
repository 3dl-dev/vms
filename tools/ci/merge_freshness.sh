#!/bin/sh
# merge_freshness.sh -- is a PR head's green CI still evidence about the merge?
#
# usage: tools/ci/merge_freshness.sh <main-ref> <pr-head-ref>
#
# A PR's checks test the merge of its head with main AS MAIN WAS when they ran.
# GitHub reports a clean textual merge long after main has moved, so a PR can
# merge green and still break main: #1490 (an ACP owner check naming
# ACP_MAXSYSGROUP) merged green after #1532 had renamed that macro in the same
# file, and vms.ko stopped compiling (2026-10-08).
#
# STALE (exit 1) when main has gained commits since the PR's merge base that
# touch any file the PR also changes -- the case where the old green result
# says nothing about the merge. Update the branch to current main, let CI run
# again, and merge only on that result. FRESH (exit 0) when main has not moved,
# or moved only in files the PR does not touch. Exit 2 on a usage error or an
# unknown ref.
#
# Limitation: a semantic break between two DISJOINT file sets (a header renamed
# on main, used by a new file in the PR) is not caught here.
set -u
main=${1:-}
head=${2:-}
if [ -z "$main" ] || [ -z "$head" ]; then
    echo "usage: $0 <main-ref> <pr-head-ref>" >&2
    exit 2
fi
git rev-parse --verify --quiet "$main^{commit}" >/dev/null || { echo "merge_freshness: unknown ref '$main'" >&2; exit 2; }
git rev-parse --verify --quiet "$head^{commit}" >/dev/null || { echo "merge_freshness: unknown ref '$head'" >&2; exit 2; }

base=$(git merge-base "$main" "$head") || { echo "merge_freshness: no merge base" >&2; exit 2; }
if [ "$(git rev-parse "$main")" = "$base" ]; then
    echo "FRESH: main has not moved since the PR's merge base ($(git rev-parse --short "$base"))"
    exit 0
fi

pr_files=$(git diff --name-only "$base" "$head" | sort -u)
main_files=$(git diff --name-only "$base" "$main" | sort -u)
# Each list is already unique, so a name seen twice is in both.
overlap=$(printf '%s\n%s\n' "$pr_files" "$main_files" | grep -v '^$' | sort | uniq -d)
if [ -n "$overlap" ]; then
    n=$(git rev-list --count "$base..$main")
    echo "STALE: main gained $n commit(s) since the PR's merge base $(git rev-parse --short "$base") that touch files this PR changes:"
    printf '%s\n' "$overlap" | sed 's/^/  /'
    echo "Update the branch to current main and let CI run again before merging."
    exit 1
fi
echo "FRESH: main moved since $(git rev-parse --short "$base"), but not in any file this PR changes"
exit 0
