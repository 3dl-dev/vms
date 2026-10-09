#!/bin/sh
# test_merge_freshness.sh <repo-root> -- tools/ci/merge_freshness.sh goes STALE
# exactly when main moved in a file the PR changes (the #1490 x #1532 shape),
# and stays FRESH otherwise. Builds a throwaway git repo; no network.
set -u
ROOT=${1:?usage: test_merge_freshness.sh <repo-root>}
TOOL="$ROOT/tools/ci/merge_freshness.sh"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }

cd "$T" || exit 1
git init -q -b main . && git config user.email t@t && git config user.name t
mkdir -p src && printf 'old\n1\n2\n3\n4\n5\n6\n7\n' > src/acp.c && echo 'x' > src/other.c && git add -A && git commit -qm base
git checkout -q -b pr && echo 'uses OLD_MACRO' >> src/acp.c && git commit -qam pr
git checkout -q main

sh "$TOOL" main pr >/dev/null; rc=$?
[ $rc -eq 0 ] && ok "main not moved -> FRESH (exit 0)" || bad "main not moved -> FRESH (got $rc)"

echo 'y' >> src/other.c && git commit -qam 'main: other file'
sh "$TOOL" main pr >/dev/null; rc=$?
[ $rc -eq 0 ] && ok "main moved in a file the PR does not touch -> FRESH" || bad "disjoint move -> FRESH (got $rc)"

sed -i 's/old/renamed/' src/acp.c && git commit -qam 'main: rename in the same file'
out=$(sh "$TOOL" main pr); rc=$?
[ $rc -eq 1 ] && ok "main changed a file the PR also changes -> STALE (exit 1)" || bad "overlap -> STALE (got $rc)"
printf '%s\n' "$out" | grep -qx '  src/acp.c' && ok "STALE names the shared file" || bad "STALE names the shared file"
printf '%s\n' "$out" | grep -q 'src/other.c' && bad "STALE lists a file only main touched" || ok "STALE lists only shared files"

git checkout -q pr && git merge -q --no-edit main >/dev/null 2>&1; git checkout -q main
sh "$TOOL" main pr >/dev/null; rc=$?
[ $rc -eq 0 ] && ok "after the branch is updated to main -> FRESH" || bad "updated branch -> FRESH (got $rc)"

sh "$TOOL" main no-such-ref >/dev/null 2>&1; rc=$?
[ $rc -eq 2 ] && ok "unknown ref -> exit 2" || bad "unknown ref -> exit 2 (got $rc)"

echo "test_merge_freshness: $pass passed, $fail failed"
[ $fail -eq 0 ]
