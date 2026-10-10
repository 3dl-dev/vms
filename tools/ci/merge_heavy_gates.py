#!/usr/bin/env python3
"""merge_heavy_gates.py -- did every gate a PR run SKIPPED pass on its head SHA?

usage: tools/ci/merge_heavy_gates.py <pr-number>

The heavy boot/e2e/negctl jobs (the VMS User Acceptance Test, the Persistent
Boot Smoke Test and its semantic oracle, the per-facility negative controls,
...) do not run on pull_request -- they show SKIPPED in the PR's checks -- so a
PR's green rollup says nothing about them. #1554 merged that way and left main's
VMS User Acceptance Test red (2026-10-09): nobody had run it on the PR's head.

This gate closes that hole. For every workflow with a SKIPPED check on the PR,
there must be a workflow_dispatch run of that workflow ON THE PR'S HEAD SHA
(`gh workflow run <file> --ref <pr-branch>`), and the latest such run that
finished (cancelled runs ignored) must have concluded success. A workflow run
concludes success only if none of its jobs failed, so the skipped jobs are
covered by the dispatch run.

Exit 0 when every such workflow passed on the head SHA; 1 when one is missing or
not green (each is listed with what to do); 2 on a usage or gh error.
"""
import json
import subprocess
import sys


def gh(args):
    r = subprocess.run(["gh"] + args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("gh %s: %s" % (" ".join(args), r.stderr.strip()))
    return json.loads(r.stdout or "null")


def main(argv):
    if len(argv) != 2 or not argv[1].isdigit():
        print(__doc__)
        return 2
    pr = argv[1]
    try:
        info = gh(["pr", "view", pr, "--json", "headRefOid,headRefName,statusCheckRollup"])
    except RuntimeError as e:
        print("merge_heavy_gates: %s" % e)
        return 2
    sha = info["headRefOid"]
    branch = info.get("headRefName", "<branch>")
    skipped = sorted({c.get("workflowName") for c in info.get("statusCheckRollup") or []
                      if c.get("conclusion") == "SKIPPED" and c.get("workflowName")})
    if not skipped:
        print("OK: PR #%s skipped no checks -- its own rollup is the whole evidence" % pr)
        return 0
    bad = []
    for wf in skipped:
        try:
            runs = gh(["run", "list", "--workflow", wf, "--commit", sha, "--event",
                       "workflow_dispatch", "--limit", "30",
                       "--json", "databaseId,status,conclusion,createdAt"]) or []
        except RuntimeError as e:
            print("merge_heavy_gates: %s" % e)
            return 2
        done = [r for r in runs if r.get("status") == "completed"
                and r.get("conclusion") != "cancelled"]
        done.sort(key=lambda r: r.get("createdAt", ""))
        if not done:
            bad.append("%s: no finished workflow_dispatch run on %s -- run "
                       "`gh workflow run '%s' --ref %s` and wait for it" % (wf, sha[:9], wf, branch))
        elif done[-1].get("conclusion") != "success":
            bad.append("%s: latest dispatch run %s on %s concluded %s" %
                       (wf, done[-1].get("databaseId"), sha[:9], done[-1].get("conclusion")))
        else:
            print("  ok: %s -- dispatch run %s on %s succeeded" % (wf, done[-1].get("databaseId"), sha[:9]))
    if bad:
        print("FAIL: PR #%s skipped checks whose workflows have not passed on its head %s:" % (pr, sha[:9]))
        for b in bad:
            print("  " + b)
        return 1
    print("OK: every workflow PR #%s skipped passed on its head %s" % (pr, sha[:9]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
