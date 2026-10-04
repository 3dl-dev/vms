#!/usr/bin/env python3
"""One-shot generator for the vms-1af ci.yml split.

Reads the monolithic .github/workflows/ci.yml (511KB, over GitHub's silent
~512000-byte non-comment workflow-file ceiling -- see rd vms-1af / vms-e18e)
and emits:
  - .github/workflows/ci-changes.yml       reusable path-filter workflow
  - .github/workflows/ci-<domain>.yml      one file per job domain, each a
                                            drop-in replacement for a slice of
                                            the original jobs: with byte-exact
                                            job bodies, same triggers, same
                                            job names, same needs graph.

This script is NOT part of CI -- it is the one-time migration tool for
vms-1af. Left in tools/ci/ for provenance / re-running if ci.yml ever needs
re-splitting.
"""
import re
import sys

SRC = ".github/workflows/ci.yml"

CHANGES_OUTPUT_KEYS = [
    "core", "toolchain", "kernel", "boot", "release", "netbsd", "netbsd_vax",
    "exec_netbsd_vax", "aarch64_boot", "compat_surface", "libcrypto",
    "openssh", "alpha_toolchain", "alpha_boot_runtime", "alpha_activation",
    "vax_toolchain", "tcpip",
]

GROUPS = {
    "core-gates": {
        "title": "Core Gates",
        "jobs": [
            "compat-surface-drift", "cluster-core-includes-gate",
            "vmslink-shell-lint", "image-parity-gate", "vaxharness-selftest",
            "aarch64-system-boot", "build-and-test", "static-analysis",
            "conformance", "corpus-conformance", "uat-session",
        ],
    },
    "release-e2e": {
        "title": "Release / E2E",
        "jobs": [
            "persistent-boot", "cut-release-reproducible", "upgrade-e2e",
            "install-boot-e2e", "release-install-e2e",
            "release-acceptance-e2e", "tcpip-daytime-boot-e2e",
            "dcl-acceptance-e2e", "console-newline-spam-e2e",
            "device-discovery-alternate-disk-e2e", "cut-release-vax-gate",
            "cut-release-alpha-gate", "parts-demo-e2e",
            "boot-scsnode-hostname-e2e", "sysboot-cluster-params-e2e",
            "cluster-config-lan-e2e", "cluster-config-lan-2node-e2e",
        ],
    },
    "kernel-executive": {
        "title": "Kernel Executive / NetBSD-amd64",
        "jobs": [
            "kernel-executive-shard", "kernel-executive-devtab-terminal",
            "kernel-executive", "kernel-executive-negative-control",
            "netbsd-amd64-vms-crosscompile", "netbsd-amd64-prime-cache",
            "netbsd-amd64-harness", "netbsd-amd64-vms-pseudodev",
            "netbsd-amd64-eflag-facility",
            "netbsd-amd64-p4a-facilities-crossproc",
            "kernel-executive-facility-negative-controls-shard",
        ],
    },
    "selfhost-x86": {
        "title": "Self-Host / x86_64 toolchain",
        "jobs": [
            "imgact-proof", "imgact-build-mode", "imgact-x86_64",
            "vmslink-mvp", "vmslink-x86_64", "decc-shr", "link-native-cmake",
            "link-native-cmake-x86_64", "imgact-symvec",
            "imgact-vms-std-alpha", "imgact-vax-backend", "libvmssys-native",
            "decc-shr-activate", "shareable-import-activate",
            "tls-producer-over-crtl", "weak-import-activate",
            "vmsprocess-native", "vmslnm-native", "vmsfs-native",
            "libvms-native", "vmsrms-native", "multiobj-exec",
            "multiobj-exec-x86_64", "rodata-reloc-x86_64", "exec-tls",
            "dcl-native", "dcl-native-x86_64", "login-native",
            "libcrypto-static-musl", "tcc-object-native", "tcc-native",
            "mmk-native", "tcc-rms", "tcc-selfhost", "link-native",
            "link-native-x86_64", "librarian-olb-native-x86_64",
            "librarian-native-x86_64", "olb-symvec-native-x86_64",
            "mmk-link-selfhost-fixpoint",
        ],
    },
    "alpha": {
        "title": "Alpha",
        "jobs": [
            "alpha-toolchain-image", "musl-alpha-vms",
            "gcc-port-include-surface", "joint-e2e-alpha-crt0",
            "alpha-rms-substrate", "alpha-boot-login", "alpha-boot-mount",
            "alpha-crtl-rms-n7", "alpha-crtl-rms-veneer",
            "alpha-mf-multifile", "alpha-shipped-shareable-activation",
            "alpha-activation-selftest", "alpha-dcl-acceptance",
            "alpha-module-gp-activation", "alpha-arith-hparith",
            "alpha-accvio-dispatch", "alpha-syssvc-suite",
        ],
    },
    "vax": {
        "title": "VAX / NetBSD-vax",
        "jobs": [
            "netbsd-vax-simh", "netbsd-vax-substrate-prime",
            "vax-toolchain-image", "netbsd-vax-devvms", "netbsd-vax-eflag",
            "netbsd-vax-access", "netbsd-vax-proctab", "netbsd-vax-mbx",
            "netbsd-vax-boot", "netbsd-vax-sysboot", "vax-dcl-acceptance",
            "netbsd-vax-vms-crosscompile",
            "netbsd-vax-facility-tools-crosscompile", "vmslink-vax",
            "vmslink-vax-exec", "vax-shipped-shareable-packaging",
            "vax-cmake-images", "vax-acp-read-ilp32",
        ],
    },
}


def is_blank_or_comment(line):
    s = line.rstrip("\n")
    return s.strip() == "" or s.lstrip().startswith("#")


def main():
    lines = open(SRC).readlines()
    job_start_re = re.compile(r"^  ([A-Za-z0-9_-]+):\s*$")
    jobs_line = None
    starts = []
    in_jobs = False
    for i, l in enumerate(lines):
        if l.startswith("jobs:"):
            in_jobs = True
            jobs_line = i
            continue
        if not in_jobs:
            continue
        m = job_start_re.match(l)
        if m:
            starts.append((i, m.group(1)))

    true_starts = []
    prev_floor = jobs_line + 1
    for s, name in starts:
        k = s
        while k - 1 >= prev_floor and is_blank_or_comment(lines[k - 1]):
            k -= 1
        true_starts.append(k)
        prev_floor = s
    true_starts.append(len(lines))

    blocks = {}
    order_index = {}
    for idx, (s, name) in enumerate(starts):
        blk = "".join(lines[true_starts[idx]:true_starts[idx + 1]])
        blocks[name] = blk
        order_index[name] = idx

    all_names = set(blocks.keys())
    assigned = set()
    for g in GROUPS.values():
        assigned.update(g["jobs"])
    missing = all_names - assigned - {"changes"}
    if missing:
        print("ERROR: unassigned jobs:", missing, file=sys.stderr)
        sys.exit(1)
    extra = assigned - all_names
    if extra:
        print("ERROR: assigned jobs not found in source:", extra, file=sys.stderr)
        sys.exit(1)

    # The original ci.yml carried a "TIER SPLIT (vms-fb8)" rationale comment
    # between `name: CI` and `on:` (lines 3..37, i.e. everything after the
    # first two lines up to the blank line before `on:`). Preserve it
    # verbatim in ci-core-gates.yml (the file that inherits `name: CI`'s
    # role most directly); the other domain files point at it instead of
    # duplicating ~35 lines six times.
    on_line = next(i for i, l in enumerate(lines) if l.startswith("on:"))
    tier_split_comment = "".join(lines[2:on_line])

    write_changes_workflow(blocks["changes"])
    for slug, g in GROUPS.items():
        ordered = sorted(g["jobs"], key=lambda n: order_index[n])
        extra_header = tier_split_comment if slug == "core-gates" else "\n"
        write_domain_workflow(slug, g["title"], ordered, blocks, extra_header)


CHANGES_HEADER = '''name: CI changes (reusable path filter)

# Reusable path-filter workflow, split out of the monolithic ci.yml by
# vms-1af (ci.yml was ~511KB, over GitHub's silent ~512000-byte non-comment
# workflow-file ceiling -- see rd vms-1af / vms-e18e). Every ci-*.yml sibling
# file calls this as its own `changes` job via `uses:`, so the path-filter
# logic (and its subsystem list) has exactly ONE source, instead of being
# copy-pasted into every domain file. `github.event_name` / `github.event`
# inside a called reusable workflow are the CALLER's triggering event, so
# behavior here is identical to when this was an inline job in ci.yml.

on:
  workflow_call:
    outputs:
'''

CHANGES_FOOTER_JOBS_PREFIX = "jobs:\n"


def write_changes_workflow(changes_block):
    out = [CHANGES_HEADER]
    for key in CHANGES_OUTPUT_KEYS:
        out.append(f"      {key}:\n")
        out.append(f"        description: '{key} (see ci-changes.yml decide step)'\n")
        out.append(f"        value: ${{{{ jobs.changes.outputs.{key} }}}}\n")
    out.append("\npermissions:\n  contents: read\n\n")
    out.append(CHANGES_FOOTER_JOBS_PREFIX)
    out.append(changes_block)
    text = "".join(out)
    path = ".github/workflows/ci-changes.yml"
    with open(path, "w") as f:
        f.write(text)
    print(f"{path}: {len(text.encode())} bytes")


DOMAIN_HEADER_TMPL = '''name: CI / {title}

# Sibling of ci.yml, split out by vms-1af (ci.yml was ~511KB, over GitHub's
# silent ~512000-byte non-comment workflow-file ceiling that produces
# conclusion=startup_failure with ZERO jobs and no pull_request run at all --
# see rd vms-1af / vms-e18e). Triggers, job names, and the needs-graph are
# carried over unchanged from the corresponding slice of the old ci.yml; only
# the shared `changes` path-filter job is now factored into the reusable
# ci-changes.yml (see that file's header) instead of being duplicated in
# every domain file. See ci-core-gates.yml for the per-PR/heavy-tier split
# rationale (vms-fb8) and the P4-only fast-path (vms-6e10), both of which
# apply identically here via the same `vax_p4_only` input and the same `if:`
# conditions.
{extra_header}on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
  merge_group:
  workflow_dispatch:
    inputs:
      vax_p4_only:
        description: 'P4-only fast-path: run only the VAX substrate chain + the P4 activation gate, skip the full heavy matrix (rd vms-6e10)'
        type: boolean
        required: false
        default: false
  schedule:
    - cron: '17 7 * * *'

permissions:
  contents: read

concurrency:
  group: ci-${{{{ github.workflow }}}}-${{{{ github.ref }}}}
  cancel-in-progress: ${{{{ github.event_name == 'pull_request' }}}}

jobs:
  changes:
    uses: ./.github/workflows/ci-changes.yml

'''


def write_domain_workflow(slug, title, job_names, blocks, extra_header=""):
    out = [DOMAIN_HEADER_TMPL.format(title=title, extra_header=extra_header)]
    for name in job_names:
        out.append(blocks[name])
    text = "".join(out)
    path = f".github/workflows/ci-{slug}.yml"
    with open(path, "w") as f:
        f.write(text)
    print(f"{path}: {len(text.encode())} bytes  ({len(job_names)} jobs)")


if __name__ == "__main__":
    main()
