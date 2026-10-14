#!/bin/bash
# test_drift_sentinel.sh -- the deploy drift sentinel can pass AND can fail
# (rd vms-fd8, epic vms-d385). Offline: a fixture stands in for the network.
#
#   1. a manifest covering every probe kind, all current      -> exit 0
#   2. NEGATIVE CONTROL: one fake stale probe                   -> exit 1, names it
#   3. a probe that cannot read a version (HTTP 404)            -> exit 1 (NOVER)
#   4. a gated target held by its pin still reports drift       -> exit 1, quotes the pin
#   5. an arch the release does not ship                        -> exit 1
#   6. a bad manifest (duplicate id, missing probe)             -> exit 2
#   7. the real deploy/targets.yaml validates
set -u
ROOT=${1:-$(cd "$(dirname "$0")/../.." && pwd)}
S="python3 $ROOT/tools/deploy/drift_sentinel.py"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
ok()  { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }

cat > "$T/m.yaml" <<'EOF'
targets:
  - {id: asset, rd: x, build: {}, deploy: {}, probe: {kind: release_asset, pattern: "DCL-VAX.EXE"}}
  - {id: label, rd: x, build: {}, deploy: {}, probe: {kind: regex, url: "http://f/", pattern: "data-v>([^<]+)<"}}
  - {id: tagfile, rd: x, build: {}, deploy: {}, gated: true, pin: "http://f/pin.json", probe: {kind: text, url: "http://f/TAG"}}
  - {id: data, rd: x, build: {}, deploy: {}, probe: {kind: json, url: "http://f/d.json", path: "releases.0.tag"}}
  - {id: disk, rd: x, build: {}, deploy: {}, probe: {kind: regex, url: "http://f/", pattern: "img-(v[0-9.-]+)\\.gz"}}
  - {id: pinned-vol, rd: x, tracks: none, build: {}, deploy: {}, probe: {kind: served, url: "http://f/vol"}}
EOF
fx() {  # fx <label> <tagfile> <disk> <assets-json> <tagfile-status>
  cat > "$T/fx.json" <<EOF
{"urls": {
  "http://f/": {"status": 200, "body": "<i data-v>$1</i> <a href=img-$3.gz>"},
  "http://f/TAG": {"status": ${5:-200}, "body": "$2\n"},
  "http://f/pin.json": {"status": 200, "body": "{\"reason\": \"visitor gate failed for V0.7-10\"}"},
  "http://f/d.json": {"status": 200, "body": "{\"releases\": [{\"tag\": \"V0.7-10\"}]}"},
  "http://f/vol": {"status": 200, "body": "x"}},
 "release_assets": $4}
EOF
}
run() { $S --manifest "$T/m.yaml" --latest V0.7-10 --fixture "$T/fx.json" > "$T/out" 2>&1; echo $?; }

fx V0.7-10 V0.7-10 v0.7-10 '["DCL-VAX.EXE"]'
rc=$(run); [ "$rc" = 0 ] && grep -q "all 6 targets on V0.7-10" "$T/out" \
  && ok "every probe kind reads a current target as current" || { bad "all-current manifest did not pass (rc=$rc)"; cat "$T/out"; }

fx V0.7-10 V0.7-10 v0.7-5 '["DCL-VAX.EXE"]'
rc=$(run); [ "$rc" = 1 ] && grep -q "\[DRIFT\] disk: live=v0.7-5" "$T/out" && grep -q "1 of 6" "$T/out" \
  && ok "negative control: one stale probe turns the sentinel red and names that target" \
  || { bad "a stale probe did not go red (rc=$rc)"; cat "$T/out"; }

fx V0.7-10 V0.7-10 v0.7-10 '["DCL-VAX.EXE"]' 404
rc=$(run); [ "$rc" = 1 ] && grep -q "\[NOVER\] tagfile" "$T/out" \
  && ok "a probe that reads no version is drift, not silence" || { bad "unreadable probe passed (rc=$rc)"; cat "$T/out"; }

fx V0.7-10 V0.7-9 v0.7-10 '["DCL-VAX.EXE"]'
rc=$(run); [ "$rc" = 1 ] && grep -q "\[DRIFT\] tagfile: live=V0.7-9" "$T/out" && grep -q "pinned: visitor gate failed" "$T/out" \
  && ok "a gated target held back by its pin still reports drift, with the pin's reason" \
  || { bad "pinned target hid its drift (rc=$rc)"; cat "$T/out"; }

fx V0.7-10 V0.7-10 v0.7-10 '["DCL-ALPHA.EXE"]'
rc=$(run); [ "$rc" = 1 ] && grep -q "\[NOVER\] asset" "$T/out" \
  && ok "an arch the release does not ship is reported" || { bad "missing arch asset passed (rc=$rc)"; cat "$T/out"; }

printf 'targets:\n  - {id: a, rd: x, build: {}, deploy: {}, probe: {kind: text, url: "u"}}\n  - {id: a, rd: x, build: {}, deploy: {}}\n' > "$T/bad.yaml"
$S --manifest "$T/bad.yaml" --latest V0.7-10 --fixture "$T/fx.json" > "$T/out" 2>&1; rc=$?
[ "$rc" = 2 ] && grep -q "duplicate id" "$T/out" && grep -q "missing 'probe'" "$T/out" \
  && ok "a malformed manifest is refused" || { bad "malformed manifest accepted (rc=$rc)"; cat "$T/out"; }

echo '{"urls": {}, "release_assets": []}' > "$T/empty.json"
$S --manifest "$ROOT/deploy/targets.yaml" --latest V0.7-10 --fixture "$T/empty.json" > "$T/out" 2>&1; rc=$?
[ "$rc" = 1 ] && ! grep -q "^manifest:" "$T/out" \
  && ok "deploy/targets.yaml validates (and with no network every release target is unproven)" \
  || { bad "deploy/targets.yaml did not validate (rc=$rc)"; cat "$T/out"; }

[ "$fail" = 0 ] && echo "PASS: drift sentinel can pass and can fail" || exit 1
