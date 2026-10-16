#!/usr/bin/env python3
"""Drives tools/ci/red_main_bot.py's whole state machine against a fake gh/git."""
import json
import os
import sys

sys.path.insert(0, os.path.join(sys.argv[1], "tools", "ci"))
import red_main_bot as bot  # noqa: E402

REPO = "o/r"
WF = "ci-vax.yml"
SHAS = ["c%02d" % i + "0" * 37 for i in range(10)]  # c00.. oldest -> newest
GOOD, BAD = SHAS[0], SHAS[9]
JOB = "VAX boot"


class Fake:
    def __init__(self, culprit, subjects=None, conflict=False, probe_other_red=False):
        self.culprit = culprit
        self.subjects = subjects or {}
        self.conflict = conflict
        self.probe_other_red = probe_other_red
        self.issues = {}
        self.log = []
        self.dispatched = []
        self.branches = {}
        self.prs = []
        self.reruns = []
        self.deleted = []

    def ok(self, *cmd):
        self.log.append(cmd)
        if cmd[:2] == ("git", "merge-base"):
            return True
        if cmd[:2] == ("git", "revert"):
            return not self.conflict
        return True

    def __call__(self, *cmd, check=True, stdin=None):
        self.log.append(cmd)
        c = cmd
        if c[:2] == ("gh", "api") and "/jobs?" in c[2]:
            rid = c[2].split("/runs/")[1].split("/")[0]
            return json.dumps({"jobs": self.jobs_for(rid)})
        if c[:3] == ("gh", "run", "list"):
            return json.dumps([{"databaseId": 100, "headSha": GOOD, "event": "schedule", "conclusion": "success"}])
        if c[:3] == ("gh", "run", "rerun"):
            self.reruns.append(c[4]); return ""
        if c[:3] == ("gh", "issue", "list"):
            return json.dumps([{"number": n, "body": b} for n, b in self.issues.items()])
        if c[:3] == ("gh", "issue", "create"):
            n = len(self.issues) + 7
            self.issues[n] = c[c.index("--body") + 1]
            return "https://github.com/o/r/issues/%d\n" % n
        if c[:3] == ("gh", "issue", "edit"):
            self.issues[int(c[3])] = c[c.index("--body") + 1]; return ""
        if c[:3] == ("gh", "issue", "comment"):
            return ""
        if c[:2] == ("gh", "api") and c[2].endswith("/git/refs"):
            ref = [x for x in c if x.startswith("ref=")][0][4:]
            sha = [x for x in c if x.startswith("sha=")][0][4:]
            self.branches[ref.replace("refs/heads/", "")] = sha; return ""
        if c[:2] == ("gh", "api") and "-X" in c:
            self.deleted.append(c[-1]); return ""
        if c[:3] == ("gh", "workflow", "run"):
            self.dispatched.append(c[c.index("--ref") + 1]); return ""
        if c[:3] == ("git", "rev-list", "--first-parent"):
            return "\n".join(SHAS[1:]) + "\n"
        if c[:3] == ("git", "log", "-1") and "--format=%s" in c:
            sha = c[-1] if len(c) > 4 else "HEAD"
            return self.subjects.get(sha, "feature work") + "\n"
        if c[:3] == ("gh", "pr", "list"):
            return "[]"
        if c[:3] == ("gh", "pr", "create"):
            self.prs.append(dict(zip(c[::2], c[1::2])) if False else c)
            return "https://github.com/o/r/pull/99\n"
        return ""

    def jobs_for(self, rid):
        rid = str(rid)
        if rid == "100":
            return [{"name": JOB, "conclusion": "success"}]
        if rid.startswith("9"):  # mainline / probe runs: red in JOB
            name = "other job" if (self.probe_other_red and rid.startswith("95")) else JOB
            return [{"name": name, "conclusion": "failure"}]
        return [{"name": JOB, "conclusion": "success"}]


def main_run(attempt, concl="failure", rid=900):
    return {"workflow_run": {"id": rid, "path": ".github/workflows/%s@refs/heads/main" % WF, "event": "schedule",
                             "head_branch": "main", "head_sha": BAD, "run_attempt": attempt, "conclusion": concl}}


def probe_run(branch, attempt, concl, rid):
    return {"workflow_run": {"id": rid, "path": ".github/workflows/%s@refs/heads/%s" % (WF, branch), "event": "workflow_dispatch",
                             "head_branch": branch, "head_sha": "x", "run_attempt": attempt, "conclusion": concl}}


fail = 0


def check(cond, msg):
    global fail
    print(("  PASS: " if cond else "  FAIL: ") + msg)
    if not cond:
        fail += 1


def drive(f, culprit_idx):
    """Run mainline red + every probe to completion; return the final status."""
    r = bot.handle(f, REPO, main_run(2))
    seen = 0
    while r == "probing":
        br = f.dispatched[-1]
        sha = f.branches[br]
        idx = SHAS.index(sha) - 1  # candidates start at SHAS[1]
        rid = 800 + seen
        seen += 1
        if idx >= culprit_idx:
            r = bot.handle(f, REPO, probe_run(br, 1, "failure", 900 + seen))
            check(r == "probe-rerun-requested", "probe %s: first red is re-run, not trusted" % sha[:3])
            r = bot.handle(f, REPO, probe_run(br, 2, "failure", 900 + seen))
        else:
            r = bot.handle(f, REPO, probe_run(br, 1, "success", rid))
    return r


# 1. a first red only requests a re-run -- never bisects / reverts
f = Fake(6)
check(bot.handle(f, REPO, main_run(1)) == "rerun-requested" and f.reruns and not f.issues and not f.prs,
      "first red on main: only a re-run of the failed jobs (infra flake cannot revert anyone)")
# 2. a re-run that passes is a flake: nothing happens
check(bot.handle(f, REPO, main_run(2, "success")) == "ignored" and not f.issues, "re-run green: flake, no action")
# 3. PR runs / other branches are ignored
check(bot.handle(f, REPO, {"workflow_run": {"id": 1, "path": "x@y", "event": "pull_request", "head_branch": "feat", "head_sha": "s",
                                            "run_attempt": 1, "conclusion": "failure"}}) == "ignored", "PR run ignored")
# 4. confirmed red -> bisect -> revert PR naming the culprit
cul = 6
f = Fake(cul, subjects={SHAS[1 + cul]: "vms-zzz: break the VAX boot"})
r = drive(f, cul)
check(r == "revert-opened", "bisect ends in a revert PR (got %s)" % r)
body = " ".join(str(x) for x in f.prs[0])
check(SHAS[1 + cul] in body and "break the VAX boot" in body and JOB in body, "revert PR names the culprit commit, subject and failed job")
check(len(f.dispatched) <= 4, "bisect used %d probes for 9 candidates (log2)" % len(f.dispatched))
check(any(c[:3] == ("git", "push", "origin") and c[3].startswith("autorevert/") for c in f.log), "revert pushed on an autorevert/ branch")
check(any(c[:3] == ("gh", "pr", "merge") and "--auto" in c for c in f.log), "auto-merge (merge queue) enabled on the revert PR")
check(len(f.deleted) == 9, "all probe branches are cleaned up")
# 5. culprit is the very first / very last candidate
for cul in (0, 8):
    f = Fake(cul)
    check(drive(f, cul) == "revert-opened" and SHAS[1 + cul] in " ".join(str(x) for x in f.prs[0]), "culprit at candidate %d found" % cul)
# 6. refusals
f = Fake(6, subjects={SHAS[7]: "cut: V0.7-10"})
check(drive(f, 6) == "refused-revert-or-cut" and not f.prs, "a cut commit is never auto-reverted")
f = Fake(6, subjects={SHAS[7]: "Revert \"x\""})
check(drive(f, 6) == "refused-revert-or-cut" and not f.prs, "a revert is never auto-reverted (no loops)")
f = Fake(6, conflict=True)
check(drive(f, 6) == "refused-conflict" and not f.prs, "a revert that conflicts is reported, not forced")
f = Fake(6, probe_other_red=True)
f.jobs_for_orig = f.jobs_for
r = bot.handle(f, REPO, main_run(2))
br = f.dispatched[-1]
f.jobs_for = lambda rid: [{"name": "different job", "conclusion": "failure"}]
r = bot.handle(f, REPO, probe_run(br, 2, "failure", 950))
check(r == "inconclusive" and not f.prs, "a probe red in a DIFFERENT job is inconclusive, never reverted")

print("red_main_bot: %d failed" % fail)
sys.exit(1 if fail else 0)
