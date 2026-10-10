#!/bin/sh
# test_merge_heavy_gates.sh <repo-root> -- tools/ci/merge_heavy_gates.py fails a
# PR whose SKIPPED heavy checks were never run green on its head SHA (the #1554
# shape: VMS User Acceptance Test skipped on the PR, red on main), and passes
# one whose skipped workflows all passed in a workflow_dispatch run on that SHA.
# A stub `gh` on PATH serves canned JSON; no network.
set -u
ROOT=${1:?usage: test_merge_heavy_gates.sh <repo-root>}
TOOL="$ROOT/tools/ci/merge_heavy_gates.py"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }

mkdir -p "$T/bin"
cat > "$T/bin/gh" <<'STUB'
#!/bin/sh
# canned answers selected by $SCENARIO
case "$1 $2" in
"pr view")
  cat <<J
{"headRefOid":"abc123def456","headRefName":"work/x","statusCheckRollup":[
 {"name":"Build & Test","workflowName":"CI / Core Gates","conclusion":"SUCCESS"},
 {"name":"VMS User Acceptance Test","workflowName":"CI / Core Gates","conclusion":"SKIPPED"},
 {"name":"Persistent Boot Smoke Test","workflowName":"CI / Release / E2E","conclusion":"SKIPPED"}]}
J
  ;;
"run list")
  wf=$4
  case "$SCENARIO:$wf" in
  none:*) echo '[]' ;;
  green:*) echo '[{"databaseId":2,"status":"completed","conclusion":"success","createdAt":"2026-10-09T02:00:00Z"},{"databaseId":1,"status":"completed","conclusion":"failure","createdAt":"2026-10-09T01:00:00Z"}]' ;;
  red:"CI / Core Gates") echo '[{"databaseId":3,"status":"completed","conclusion":"failure","createdAt":"2026-10-09T03:00:00Z"},{"databaseId":2,"status":"completed","conclusion":"success","createdAt":"2026-10-09T02:00:00Z"}]' ;;
  red:*) echo '[{"databaseId":4,"status":"completed","conclusion":"success","createdAt":"2026-10-09T02:00:00Z"}]' ;;
  cancelled:*) echo '[{"databaseId":5,"status":"completed","conclusion":"cancelled","createdAt":"2026-10-09T04:00:00Z"},{"databaseId":4,"status":"completed","conclusion":"success","createdAt":"2026-10-09T02:00:00Z"}]' ;;
  esac ;;
"run view")
  echo '{"jobs":[{"name":"VMS User Acceptance Test","conclusion":"failure"},{"name":"Build & Test","conclusion":"success"}]}' ;;
*) echo "stub gh: unexpected $*" >&2; exit 9 ;;
esac
STUB
chmod +x "$T/bin/gh"

run() { s=$1; shift; SCENARIO=$s PATH="$T/bin:$PATH" python3 "$TOOL" 1554 "$@" >"$T/out" 2>&1; echo $?; }

rc=$(run none)
[ "$rc" = 1 ] && ok "skipped heavy workflows never dispatched on the head -> FAIL (exit 1)" || bad "no dispatch run -> exit 1 (got $rc)"
grep -q "CI / Core Gates: no finished workflow_dispatch run" "$T/out" && ok "names the workflow and what to run" || bad "names the workflow"

rc=$(run red)
[ "$rc" = 1 ] && ok "latest dispatch run of a skipped workflow red -> FAIL (an older green does not count)" || bad "latest red -> exit 1 (got $rc)"
grep -q "CI / Core Gates: latest dispatch run 3 on abc123def concluded failure; unmapped job(s): VMS User Acceptance Test" "$T/out" && ok "names the red run and its unmapped job" || bad "names the red run"

rc=$(run red --allow "CI / Core Gates::User Acceptance::#1597" --table "$T/tbl")
[ "$rc" = 0 ] && ok "a red whose every failed job is mapped to a named fix passes" || bad "mapped red -> 0 (got $rc)"
grep -q "fixed by #1597" "$T/tbl" && ok "the mapping table names the fix" || bad "mapping table"

rc=$(run red --allow "CI / Alpha::User Acceptance::#1597")
[ "$rc" = 1 ] && ok "a mapping for another workflow does not cover this red" || bad "wrong-workflow map -> 1 (got $rc)"

rc=$(run red --allow "CI / Core Gates::User Acceptance::")
[ "$rc" = 2 ] && ok "a mapping that names no fix is refused (usage)" || bad "empty fixed-by -> 2 (got $rc)"

rc=$(run green)
[ "$rc" = 0 ] && ok "every skipped workflow's latest dispatch run on the head is green -> OK" || bad "green -> exit 0 (got $rc)"

rc=$(run cancelled)
[ "$rc" = 0 ] && ok "a cancelled later run is ignored, the finished green one counts" || bad "cancelled ignored (got $rc)"

rc=$(PATH="$T/bin:$PATH" python3 "$TOOL" >/dev/null 2>&1; echo $?)
[ "$rc" = 2 ] && ok "usage error -> exit 2" || bad "usage -> 2 (got $rc)"

echo "merge_heavy_gates: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
