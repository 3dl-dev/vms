#!/bin/sh
# test_ci_tiers.sh <repo-root> -- tools/ci/ci_tiers.py decides the heavy CI
# tiers correctly, the negctl path->facility map FAILS CLOSED, and
# tools/ci/pre_tag_verify.py refuses a SHA with no green full run. No network.
set -u
ROOT=${1:?usage: test_ci_tiers.sh <repo-root>}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "  PASS: $1"; }
bad() { fail=$((fail+1)); echo "  FAIL: $1"; }
tier() { # event ref files... -> prints outputs
    ev=$1; ref=$2; shift 2
    : > "$T/files"; for f in "$@"; do echo "$f" >> "$T/files"; done
    python3 "$ROOT/tools/ci/ci_tiers.py" --repo "$ROOT" --event "$ev" --ref "$ref" --files "$T/files"
}
get() { echo "$1" | sed -n "s/^$2=//p"; }

# full events run everything, ignoring the file list
for ev in schedule workflow_dispatch; do
    o=$(tier $ev refs/heads/main docs/x.md)
    [ "$(get "$o" heavy_release)" = true ] && [ "$(get "$o" keystroke)" = true ] \
      && [ "$(get "$o" alpha_selfhost_tier)" = true ] && [ "$(get "$o" negctl_select)" = ALL ] \
      && ok "$ev runs every heavy tier" || bad "$ev full"
done
o=$(tier push refs/tags/V0.9 docs/x.md)
[ "$(get "$o" negctl_select)" = ALL ] && [ "$(get "$o" heavy_release)" = true ] \
  && ok "a release-tag push is a full run (pre-tag verify)" || bad "tag push full"

# a push to a branch runs no heavy tier
o=$(tier push refs/heads/main src/kernel-core/vms_access.c)
[ "$(get "$o" heavy_release)" = false ] && [ -z "$(get "$o" negctl_select)" ] && [ "$(get "$o" keystroke)" = false ] \
  && ok "push to main drops the heavy legs" || bad "push none"

# a pull_request runs no heavy tier
o=$(tier pull_request refs/pull/1/merge tools/cut-release.sh src/kernel-core/vms_access.c)
[ "$(get "$o" heavy_release)" = false ] && [ -z "$(get "$o" negctl_select)" ] && [ "$(get "$o" alpha_selfhost_tier)" = false ] \
  && ok "PRs run no heavy tier" || bad "pr none"

# merge queue: docs-only touches nothing heavy
o=$(tier merge_group refs/heads/gh-readonly-queue/main/x docs/foo.md README.md)
[ "$(get "$o" heavy_release)" = false ] && [ -z "$(get "$o" negctl_select)" ] && [ "$(get "$o" keystroke)" = false ] \
  && ok "queue: docs-only change runs no heavy leg" || { bad "mq docs-only"; echo "$o"; }

# merge queue: release tooling pulls in the release tier only
o=$(tier merge_group q tools/cut-release.sh)
[ "$(get "$o" heavy_release)" = true ] && [ "$(get "$o" keystroke)" = false ] \
  && ok "queue: release tooling change runs the release tier" || bad "mq release"

# merge queue: a defect target selects only that facility's defects
o=$(tier merge_group q src/kernel-core/vms_access.c)
sel=$(get "$o" negctl_select)
case "$sel" in ALL|"") bad "mq target -> subset (got '$sel')" ;;
  *access-mode-escalation*) ok "queue: a mapped executive source selects only its facility's defects" ;;
  *) bad "mq target subset content" ;; esac
n=$(echo "$sel" | wc -w); total=$(sh "$ROOT/tests/qemu/facility_defects.sh" list | wc -l)
[ "$n" -gt 0 ] && [ "$n" -lt "$total" ] && ok "queue: subset ($n) is smaller than the manifest ($total)" || bad "subset size"

# merge queue: FAIL CLOSED -- any unmapped path selects ALL defects
for p in src/vmsdcl/not_a_target_anywhere.c src/kernel-core/does_not_exist_anywhere.c CMakeLists.txt tests/qemu/Dockerfile unknown/new/path.txt; do
    o=$(tier merge_group q docs/a.md "$p")
    [ "$(get "$o" negctl_select)" = ALL ] && ok "fail closed: unmapped $p -> ALL" || bad "fail closed $p"
done

# the mapped test source selects defects via suites_red
o=$(tier merge_group q tests/qemu/test_kmod_access.c)
case "$(get "$o" negctl_select)" in *access-mode-escalation*) ok "queue: a suite source selects the defects that redden it" ;; *) bad "suite source map" ;; esac

# decnet tier (ci-vax vax-decnet-pair, rd vms-9394): queue only when touched
o=$(tier merge_group q src/libdatalink/ovmx_datalink.c)
[ "$(get "$o" decnet)" = true ] && ok "queue: a datalink change runs the DECnet pair" || bad "decnet area"
o=$(tier merge_group q src/vmsdecnet/engine/decnetd.c)
[ "$(get "$o" decnet)" = true ] && ok "queue: a NETACP change runs the DECnet pair" || bad "decnet netacp"
o=$(tier merge_group q docs/foo.md src/kernel-core/vms_access.c)
[ "$(get "$o" decnet)" = false ] && ok "queue: an unrelated change does not run the DECnet pair" || bad "decnet unrelated"
o=$(tier pull_request refs/pull/1/merge src/vmsdecnet/engine/decnetd.c)
[ "$(get "$o" decnet)" = false ] && ok "PRs never run the DECnet pair" || bad "decnet pr"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" decnet)" = true ] && ok "nightly runs the DECnet pair" || bad "decnet nightly"

# keystroke / alpha selfhost area
o=$(tier merge_group q src/kernel-core/vms_tt.c)
[ "$(get "$o" keystroke)" = true ] && ok "queue: terminal driver change runs the keystroke oracle" || bad "keystroke area"
o=$(tier merge_group q tools/cross-alpha-vms/Dockerfile)
[ "$(get "$o" alpha_selfhost_tier)" = true ] && ok "queue: alpha cross-toolchain change runs the cc1/C++ self-host" || bad "alpha area"

# failing to compute the changed files fails closed in the queue
o=$(python3 "$ROOT/tools/ci/ci_tiers.py" --repo "$T" --event merge_group --ref x --base deadbeef 2>/dev/null)
[ "$(get "$o" negctl_select)" = ALL ] && [ "$(get "$o" heavy_release)" = true ] \
  && ok "queue: git failure fails closed to the full suite" || bad "git failure fail-closed"

# pre_tag_verify: red without a green full run on the SHA, green with all six
python3 - "$ROOT" <<'PY' && ok "pre_tag_verify check: missing -> 1, all green -> 0, push/PR runs do not count" || bad "pre_tag_verify"
import sys
sys.path.insert(0, sys.argv[1] + "/tools/ci")
import pre_tag_verify as p
sha = "a" * 40
def run(event, concl, s=sha): return {"headSha": s, "event": event, "conclusion": concl, "url": "u"}
assert p.check(sha, fetch=lambda wf, r: []) == 1
assert p.check(sha, fetch=lambda wf, r: [run("push", "success"), run("pull_request", "success")]) == 1
assert p.check(sha, fetch=lambda wf, r: [run("schedule", "failure")]) == 1
assert p.check(sha, fetch=lambda wf, r: [run("schedule", "success", "b" * 40)]) == 1
assert p.check(sha, fetch=lambda wf, r: [run("workflow_dispatch", "success")]) == 0
PY

echo "ci_tiers: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
