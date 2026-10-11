#!/usr/bin/env python3
"""check_merge_gates.py - every gating workflow ends in a `gate` job the merge
queue can require, and that job waits on every other job in the workflow.

The merge queue on main requires ONE check per gating workflow, the aggregate
`gate` job: `needs:` every other job, `if: always()`, and red when any of them
failed or was cancelled (a job skipped by its own condition, or by a path
filter, counts as passed). A job added to a gating workflow but left out of the
gate's needs would never block a merge, so this fails on it. A gating workflow
must also run on every pull request (no `paths:` filter on pull_request), or
its required gate would never report on a PR that touches none of the paths.

usage: check_merge_gates.py [workflows-dir]          (exit 0 ok, 1 not)
"""
import os
import sys

import yaml

GATING = ["ci-alpha.yml", "ci-core-gates.yml", "ci-kernel-executive.yml",
          "ci-release-e2e.yml", "ci-selfhost-x86.yml", "ci-vax.yml", "ledger.yml"]


def check(path):
    errs = []
    d = yaml.safe_load(open(path))
    on = d.get(True) or d.get("on") or {}
    if not isinstance(on, dict) or "merge_group" not in on:
        errs.append("does not run on merge_group")
    pr = on.get("pull_request") if isinstance(on, dict) else None
    if not isinstance(on, dict) or "pull_request" not in on:
        errs.append("does not run on pull_request")
    elif isinstance(pr, dict) and ("paths" in pr or "paths-ignore" in pr):
        errs.append("pull_request has a paths filter: its gate would not report on every PR")
    jobs = d.get("jobs") or {}
    gate = jobs.get("gate")
    if not gate:
        return errs + ["no `gate` job"]
    needs = gate.get("needs") or []
    if isinstance(needs, str):
        needs = [needs]
    others = sorted(j for j in jobs if j != "gate")
    missing = [j for j in others if j not in needs]
    if missing:
        errs.append("gate does not need: " + ", ".join(missing))
    if "always()" not in str(gate.get("if", "")):
        errs.append("gate is not `if: always()` (it would be skipped when a job fails)")
    if "toJSON(needs)" not in yaml.safe_dump(gate):
        errs.append("gate does not inspect the needs results")
    return errs


def main():
    wdir = sys.argv[1] if len(sys.argv) > 1 else ".github/workflows"
    bad = 0
    for f in GATING:
        p = os.path.join(wdir, f)
        if not os.path.exists(p):
            print("FAIL %s: missing" % f)
            bad += 1
            continue
        errs = check(p)
        for e in errs:
            print("FAIL %s: %s" % (f, e))
        bad += bool(errs)
        if not errs:
            print("  ok: %s" % f)
    print("check_merge_gates: %s" % ("FAIL" if bad else "ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
