#!/usr/bin/env python3
"""red_main_bot.py -- find the commit that turned main (or the nightly) red and
revert it through the merge queue.

Driven by `workflow_run` completion events of the six CI workflows
(.github/workflows/red-main-bot.yml). It is a STATE MACHINE, one step per
event, so nothing waits for hours inside a job; state lives in one tracking
issue (label red-main-bot) as a JSON block.

  mainline run (event push|schedule on main) FAILED
    attempt 1 -> `gh run rerun --failed`, stop. (Never act on a first red: an
                 infra flake must not revert anybody's work.)
    attempt 2+, same jobs still red -> CONFIRMED real. Find the last run of
                 that workflow on an ancestor commit where those jobs passed;
                 the commits between are the candidates. One candidate = the
                 culprit; several = bisect.
    attempt 2+ green -> flake; log and stop.
  bisect probe run (workflow_dispatch on branch bisect/*) COMPLETED
    failed attempt 1 -> rerun failed (same flake rule); success / confirmed red
    -> narrow the range, dispatch the next probe, or name the culprit.
  culprit found -> `git revert` it on branch autorevert/<sha12>, open a PR
    whose title and body NAME the culprit commit, the failed jobs and the run,
    and enable auto-merge so it lands through the merge queue. A revert that
    conflicts, a culprit that is itself a revert/cut commit, or an already open
    bot revert PR stops the bot with a comment on the tracking issue instead.

All GitHub/git access goes through the `sh` callable so tests drive the whole
state machine with a fake.
"""
import json
import re
import subprocess
import sys

LABEL = "red-main-bot"
PROBE_PREFIX = "bisect/"
REVERT_PREFIX = "autorevert/"
STATE_RE = re.compile(r"<!-- red-main-bot-state\n(.*?)\n-->", re.S)


class Sh:
    """Real shell: returns stdout, raises on failure."""

    def __call__(self, *cmd, check=True, stdin=None):
        r = subprocess.run(list(cmd), capture_output=True, text=True, input=stdin)
        if check and r.returncode != 0:
            raise RuntimeError("%s failed: %s" % (" ".join(cmd[:3]), r.stderr.strip()))
        return r.stdout

    def ok(self, *cmd):
        """True iff the command exits 0 (no exception)."""
        return subprocess.run(list(cmd), capture_output=True, text=True).returncode == 0


def gh_json(sh, *args):
    return json.loads(sh("gh", *args))


def failed_jobs(sh, repo, run_id):
    d = gh_json(sh, "api", "repos/%s/actions/runs/%s/jobs?per_page=100&filter=latest" % (repo, run_id))
    return sorted(j["name"] for j in d["jobs"] if j["conclusion"] == "failure")


def job_results(sh, repo, run_id):
    d = gh_json(sh, "api", "repos/%s/actions/runs/%s/jobs?per_page=100&filter=latest" % (repo, run_id))
    return {j["name"]: j["conclusion"] for j in d["jobs"]}


def is_ancestor(sh, a, b):
    return sh.ok("git", "merge-base", "--is-ancestor", a, b)


# ---- tracking issue state ---------------------------------------------------
def find_issue(sh, repo, wf_file, bad_sha=None):
    issues = gh_json(sh, "issue", "list", "--repo", repo, "--label", LABEL, "--state", "open",
                     "--json", "number,body", "--limit", "50")
    for i in issues:
        m = STATE_RE.search(i.get("body") or "")
        if not m:
            continue
        st = json.loads(m.group(1))
        if st["workflow"] == wf_file and (bad_sha is None or st["bad"] == bad_sha):
            return i["number"], st
    return None, None


def render(st, note):
    return "%s\n\n<!-- red-main-bot-state\n%s\n-->" % (note, json.dumps(st, sort_keys=True))


def save_issue(sh, repo, num, st, note):
    sh("gh", "issue", "edit", str(num), "--repo", repo, "--body", render(st, note))


def new_issue(sh, repo, st, title, note):
    out = sh("gh", "issue", "create", "--repo", repo, "--label", LABEL, "--title", title,
             "--body", render(st, note))
    return int(out.strip().rsplit("/", 1)[-1])


def comment(sh, repo, num, text):
    sh("gh", "issue", "comment", str(num), "--repo", repo, "--body", text)


# ---- the steps ----------------------------------------------------------------
def candidates_between(sh, good, bad):
    out = sh("git", "rev-list", "--first-parent", "--reverse", "%s..%s" % (good, bad))
    return [l for l in out.split() if l]


def last_good(sh, repo, wf_file, bad, jobs):
    runs = gh_json(sh, "run", "list", "--repo", repo, "--workflow", wf_file, "--branch", "main",
                   "--limit", "100", "--json", "databaseId,headSha,event,conclusion")
    for r in runs:
        if r["event"] not in ("push", "schedule") or r["headSha"] == bad:
            continue
        if not is_ancestor(sh, r["headSha"], bad):
            continue
        res = job_results(sh, repo, r["databaseId"])
        if jobs and all(res.get(j) == "success" for j in jobs):
            return r["headSha"]
    return None


def start_probe(sh, repo, st, idx):
    sha = st["candidates"][idx]
    branch = "%s%s-%s" % (PROBE_PREFIX, st["issue"], sha[:12])
    sh("gh", "api", "repos/%s/git/refs" % repo, "-f", "ref=refs/heads/" + branch, "-f", "sha=" + sha)
    sh("gh", "workflow", "run", st["workflow"], "--repo", repo, "--ref", branch)
    st["probe"] = {"idx": idx, "branch": branch}


def cleanup_probes(sh, repo, st):
    for sha in st["candidates"]:
        b = "%s%s-%s" % (PROBE_PREFIX, st["issue"], sha[:12])
        sh("gh", "api", "-X", "DELETE", "repos/%s/git/refs/heads/%s" % (repo, b), check=False)


def advance(sh, repo, st, num):
    """Probe the middle of (lo, hi), or name the culprit."""
    lo, hi = st["lo"], st["hi"]
    if hi - lo <= 1:
        cleanup_probes(sh, repo, st)
        return open_revert(sh, repo, st, num, st["candidates"][hi])
    start_probe(sh, repo, st, (lo + hi) // 2)
    save_issue(sh, repo, num, st, "Bisecting %s: %d candidate(s) left; probing %s"
               % (st["workflow"], hi - lo, st["candidates"][(lo + hi) // 2][:12]))
    return "probing"


def open_revert(sh, repo, st, num, culprit):
    subj = sh("git", "log", "-1", "--format=%s", culprit).strip()
    if subj.startswith("Revert ") or subj.startswith("cut:"):
        comment(sh, repo, num, "Culprit %s (%s) is a revert/cut commit: NOT auto-reverting. Human decision." % (culprit[:12], subj))
        return "refused-revert-or-cut"
    existing = gh_json(sh, "pr", "list", "--repo", repo, "--state", "open", "--label", LABEL, "--json", "number")
    if existing:
        comment(sh, repo, num, "A bot revert PR (#%d) is already open; not stacking another. Culprit: %s (%s)."
                % (existing[0]["number"], culprit[:12], subj))
        return "refused-open-revert"
    branch = REVERT_PREFIX + culprit[:12]
    sh("git", "checkout", "-B", branch, "origin/main")
    if not sh.ok("git", "revert", "--no-edit", culprit):
        sh("git", "revert", "--abort", check=False)
        comment(sh, repo, num, "`git revert %s` does not apply cleanly on main (later commits depend on it). "
                "NOT auto-reverting; culprit named for a human: %s (%s)." % (culprit[:12], culprit, subj))
        return "refused-conflict"
    sh("git", "push", "origin", branch)
    jobs = ", ".join(st["jobs"])
    body = ("Automatic revert by the red-main bot (tools/ci/red_main_bot.py).\n\n"
            "**Culprit:** %s -- %s\n**Workflow:** %s\n**Failed jobs (confirmed red on re-run; bisected):** %s\n"
            "**Tracking issue:** #%d\n\nThe bot re-ran the failed jobs once to rule out an infra flake before "
            "bisecting. If this revert is wrong, close the PR; it merges through the queue like any other.\n\n"
            "Fixes the red in #%d.\n" % (culprit, subj, st["workflow"], jobs, num, num))
    url = sh("gh", "pr", "create", "--repo", repo, "--base", "main", "--head", branch, "--label", LABEL,
             "--title", "Revert \"%s\" (red-main bot: culprit of %s)" % (subj, st["workflow"]),
             "--body", body).strip()
    sh("gh", "pr", "merge", "--repo", repo, "--auto", url, check=False)
    comment(sh, repo, num, "Culprit: %s (%s). Revert PR: %s (auto-merge enabled)." % (culprit, subj, url))
    return "revert-opened"


def on_mainline(sh, repo, run):
    wf = run["path"].split("@")[0].split("/")[-1]
    bad = run["head_sha"]
    if run["run_attempt"] < 2:
        sh("gh", "run", "rerun", "--repo", repo, str(run["id"]), "--failed")
        return "rerun-requested"
    jobs = failed_jobs(sh, repo, run["id"])
    if not jobs:
        return "no-failed-jobs"
    num, st = find_issue(sh, repo, wf)
    if st:
        return "already-tracked"
    good = last_good(sh, repo, wf, bad, jobs)
    if not good:
        new_issue(sh, repo, {"workflow": wf, "bad": bad, "jobs": jobs, "candidates": [], "lo": -1, "hi": -1},
                  "red-main: %s red at %s (no known-good run to bisect from)" % (wf, bad[:12]),
                  "Confirmed red after a re-run; no earlier green run of these jobs found. Human triage.\nJobs: %s" % ", ".join(jobs))
        return "no-good-baseline"
    cands = candidates_between(sh, good, bad)
    st = {"workflow": wf, "bad": bad, "good": good, "jobs": jobs, "candidates": cands,
          "lo": -1, "hi": len(cands) - 1}
    num = new_issue(sh, repo, st, "red-main: %s red at %s" % (wf, bad[:12]),
                    "Confirmed red after a re-run. Jobs: %s\nLast green: %s\nCandidates: %d"
                    % (", ".join(jobs), good[:12], len(cands)))
    st["issue"] = num
    return advance(sh, repo, st, num)


def on_probe(sh, repo, run):
    wf = run["path"].split("@")[0].split("/")[-1]
    m = re.match(r"^bisect/(\d+)-", run["head_branch"])
    if not m:
        return "not-a-probe"
    num, st = find_issue(sh, repo, wf)
    if not st or st.get("issue") != int(m.group(1)) or not st.get("probe"):
        return "no-state"
    idx = st["probe"]["idx"]
    if run["conclusion"] == "success":
        verdict = "good"
    else:
        res = job_results(sh, repo, run["id"])
        red = [j for j in st["jobs"] if res.get(j) == "failure"]
        if not red:
            comment(sh, repo, num, "Probe of %s was red but not in the tracked jobs (%s): inconclusive. Stopping; human triage."
                    % (st["candidates"][idx][:12], ", ".join(sorted(k for k, v in res.items() if v == "failure"))))
            cleanup_probes(sh, repo, st)
            return "inconclusive"
        if run["run_attempt"] < 2:
            sh("gh", "run", "rerun", "--repo", repo, str(run["id"]), "--failed")
            return "probe-rerun-requested"
        verdict = "bad"
    if verdict == "good":
        st["lo"] = idx
    else:
        st["hi"] = idx
    comment(sh, repo, num, "Probe %s: %s" % (st["candidates"][idx][:12], verdict))
    return advance(sh, repo, st, num)


def handle(sh, repo, payload):
    run = payload["workflow_run"]
    if run["event"] == "workflow_dispatch" and run["head_branch"].startswith(PROBE_PREFIX):
        if run["conclusion"] in ("success", "failure"):
            return on_probe(sh, repo, run)
        return "probe-ignored-conclusion"
    if run["head_branch"] == "main" and run["event"] in ("push", "schedule") and run["conclusion"] == "failure":
        return on_mainline(sh, repo, run)
    return "ignored"


def main():
    if len(sys.argv) != 3:
        print("usage: red_main_bot.py <event.json> <owner/repo>", file=sys.stderr)
        return 2
    payload = json.load(open(sys.argv[1]))
    print("red_main_bot:", handle(Sh(), sys.argv[2], payload))
    return 0


if __name__ == "__main__":
    sys.exit(main())
