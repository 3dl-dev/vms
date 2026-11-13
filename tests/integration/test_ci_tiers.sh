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

# a QEMU suite header selects what the suites including it select; anything
# else including it, or nothing including it, fails closed. Fixture tree (its
# own git index, so the map reads only this):
#   mapped.h   <- test_a.c (defect da), and via mid.h <- test_b.c (defect db)
#   lonely.h   <- test_c.c, which no defect names
#   fixture.h  <- test_a.c AND mkimage_x.c (a fixture builder)
#   outside.h  <- test_a.c AND tools/cross-x/init.c (outside tests/qemu)
#   orphan.h   <- nothing
H="$T/hdr"; mkdir -p "$H/tests/qemu/defects.d" "$H/tools/cross-x" "$H/src/x"
for d in da:test_a db:test_b; do
    printf 'defect_meta() {\n        case "$_f" in\n        targets)      echo "x/%s.c";;\n        suites_red)   echo "%s";;\n        esac\n}\n' \
        "${d%%:*}" "${d#*:}" > "$H/tests/qemu/defects.d/${d%%:*}.defect"
done
q="$H/tests/qemu"
printf '#include "ovmx_x.h"\n#include "mapped.h"\n#  include "fixture.h"\n#include <outside.h>\n' > "$q/test_a.c"
printf '#include "mid.h"\n' > "$q/test_b.c"
printf '#include "lonely.h"\n' > "$q/test_c.c"
printf '#include "mapped.h"\n' > "$q/mid.h"
printf '/* mentions orphan.h and lonely.h in a comment only */\n' > "$q/mapped.h"
for h in lonely fixture outside orphan ovmx_x; do : > "$q/$h.h"; done
printf '#include "fixture.h"\n' > "$q/mkimage_x.c"
printf '#include "../../tests/qemu/outside.h"\n' > "$H/tools/cross-x/init.c"
if git -C "$H" init -q && git -C "$H" add -A; then
    htier() { : > "$T/files"; for f in "$@"; do echo "$f" >> "$T/files"; done
              python3 "$ROOT/tools/ci/ci_tiers.py" --repo "$H" --event merge_group --ref q --files "$T/files"; }
    o=$(htier tests/qemu/mapped.h)
    [ "$(get "$o" negctl_select)" = "da db" ] \
      && ok "queue: a suite header selects the defects of every suite including it, through another header too" \
      || bad "suite header map (got '$(get "$o" negctl_select)')"
    o=$(htier tests/qemu/mid.h)
    [ "$(get "$o" negctl_select)" = "db" ] && ok "queue: an intermediate header selects only its own includers' defects" \
      || bad "intermediate header (got '$(get "$o" negctl_select)')"
    o=$(htier tests/qemu/ovmx_x.h tests/qemu/test_b.c)
    [ "$(get "$o" negctl_select)" = "da db" ] && ok "queue: a header plus a suite source select the union" \
      || bad "header + suite union (got '$(get "$o" negctl_select)')"
    for h in lonely fixture outside orphan nosuch; do
        o=$(htier tests/qemu/$h.h)
        [ "$(get "$o" negctl_select)" = ALL ] && ok "fail closed: header $h.h -> ALL" || bad "fail closed header $h.h (got '$(get "$o" negctl_select)')"
    done
    o=$(htier tests/qemu/sub/deeper.h)
    [ "$(get "$o" negctl_select)" = ALL ] && ok "fail closed: a header below tests/qemu/ is not a suite header -> ALL" || bad "nested header"
else
    bad "cannot build the header-map fixture git index"
fi
# the includers cannot be read (the defect table can, but it is no git tree)
mkdir -p "$T/nogit/tests/qemu" && cp -r "$H/tests/qemu/defects.d" "$T/nogit/tests/qemu/"
echo tests/qemu/mapped.h > "$T/files"
o=$(python3 "$ROOT/tools/ci/ci_tiers.py" --repo "$T/nogit" --event merge_group --ref q --files "$T/files" 2>/dev/null)
[ "$(get "$o" negctl_select)" = ALL ] && ok "queue: a header map git cannot answer fails closed" || bad "header map failure"

# decnet tier (ci-vax vax-decnet-pair, rd vms-9394): queue only when touched
o=$(tier merge_group q src/libdatalink/ovmx_datalink.c)
[ "$(get "$o" decnet)" = true ] && ok "queue: a datalink change runs the DECnet pair" || bad "decnet area"
o=$(tier merge_group q src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" decnet)" = true ] && ok "queue: a NETACP change runs the DECnet pair" || bad "decnet netacp"
o=$(tier merge_group q docs/foo.md src/kernel-core/vms_access.c)
[ "$(get "$o" decnet)" = false ] && ok "queue: an unrelated change does not run the DECnet pair" || bad "decnet unrelated"
o=$(tier pull_request refs/pull/1/merge src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" decnet)" = false ] && ok "PRs never run the DECnet pair" || bad "decnet pr"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" decnet)" = true ] && ok "nightly runs the DECnet pair" || bad "decnet nightly"

# vax_boot tier (ci-vax vax-acceptance-16mb + vax-sysboot-single, rd vms-236):
# the demo-disk battery runs in the queue when the VAX boot image or the battery
# is touched -- the two changes that reached main with every demo build red.
o=$(tier merge_group q tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_boot)" = true ] && ok "queue: a battery change runs the VAX demo-disk battery (#1908)" || bad "vax_boot battery"
o=$(tier merge_group q tools/oracle/surfaces/vax-show-process.surface)
[ "$(get "$o" vax_boot)" = true ] && ok "queue: an oracle surface change runs the VAX battery" || bad "vax_boot surface"
o=$(tier merge_group q docs/oracle/golden/vax-show-process.txt)
[ "$(get "$o" vax_boot)" = true ] && ok "queue: a golden change runs the VAX battery" || bad "vax_boot golden"
o=$(tier merge_group q tools/cross-vax/build-vmsdcl-vax.sh)
[ "$(get "$o" vax_boot)" = true ] && ok "queue: a VAX image link-script change runs the VAX battery (#1913)" || bad "vax_boot cross-vax"
o=$(tier merge_group q src/ovmx_opcom/ovmx_opcom.c)
[ "$(get "$o" vax_boot)" = true ] && ok "queue: a boot-process change runs the VAX battery (#1850)" || bad "vax_boot opcom"
o=$(tier merge_group q docs/foo.md src/kernel-core/vms_access.c src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" vax_boot)" = false ] && ok "queue: an unrelated change does not boot the VAX" || bad "vax_boot unrelated"
o=$(tier pull_request refs/pull/1/merge tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_boot)" = false ] && ok "vax_boot is a queue tier (PRs use ci-changes vax_login)" || bad "vax_boot pr"
o=$(tier push refs/heads/main tools/cross-vax/build-vmsdcl-vax.sh)
[ "$(get "$o" vax_boot)" = false ] && ok "push to main does not re-boot the VAX" || bad "vax_boot push"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" vax_boot)" = true ] && ok "nightly runs the VAX battery" || bad "vax_boot nightly"

# vax_acceptance tier (ci-vax vax-dcl-acceptance, rd vms-365): the full-memory
# VAX battery is the only job that runs the battery's checks on the VAX (the 16 MB
# leg stops after login); #1940 changed one with NETACP and the queue never ran it.
o=$(tier merge_group q tests/qemu/lib/dcl_acceptance_battery.sh src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: #1940's change set (battery + NETACP) runs the full-memory VAX battery" || bad "vax_acceptance 1940"
o=$(tier merge_group q tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a battery change runs the full-memory VAX battery" || bad "vax_acceptance battery"
o=$(tier merge_group q src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a NETACP change runs the full-memory VAX battery" || bad "vax_acceptance netacp"
o=$(tier merge_group q distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/STARTNET.COM)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a NETACP startup change runs the full-memory VAX battery" || bad "vax_acceptance startnet"
o=$(tier merge_group q docs/oracle/golden/vax-show-process.txt)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a golden change runs the full-memory VAX battery" || bad "vax_acceptance golden"
o=$(tier merge_group q tests/lab-vax/test_dcl_acceptance_vax.sh)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: the battery's VAX driver change runs it" || bad "vax_acceptance driver"
o=$(tier merge_group q src/kernel-netbsd/vms_netbsd.c)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a NetBSD executive-glue change runs the full-memory VAX battery (rd vms-1b6a: its empty-record drop reached main unrun)" || bad "vax_acceptance kernel-netbsd"
o=$(tier merge_group q docs/foo.md src/kernel-core/vms_access.c src/ovmx_opcom/ovmx_opcom.c)
[ "$(get "$o" vax_acceptance)" = false ] && ok "queue: an unrelated change does not run the full-memory VAX battery" || bad "vax_acceptance unrelated"
o=$(tier pull_request refs/pull/1/merge src/vmsdecnet/engine/netacp.c tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_acceptance)" = false ] && ok "vax_acceptance is a queue tier (not on PRs)" || bad "vax_acceptance pr"
o=$(tier push refs/heads/main src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" vax_acceptance)" = false ] && ok "push to main does not re-run the full-memory VAX battery" || bad "vax_acceptance push"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" vax_acceptance)" = true ] && ok "nightly runs the full-memory VAX battery" || bad "vax_acceptance nightly"
o=$(tier push refs/tags/V0.9 docs/x.md)
[ "$(get "$o" vax_acceptance)" = true ] && ok "a release-tag push runs the full-memory VAX battery" || bad "vax_acceptance tag"
# the workflow wiring: ci-changes exports the output and the job reads it in merge_group
grep -q "vax_acceptance: \${{ steps.tiers.outputs.vax_acceptance }}" "$ROOT/.github/workflows/ci-changes.yml" \
  && grep -q "value: \${{ jobs.changes.outputs.vax_acceptance }}" "$ROOT/.github/workflows/ci-changes.yml" \
  && ok "ci-changes.yml exports vax_acceptance" || bad "ci-changes vax_acceptance output"
awk '/^  vax-dcl-acceptance:/{f=1} f&&/^    if:/{print; exit}' "$ROOT/.github/workflows/ci-vax.yml" \
  | grep -q "github.event_name == 'merge_group' && needs.changes.outputs.vax_acceptance == 'true'" \
  && ok "ci-vax vax-dcl-acceptance runs in the queue on vax_acceptance" || bad "ci-vax vax-dcl-acceptance merge_group condition"

# alpha_syssvc tier (ci-alpha alpha-syssvc-suite): the queue runs the Alpha sys$
# API suite when its own harness changes (#1893 broke the harness's quoting and
# the suite first ran on main).
o=$(tier merge_group q tools/cross-alpha/run-syssvc-tests-alpha.sh)
[ "$(get "$o" alpha_syssvc)" = true ] && ok "queue: a syssvc harness change runs the Alpha sys\$ suite (#1893)" || bad "alpha_syssvc harness"
o=$(tier merge_group q tools/cross-alpha/alpha-syssvc-baseline.txt)
[ "$(get "$o" alpha_syssvc)" = true ] && ok "queue: a baseline change runs the Alpha sys\$ suite" || bad "alpha_syssvc baseline"
o=$(tier merge_group q tests/qemu/test_syssvc_dcl_remote_syscall_audit.c)
[ "$(get "$o" alpha_syssvc)" = true ] && ok "queue: a ptrace syscall-audit suite change runs the Alpha sys\$ suite (vms-0a7)" || bad "alpha_syssvc audit suite"
o=$(tier merge_group q docs/foo.md src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" alpha_syssvc)" = false ] && ok "queue: an unrelated change does not run the Alpha sys\$ suite" || bad "alpha_syssvc unrelated"
o=$(tier push refs/heads/main tools/cross-alpha/run-syssvc-tests-alpha.sh)
[ "$(get "$o" alpha_syssvc)" = false ] && ok "alpha_syssvc tier is false on push (push always runs the suite)" || bad "alpha_syssvc push"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" alpha_syssvc)" = true ] && ok "nightly sets alpha_syssvc" || bad "alpha_syssvc nightly"

# tcpip_net tier (ci-release-e2e tcpip-daytime-boot-e2e, rd vms-d0c4): the booted
# TCP/IP proofs (inbound DAYTIME, reboot reapply, the external TELNET/FTP peer)
# run in the queue when the BGn: driver, the engines or the DCL verbs change.
o=$(tier merge_group q src/kernel-core/vms_bg.c)
[ "$(get "$o" tcpip_net)" = true ] && ok "queue: a BGn: driver change runs the booted TCP/IP proofs" || bad "tcpip_net bg"
o=$(tier merge_group q src/vmstcpip/services/tcpip_client.h)
[ "$(get "$o" tcpip_net)" = true ] && ok "queue: a TELNET/FTP engine change runs the booted TCP/IP proofs" || bad "tcpip_net engine"
o=$(tier merge_group q src/vmsdcl/dcl_cmd_misc.c)
[ "$(get "$o" tcpip_net)" = true ] && ok "queue: a TELNET/FTP verb change runs the booted TCP/IP proofs" || bad "tcpip_net verb"
o=$(tier merge_group q tests/qemu/test_tcpip_external_boot_e2e.sh)
[ "$(get "$o" tcpip_net)" = true ] && ok "queue: the external-peer e2e itself runs when changed" || bad "tcpip_net e2e"
o=$(tier merge_group q docs/foo.md src/vmsdecnet/engine/netacp.c)
[ "$(get "$o" tcpip_net)" = false ] && ok "queue: an unrelated change does not boot the TCP/IP proofs" || bad "tcpip_net unrelated"
o=$(tier pull_request refs/pull/1/merge src/kernel-core/vms_bg.c)
[ "$(get "$o" tcpip_net)" = false ] && ok "tcpip_net is a queue tier (PRs use ci-changes tcpip)" || bad "tcpip_net pr"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" tcpip_net)" = true ] && ok "nightly runs the booted TCP/IP proofs" || bad "tcpip_net nightly"
grep -q "tcpip_net: \${{ steps.tiers.outputs.tcpip_net }}" "$ROOT/.github/workflows/ci-changes.yml" \
  && grep -q "value: \${{ jobs.changes.outputs.tcpip_net }}" "$ROOT/.github/workflows/ci-changes.yml" \
  && ok "ci-changes.yml exports tcpip_net" || bad "ci-changes tcpip_net output"
awk '/^  tcpip-daytime-boot-e2e:/{f=1} f&&/^    if:/{p=1} p{print} p&&/}}/{exit}' "$ROOT/.github/workflows/ci-release-e2e.yml" \
  | grep -q "github.event_name == 'merge_group' && needs.changes.outputs.tcpip_net == 'true'" \
  && ok "ci-release-e2e tcpip-daytime-boot-e2e runs in the queue on tcpip_net" || bad "tcpip job merge_group condition"
grep -q "tests/qemu/run_tcpip_external_boot_e2e.sh" "$ROOT/.github/workflows/ci-release-e2e.yml" \
  && ok "the tcpip job runs the external-peer proof" || bad "external e2e not wired"

# keystroke / alpha selfhost area
o=$(tier merge_group q src/kernel-core/vms_tt.c)
[ "$(get "$o" keystroke)" = true ] && ok "queue: terminal driver change runs the keystroke oracle" || bad "keystroke area"
o=$(tier merge_group q tools/cross-alpha-vms/Dockerfile)
[ "$(get "$o" alpha_selfhost_tier)" = true ] && ok "queue: alpha cross-toolchain change runs the cc1/C++ self-host" || bad "alpha area"

# VMS-hosted GCC pipeline (alpha_vmsgcc): queue-when-touched, never on a PR,
# always on the full events
o=$(tier merge_group q src/vmsrms/rms_seq.c)
[ "$(get "$o" alpha_vmsgcc)" = true ] && ok "queue: an RMS change runs the VMS-hosted GCC pipeline" || bad "vmsgcc rms"
o=$(tier merge_group q src/vmsdcl/dcl_main.c)
[ "$(get "$o" alpha_vmsgcc)" = true ] && ok "queue: a DCL change runs the VMS-hosted GCC pipeline" || bad "vmsgcc dcl"
o=$(tier merge_group q src/kernel-core/vms_tt.c docs/x.md)
[ "$(get "$o" alpha_vmsgcc)" = false ] && ok "queue: an unrelated change skips the VMS-hosted GCC pipeline" || bad "vmsgcc unrelated"
o=$(tier pull_request x src/vmsrms/rms_seq.c)
[ "$(get "$o" alpha_vmsgcc)" = false ] && ok "PR: the VMS-hosted GCC pipeline is out of the PR path" || bad "vmsgcc pr"
for ev in schedule workflow_dispatch; do
    o=$(tier $ev refs/heads/main docs/x.md)
    [ "$(get "$o" alpha_vmsgcc)" = true ] && ok "$ev runs the VMS-hosted GCC pipeline" || bad "vmsgcc $ev"
done
o=$(python3 "$ROOT/tools/ci/ci_tiers.py" --repo "$T" --event merge_group --ref x --base deadbeef 2>/dev/null)
[ "$(get "$o" alpha_vmsgcc)" = true ] && ok "queue: git failure fails closed to the VMS-hosted GCC pipeline" || bad "vmsgcc fail-closed"

# negctl shard partition (tools/ci/negctl_shard.py): every defect on exactly
# one shard for any N, a defect the cost table lacks still runs, the CLI shard
# union equals the manifest, and the balance beats round-robin on the table.
python3 - "$ROOT" <<'PY' && ok "negctl_shard: complete + disjoint for any N, unknown defects still placed, LPT <= round-robin" || bad "negctl_shard partition"
import subprocess, sys
root = sys.argv[1]
sys.path.insert(0, root + "/tools/ci")
import negctl_shard as ns
names = subprocess.run(["sh", root + "/tests/qemu/facility_defects.sh", "list"],
                       capture_output=True, text=True, check=True).stdout.split()
assert len(names) > 100, len(names)
costs = ns.load_costs(root + "/tools/ci/negctl_costs.tsv")
for n in (1, 7, 40, 64, len(names) + 5):
    sh = ns.assign(names + ["zz-not-in-cost-table"], n, costs)
    flat = [x for s in sh for x in s]
    assert len(flat) == len(set(flat)) == len(names) + 1, n
    assert set(flat) == set(names) | {"zz-not-in-cost-table"}, n
med = sorted(costs.values())[len(costs) // 2]
c = lambda x: costs.get(x, med)
for n in (40, 64):
    lpt = max(sum(c(x) for x in s) for s in ns.assign(names, n, costs))
    rr = max(sum(c(x) for i, x in enumerate(sorted(names)) if i % n == k) for k in range(n))
    assert lpt <= rr, (n, lpt, rr)
# the CLI, as the workflow calls it: the union over all shards is the input
got = []
for i in range(5):
    out = subprocess.run([sys.executable, root + "/tools/ci/negctl_shard.py", "pick", "--index", str(i),
                          "--total", "5", "--costs", root + "/tools/ci/negctl_costs.tsv"],
                         input="\n".join(names), capture_output=True, text=True, check=True).stdout.split()
    got += out
assert sorted(got) == sorted(names)
r = subprocess.run([sys.executable, root + "/tools/ci/negctl_shard.py", "pick", "--index", "5", "--total", "5"],
                   input="a", capture_output=True, text=True)
assert r.returncode != 0
PY

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
