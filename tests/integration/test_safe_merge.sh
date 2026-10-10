#!/bin/sh
# test_safe_merge.sh <repo-root> -- tools/ci/safe_merge.sh refuses a STALE PR, a
# PR with a failed check, and passes (dry run) a fresh green one. Throwaway git
# repos + a stub gh; no network.
set -u
ROOT=${1:?usage: test_safe_merge.sh <repo-root>}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }

git init -q --bare "$T/origin.git"
git clone -q "$T/origin.git" "$T/w" 2>/dev/null
cd "$T/w" || exit 1
git config user.email t@t; git config user.name t
mkdir -p tools/ci src
cp "$ROOT/tools/ci/safe_merge.sh" "$ROOT/tools/ci/merge_freshness.sh" "$ROOT/tools/ci/merge_heavy_gates.py" tools/ci/
echo base > src/a.c; echo x > src/b.c
git add -A && git commit -qm base && git branch -M main && git push -q origin main
git checkout -q -b pr && echo change >> src/a.c && git commit -qam pr && git push -q origin pr
PRSHA=$(git rev-parse HEAD)

mkdir -p "$T/bin"
cat > "$T/bin/gh" <<STUB
#!/bin/sh
case "\$1 \$2" in
"pr view")
  case "\$*" in
    *statusCheckRollup*) if [ "\${ROLLUP:-green}" = red ]; then
        echo '{"headRefOid":"$PRSHA","headRefName":"pr","statusCheckRollup":[{"name":"Build & Test","workflowName":"CI","conclusion":"FAILURE"}]}'
      else
        echo '{"headRefOid":"$PRSHA","headRefName":"pr","statusCheckRollup":[{"name":"Build & Test","workflowName":"CI","conclusion":"SUCCESS"}]}'
      fi ;;
    *) echo "$PRSHA" ;;
  esac ;;
*) echo "stub gh: \$*" >&2; exit 9 ;;
esac
STUB
chmod +x "$T/bin/gh"
# the stub prints the bare sha for --jq .headRefOid calls
sm() { PATH="$T/bin:$PATH" sh tools/ci/safe_merge.sh "$@" >"$T/out" 2>&1; echo $?; }

git checkout -q main
rc=$(sm 1 --dry-run)
[ "$rc" = 0 ] && ok "fresh, green PR passes (dry run)" || { bad "fresh green -> 0 (got $rc)"; cat "$T/out"; }

rc=$(ROLLUP=red sm 1 --dry-run)
[ "$rc" = 1 ] && grep -q "REFUSED: PR checks not green" "$T/out" && ok "a failed PR check is refused" || bad "red rollup -> refused (got $rc)"

echo mainchange >> src/a.c && git commit -qam "main touches a.c" && git push -q origin main
rc=$(sm 1 --dry-run)
[ "$rc" = 1 ] && grep -q "REFUSED: not fresh" "$T/out" && ok "a STALE PR is refused (main changed a file it changes)" || bad "stale -> refused (got $rc)"

rc=$(sm --dry-run)
[ "$rc" = 2 ] && ok "usage error -> 2" || bad "usage -> 2 (got $rc)"

echo "safe_merge: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
