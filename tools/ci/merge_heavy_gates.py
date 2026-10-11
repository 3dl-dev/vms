#!/usr/bin/env python3
"""merge_heavy_gates.py -- did every gate a PR run SKIPPED pass on its head SHA?

usage: tools/ci/merge_heavy_gates.py <pr-number> [--allow 'WORKFLOW::JOB::FIXED-BY' ...]
                                     [--table FILE]

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

A MAPPED RED (--allow, repeatable) lets a latest run that failed count when
EVERY failed job in it matches an entry: WORKFLOW is the workflow name exactly,
JOB a substring of the failed job's name, FIXED-BY the PR or item that fixes it
(mandatory -- a red with no named fix is not mapped). Use it only for a red the
same job shows on main, which another PR is landing the fix for; anything else
is a stop. --table FILE writes the mapping as a Markdown table for the merge
comment.

A MAPPED RED MUST NOT HIDE ANOTHER. When a mapped job's failing step made later
steps of that job SKIP, those steps proved nothing on the head (Release/E2E's
smoke job skipped boot-to-login and the taint gate for days behind a mapped
TT red). The mapping is then refused unless each such step PASSED in the same
job of main's latest finished run of the workflow (or was skipped there by its
own condition, with nothing failed ahead of it). Give independent steps
`if: !cancelled() && <prerequisite ok>` so they run past a red instead.

Exit 0 when every such workflow passed (or failed only in mapped jobs) on the
head SHA; 1 when one is missing or not green (each is listed with what to do);
2 on a usage or gh error.
"""
import json
import subprocess
import sys


def gh(args):
    r = subprocess.run(["gh"] + args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("gh %s: %s" % (" ".join(args), r.stderr.strip()))
    return json.loads(r.stdout or "null")


IGNORED_STEPS = ("Set up job", "Complete job")


def step_list(job):
    return sorted(job.get("steps") or [], key=lambda st: st.get("number", 0))


def hidden_steps(job):
    """Names of the steps that SKIPPED after the job's first failed step."""
    steps = step_list(job)
    first = next((i for i, st in enumerate(steps) if st.get("conclusion") == "failure"), None)
    if first is None:
        return []
    return [st.get("name", "") for st in steps[first + 1:]
            if st.get("conclusion") == "skipped"
            and st.get("name", "") not in IGNORED_STEPS
            and not st.get("name", "").startswith("Post ")]


def uncovered_on_main(hidden, main_job):
    """The hidden steps main's job does not vouch for: each must have passed
    there, or been skipped by its own condition with nothing failed ahead."""
    if not main_job:
        return list(hidden)
    steps = step_list(main_job)
    out = []
    for name in hidden:
        idx = next((i for i, st in enumerate(steps) if st.get("name") == name), None)
        if idx is None:
            out.append(name)
            continue
        c = steps[idx].get("conclusion")
        failed_before = any(st.get("conclusion") == "failure" for st in steps[:idx])
        if c == "success" or (c == "skipped" and not failed_before):
            continue
        out.append(name)
    return out


def main_job_of(wf, job_name):
    runs = gh(["run", "list", "--workflow", wf, "--branch", "main", "--limit", "20",
               "--json", "databaseId,status,conclusion,createdAt"]) or []
    done = [r for r in runs if r.get("status") == "completed"
            and r.get("conclusion") != "cancelled"]
    done.sort(key=lambda r: r.get("createdAt", ""))
    for r in reversed(done):
        jobs = (gh(["run", "view", str(r["databaseId"]), "--json", "jobs"]) or {}).get("jobs") or []
        for j in jobs:
            if j.get("name") == job_name and j.get("conclusion") not in (None, "", "cancelled", "skipped"):
                return r["databaseId"], j
    return None, None


def parse(argv):
    pr, allows, table = None, [], None
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--allow" and i + 1 < len(argv):
            parts = argv[i + 1].split("::")
            if len(parts) != 3 or not all(p.strip() for p in parts):
                return None
            allows.append(tuple(p.strip() for p in parts))
            i += 2
        elif a == "--table" and i + 1 < len(argv):
            table = argv[i + 1]
            i += 2
        elif a.isdigit() and pr is None:
            pr = a
            i += 1
        else:
            return None
    return (pr, allows, table) if pr else None


def main(argv):
    parsed = parse(argv)
    if not parsed:
        print(__doc__)
        return 2
    pr, allows, table = parsed
    try:
        info = gh(["pr", "view", pr, "--json", "headRefOid,headRefName,statusCheckRollup"])
    except RuntimeError as e:
        print("merge_heavy_gates: %s" % e)
        return 2
    sha = info["headRefOid"]
    branch = info.get("headRefName", "<branch>")
    skipped = sorted({c.get("workflowName") for c in info.get("statusCheckRollup") or []
                      if c.get("conclusion") == "SKIPPED" and c.get("workflowName")})
    rows = []
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
            continue
        last = done[-1]
        rid = last.get("databaseId")
        if last.get("conclusion") == "success":
            print("  ok: %s -- dispatch run %s on %s succeeded" % (wf, rid, sha[:9]))
            rows.append((wf, rid, "-", "green"))
            continue
        if last.get("conclusion") != "failure":
            bad.append("%s: latest dispatch run %s on %s concluded %s" %
                       (wf, rid, sha[:9], last.get("conclusion")))
            continue
        try:
            jobs = (gh(["run", "view", str(rid), "--json", "jobs"]) or {}).get("jobs") or []
        except RuntimeError as e:
            print("merge_heavy_gates: %s" % e)
            return 2
        failed = [j for j in jobs
                  if j.get("conclusion") not in ("success", "skipped", "neutral")]
        unmapped = []
        for jobd in failed:
            job = jobd.get("name", "")
            hit = [a for a in allows if a[0] == wf and a[1] in job]
            if not hit:
                unmapped.append(job)
                continue
            hidden = hidden_steps(jobd)
            if hidden:
                try:
                    mrid, mjob = main_job_of(wf, job)
                except RuntimeError as e:
                    print("merge_heavy_gates: %s" % e)
                    return 2
                unc = uncovered_on_main(hidden, mjob)
                if unc:
                    bad.append("%s / %s: the mapped red SKIPPED later steps that did not pass on "
                               "main's latest run (%s) either, so nothing proves them: %s -- make "
                               "them run past the red (if: !cancelled() && ...) and re-dispatch" %
                               (wf, job, mrid or "none", "; ".join(unc)))
                    continue
                print("  mapped: %s / %s -- its skipped steps passed on main run %s" % (wf, job, mrid))
            print("  mapped: %s / %s -- red, fixed by %s" % (wf, job, hit[0][2]))
            rows.append((wf, rid, job, "fixed by " + hit[0][2]))
        if unmapped or (not failed):
            bad.append("%s: latest dispatch run %s on %s concluded failure; unmapped job(s): %s" %
                       (wf, rid, sha[:9], ", ".join(unmapped) or "(none listed)"))
    if table:
        with open(table, "w") as fh:
            fh.write("| Workflow | Run | Failing job | Status |\n|---|---|---|---|\n")
            for wf, rid, job, st in rows:
                fh.write("| %s | %s | %s | %s |\n" % (wf, rid, job, st))
    if bad:
        print("FAIL: PR #%s skipped checks whose workflows have not passed on its head %s:" % (pr, sha[:9]))
        for b in bad:
            print("  " + b)
        return 1
    print("OK: every workflow PR #%s skipped passed on its head %s (mapped reds listed above)" % (pr, sha[:9]))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
