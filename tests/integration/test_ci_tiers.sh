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
o=$(tier merge_group q docs/foo.md src/kernel-core/vms_access.c src/vmsdecnet/engine/decnetd.c)
[ "$(get "$o" vax_boot)" = false ] && ok "queue: an unrelated change does not boot the VAX" || bad "vax_boot unrelated"
o=$(tier pull_request refs/pull/1/merge tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_boot)" = false ] && ok "vax_boot is a queue tier (PRs use ci-changes vax_login)" || bad "vax_boot pr"
o=$(tier push refs/heads/main tools/cross-vax/build-vmsdcl-vax.sh)
[ "$(get "$o" vax_boot)" = false ] && ok "push to main does not re-boot the VAX" || bad "vax_boot push"
o=$(tier schedule refs/heads/main docs/x.md)
[ "$(get "$o" vax_boot)" = true ] && ok "nightly runs the VAX battery" || bad "vax_boot nightly"

# vax_acceptance tier (ci-vax vax-dcl-acceptance, rd vms-365): the full-memory
# VAX battery is the only job that runs the battery's checks on the VAX (the 16 MB
# leg stops after login); #1940 changed one with DECNETD and the queue never ran it.
o=$(tier merge_group q tests/qemu/lib/dcl_acceptance_battery.sh src/vmsdecnet/engine/decnetd.c)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: #1940's change set (battery + DECNETD) runs the full-memory VAX battery" || bad "vax_acceptance 1940"
o=$(tier merge_group q tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a battery change runs the full-memory VAX battery" || bad "vax_acceptance battery"
o=$(tier merge_group q src/vmsdecnet/engine/decnetd.c)
[ "$(get "$o" vax_acceptance)" = true ] && ok "queue: a DECNETD/NETACP change runs the full-memory VAX battery" || bad "vax_acceptance netacp"
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
o=$(tier pull_request refs/pull/1/merge src/vmsdecnet/engine/decnetd.c tests/qemu/lib/dcl_acceptance_battery.sh)
[ "$(get "$o" vax_acceptance)" = false ] && ok "vax_acceptance is a queue tier (not on PRs)" || bad "vax_acceptance pr"
o=$(tier push refs/heads/main src/vmsdecnet/engine/decnetd.c)
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
o=$(tier merge_group q docs/foo.md src/vmsdecnet/engine/decnetd.c)
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
o=$(tier merge_group q docs/foo.md src/vmsdecnet/engine/decnetd.c)
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
