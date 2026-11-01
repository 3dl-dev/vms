#!/usr/bin/env python3
"""pre_tag_verify.py -- the FULL suite, on the exact SHA a release tags.

The merge queue no longer runs every heavy leg (release cuts + e2e, the
per-facility executive negctl, Alpha cc1/C++ self-host, keystroke oracle): it
runs each only when its area is touched (tools/ci/ci_tiers.py). The backstops
are the nightly schedule and THIS: a release must be tagged on a SHA where all
six CI workflows completed green as a FULL run (event schedule or
workflow_dispatch -- both force every heavy tier on).

  pre_tag_verify.py dispatch [--ref REF]   start the six full runs on REF
                                           (default main) and print their URLs
  pre_tag_verify.py check --sha SHA        exit 0 iff each workflow has a
                                           successful full run on SHA; prints
                                           what is missing otherwise

release.yml runs `check` against the tagged commit before cutting/publishing,
so a tag on a SHA that never ran the moved tests cannot publish.
"""
import argparse
import json
import subprocess
import sys

WORKFLOWS = [
    "ci-alpha.yml",
    "ci-core-gates.yml",
    "ci-kernel-executive.yml",
    "ci-release-e2e.yml",
    "ci-selfhost-x86.yml",
    "ci-vax.yml",
]
FULL_EVENTS = ("schedule", "workflow_dispatch")


def gh(*args):
    return subprocess.run(["gh", *args], check=True, capture_output=True, text=True).stdout


def runs_for(wf, repo=None):
    cmd = ["run", "list", "--workflow", wf, "--limit", "100",
           "--json", "databaseId,headSha,event,conclusion,status,url"]
    if repo:
        cmd += ["--repo", repo]
    return json.loads(gh(*cmd))


def check(sha, repo=None, fetch=runs_for):
    missing = []
    for wf in WORKFLOWS:
        ok = [r for r in fetch(wf, repo)
              if r["headSha"] == sha and r["event"] in FULL_EVENTS
              and r["conclusion"] == "success"]
        if ok:
            print("ok:      %-26s %s" % (wf, ok[0]["url"]))
        else:
            missing.append(wf)
            print("MISSING: %-26s no successful full (schedule/dispatch) run on %s" % (wf, sha[:12]))
    if missing:
        print("\npre-tag verify FAILED: run `tools/ci/pre_tag_verify.py dispatch --ref <branch>` on the "
              "release SHA and wait for all six to pass before tagging.")
        return 1
    print("\npre-tag verify OK: every moved heavy test ran green on %s" % sha[:12])
    return 0


def dispatch(ref, repo=None):
    for wf in WORKFLOWS:
        cmd = ["workflow", "run", wf, "--ref", ref]
        if repo:
            cmd += ["--repo", repo]
        gh(*cmd)
        print("dispatched %s on %s" % (wf, ref))
    print("watch: gh run list --event workflow_dispatch --branch %s" % ref)
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dispatch")
    d.add_argument("--ref", default="main")
    c = sub.add_parser("check")
    c.add_argument("--sha", required=True)
    for p in (d, c):
        p.add_argument("--repo")
    a = ap.parse_args()
    sys.exit(dispatch(a.ref, a.repo) if a.cmd == "dispatch" else check(a.sha, a.repo))


if __name__ == "__main__":
    main()
