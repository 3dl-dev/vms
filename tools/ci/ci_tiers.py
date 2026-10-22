#!/usr/bin/env python3
"""ci_tiers.py -- which HEAVY CI legs does this event owe? (CI-speed tiering)

The heavy legs (release cuts + e2e, the per-facility executive negctl, the
Alpha cc1/C++ self-host jobs, the keystroke oracle) are out of the PR path.
They run:
  * on the schedule (nightly), on workflow_dispatch and on a release-tag push
    ("full" events: everything, no filtering -- the backstop and the pre-tag
    verify);
  * in the MERGE QUEUE only when the change touches their area (path filter);
  * never on a push to a branch (the queue already tested that exact tree).
On a pull_request only the cheap smoke tier runs (alpha-boot-mount etc.); all
heavy outputs are false.

usage: ci_tiers.py --event EVENT --ref REF [--base SHA] [--files FILE]
                   [--repo DIR]
Prints `key=value` lines (GITHUB_OUTPUT format):
  heavy_release   true|false
  alpha_selfhost_tier  true|false
  keystroke       true|false
  decnet          true|false
  negctl_select   ALL | (empty = none) | space-separated defect names

negctl_select is a PATH-TO-FACILITY map that FAILS CLOSED: a changed path that
is a defect's mutation target, or a test source one defect's `suites_red`
names, selects only those defects; a path on the explicit IGNORE list selects
nothing; EVERY OTHER PATH selects ALL defects. Any error computing the changed
files also yields ALL / true (fail closed).
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

# --------------------------------------------------------------------------
# Tier path filters (single source). `**` crosses directories, `*` does not.
# --------------------------------------------------------------------------
TIERS = {
    # The release-engineering surface (cut-release, upgrade, acceptance, the
    # aarch64 release set). Runs pre-merge only when release tooling, the
    # bootable Dockerfile or the release workflows change.
    "heavy_release": [
        ".github/workflows/release.yml",
        ".github/workflows/ci-release-e2e.yml",
        "tools/cut-release*.sh",
        "tools/publish-release.sh",
        "tools/gen_release_notes.py",
        "tools/check_guide_drift.py",
        "distro/Dockerfile.bootable",
        "tools/ovmx_kit_pack.c",
        "tools/vmsfs_master.c",
        "src/libvms/include/ovmx_identity.h",
        "src/libvms/include/ovmx_kit_format.h",
        "src/vmsfs/include/vmsfs_ondisk.h",
        "src/product/**",
        "tests/qemu/test_upgrade_e2e.sh",
        "tests/qemu/run_upgrade_e2e.sh",
        "tests/qemu/test_product_install_e2e.sh",
        "tests/qemu/run_product_install_e2e.sh",
        "tests/qemu/test_install_boot_e2e.sh",
        "tests/qemu/run_install_boot_e2e.sh",
        "tests/qemu/test_release_acceptance_e2e.sh",
        "tests/qemu/run_release_acceptance_e2e.sh",
        "tests/qemu/test_release_install.sh",
        "tests/qemu/run_release_install.sh",
        "tests/qemu/release_install_inner.sh",
        "docs/install-guide.md",
        "docs/upgrade-guide.md",
        "tools/ci/ci_tiers.py",
        ".github/workflows/ci-changes.yml",
    ],
    # Alpha cc1 / C++ self-host gates (hours of QEMU TCG).
    "alpha_selfhost_tier": [
        ".github/workflows/ci-alpha.yml",
        ".github/workflows/ci-changes.yml",
        "tools/cross-alpha-vms/**",
        "tools/cross-alpha/run-module-gp-activation-alpha.sh",
        "tools/ci/ci_tiers.py",
    ],
    # DECnet pair (ci-vax vax-decnet-pair, rd vms-9394): OVMX/VAX <-> OVMX/x86
    # SET HOST over NETACP on the executive datalink. Runs in the queue only
    # when DECnet, the raw-L2 datalink or NETACP's startup is touched.
    "decnet": [
        ".github/workflows/ci-vax.yml",
        ".github/workflows/ci-changes.yml",
        "src/vmsdecnet/**",
        "src/libdatalink/**",
        "src/kernel-core/vms_l2*",
        "src/kernel/vms_l2*",
        "src/kernel-netbsd/vms_socket_netbsd.c",
        "src/kernel-netbsd/vms_l2*",
        "distro/rootfs/vms/SYS0/SYSCOMMON/SYSMGR/STARTNET.COM",
        "tests/lab-vax/run-boot.sh",
        "tests/lab-vax/test_decnet_pair_vax.py",
        "tools/ci/ci_tiers.py",
    ],
    # Keystroke oracle: the console, terminal driver and boot path.
    "keystroke": [
        ".github/workflows/ci-release-e2e.yml",
        ".github/workflows/ci-changes.yml",
        "src/kernel-core/vms_tt*",
        "src/kernel/*tt*",
        "src/kernel/*term*",
        "src/ovmx_init/**",
        "src/vmsdcl/**",
        "src/libvms/include/sysgen_params.h",
        "tools/vms_login.c",
        "tools/loginout_display.c",
        "tools/oracle/keystroke/**",
        "docs/oracle/keystroke/**",
        "tests/qemu/test_keystroke_oracle.sh",
        "distro/**",
        "tools/ci/ci_tiers.py",
    ],
}

# Paths that cannot change what the per-facility executive negctl proves.
# Deliberately short: anything NOT here (and not a defect target / suite
# source) selects ALL defects -- the map fails closed.
NEGCTL_IGNORE = [
    "docs/**",
    "**/*.md",
    "tests/lab/**",
    "tests/lab-vax/**",
    "tests/lab-alpha/**",
    "tests/netbsd/**",
    "tests/qemu-aarch64/**",
    "tools/cross-*/**",
    ".github/workflows/ci-alpha.yml",
    ".github/workflows/ci-vax.yml",
    ".github/workflows/ci-selfhost-x86.yml",
    ".github/workflows/ci-core-gates.yml",
    ".github/workflows/ci-release-e2e.yml",
    ".github/workflows/ledger.yml",
    ".github/workflows/release.yml",
    ".github/workflows/drift-sentinel.yml",
    ".github/workflows/cluster-*.yml",
    ".github/workflows/mirror-base-images.yml",
    ".github/workflows/negctl-adhoc.yml",
    ".github/workflows/build-boot-artifacts.yml",
]

FULL_EVENTS = ("schedule", "workflow_dispatch")


def glob_re(pat):
    out = []
    i = 0
    while i < len(pat):
        c = pat[i]
        if pat.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
            continue
        if pat.startswith("**", i):
            out.append(".*")
            i += 2
            continue
        if c == "*":
            out.append("[^/]*")
        elif c == "?":
            out.append("[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return re.compile("^" + "".join(out) + "$")


def matches(path, pats):
    return any(glob_re(p).match(path) for p in pats)


def is_full(event, ref):
    return event in FULL_EVENTS or (event == "push" and ref.startswith("refs/tags/"))


def defect_table(repo):
    """{defect: (targets, suites_red)} parsed from tests/qemu/defects.d/*.defect
    (one process; the manifest's own `field` subcommand costs ~1s per call)."""
    d = os.path.join(repo, "tests/qemu/defects.d")
    table = {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".defect"):
            continue
        txt = open(os.path.join(d, fn)).read()
        def field(name):
            m = re.search(r'^\s*' + name + r'\)\s+echo "([^"]*)"', txt, re.M)
            return m.group(1).split() if m else []
        t, sr = field("targets"), field("suites_red")
        if not t:
            raise ValueError("%s: no targets field" % fn)
        table[fn[:-len(".defect")]] = (t, sr)
    if not table:
        raise ValueError("no defects found")
    return table


def negctl_select(files, table):
    """'ALL', '' (none) or space-separated defect names. Fails closed."""
    chosen = set()
    for f in files:
        if matches(f, NEGCTL_IGNORE):
            continue
        hit = False
        for d, (targets, suites) in table.items():
            if f in ["src/" + t for t in targets]:
                chosen.add(d)
                hit = True
            m = re.match(r"^tests/qemu/(test_[A-Za-z0-9_]+)\.c$", f)
            if m and any(fnmatch.fnmatchcase(m.group(1), g) for g in suites):
                chosen.add(d)
                hit = True
        if not hit:
            return "ALL"  # unmapped path: fail closed
    return " ".join(sorted(chosen))


def changed_files(repo, base):
    if base:
        subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", base],
                       cwd=repo, check=False, capture_output=True)
        rng = [base, "HEAD"]
    else:
        rng = ["HEAD^1", "HEAD"]
    out = subprocess.run(["git", "diff", "--name-only"] + rng, cwd=repo, check=True,
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if l]


def decide(event, ref, files, table_fn):
    res = {"heavy_release": "false", "alpha_selfhost_tier": "false",
           "keystroke": "false", "decnet": "false", "negctl_select": ""}
    if is_full(event, ref):
        return {"heavy_release": "true", "alpha_selfhost_tier": "true",
                "keystroke": "true", "decnet": "true", "negctl_select": "ALL"}
    if event == "push":
        return res  # the queue already tested this exact tree
    if files is None:  # could not compute: fail closed (queue / PR only)
        if event == "merge_group":
            return {"heavy_release": "true", "alpha_selfhost_tier": "true",
                    "keystroke": "true", "decnet": "true", "negctl_select": "ALL"}
        return res
    if event == "merge_group":
        res["alpha_selfhost_tier"] = "true" if matches_any(files, TIERS["alpha_selfhost_tier"]) else "false"
        res["heavy_release"] = "true" if matches_any(files, TIERS["heavy_release"]) else "false"
        res["keystroke"] = "true" if matches_any(files, TIERS["keystroke"]) else "false"
        res["decnet"] = "true" if matches_any(files, TIERS["decnet"]) else "false"
        try:
            res["negctl_select"] = negctl_select(files, table_fn())
        except Exception as e:  # noqa: BLE001 -- fail closed
            print("ci_tiers: negctl map failed (%s); selecting ALL" % e, file=sys.stderr)
            res["negctl_select"] = "ALL"
    return res


def matches_any(files, pats):
    return any(matches(f, pats) for f in files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--event", required=True)
    ap.add_argument("--ref", default="")
    ap.add_argument("--base", default="")
    ap.add_argument("--files", help="file with one changed path per line (tests)")
    ap.add_argument("--repo", default=".")
    a = ap.parse_args()
    files = None
    try:
        if a.files:
            files = [l.strip() for l in open(a.files) if l.strip()]
        elif a.event == "merge_group":
            files = changed_files(a.repo, a.base)
    except Exception as e:  # noqa: BLE001 -- fail closed
        print("ci_tiers: cannot compute changed files (%s)" % e, file=sys.stderr)
    res = decide(a.event, a.ref, files, lambda: defect_table(a.repo))
    for k in ("heavy_release", "alpha_selfhost_tier", "keystroke", "decnet", "negctl_select"):
        print("%s=%s" % (k, res[k]))


if __name__ == "__main__":
    main()
