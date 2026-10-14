#!/usr/bin/env python3
"""drift_sentinel.py -- is every deployed OVMX surface running the latest tag?

Reads deploy/targets.yaml (the ONE inventory, rd epic vms-d385), probes each
target's LIVE version with a no-secrets, read-only request, and compares it
with the newest release tag in git. Any target that is not on that tag is
DRIFT. So is a target whose probe cannot read a version: a surface nobody
can check is the gap this tool exists to close.

    drift_sentinel.py [--manifest F] [--latest TAG] [--fixture F]
                      [--json OUT] [--sync-issues --repo R --run-url U]

Exit 0 when every target is current, 1 on any drift, 2 on a bad manifest.

--fixture replaces the network with a JSON map, so the tests (and the
negative control) run offline:
    {"urls": {"<url>": {"status": 200, "body": "..."}},
     "release_assets": ["vmlinuz", ...]}
--sync-issues (CI only) keeps one open GitHub issue per drifted target,
titled "drift: <id>", and closes it once the target is current again.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("drift_sentinel: needs PyYAML")

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TAG_RE = re.compile(r"^[vV]?(\d+(?:\.\d+)+)(?:-(\d+))?$")
PROBE_KINDS = {"text", "regex", "json", "release_asset", "served"}
ISSUE_LABEL = "deploy-drift"


def tag_key(tag):
    m = TAG_RE.match(tag)
    nums = tuple(int(x) for x in m.group(1).split("."))
    return nums + (int(m.group(2) or 0),)


def latest_release_tag(repo_root):
    out = subprocess.run(["git", "-C", repo_root, "tag", "-l"],
                         capture_output=True, text=True, check=True).stdout
    tags = [t for t in out.split() if TAG_RE.match(t)]
    if not tags:
        sys.exit("drift_sentinel: no release-shaped tag in git (fetch tags)")
    return max(tags, key=tag_key)


def norm(v):
    """V0.7-10, v0.7-10 and 0.7-10 are the same version."""
    v = (v or "").strip()
    return v[1:].lower() if v[:1] in "vV" else v.lower()


def load_manifest(path):
    with open(path) as f:
        doc = yaml.safe_load(f)
    targets = doc.get("targets") or []
    errs, seen = [], set()
    for i, t in enumerate(targets):
        tid = t.get("id") or f"#{i}"
        for k in ("id", "rd", "build", "deploy", "probe"):
            if k not in t:
                errs.append(f"{tid}: missing '{k}'")
        if tid in seen:
            errs.append(f"{tid}: duplicate id")
        seen.add(tid)
        p = t.get("probe") or {}
        if p.get("kind") not in PROBE_KINDS:
            errs.append(f"{tid}: probe.kind must be one of {sorted(PROBE_KINDS)}")
        if p.get("kind") == "served" and t.get("tracks") != "none":
            errs.append(f"{tid}: a 'served' probe proves no version; use it only with tracks: none")
    if errs:
        for e in errs:
            print(f"manifest: {e}", file=sys.stderr)
        sys.exit(2)
    return targets


class Net:
    """The network, or a fixture standing in for it."""

    def __init__(self, fixture=None, repo="3dl-dev/vms"):
        self.fx = fixture
        self.repo = repo

    def get(self, url):
        if self.fx is not None:
            r = self.fx.get("urls", {}).get(url)
            return (404, "") if r is None else (r.get("status", 200), r.get("body", ""))
        req = urllib.request.Request(url, headers={"User-Agent": "ovmx-drift-sentinel"})
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.status, resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            return e.code, ""
        except Exception as e:  # DNS, timeout, TLS: unreachable is not current
            return 0, str(e)

    def release_assets(self, tag):
        if self.fx is not None:
            return self.fx.get("release_assets", [])
        url = f"https://api.github.com/repos/{self.repo}/releases/tags/{tag}"
        hdr = {"User-Agent": "ovmx-drift-sentinel", "Accept": "application/vnd.github+json"}
        tok = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
        if tok:
            hdr["Authorization"] = f"Bearer {tok}"
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=hdr), timeout=30) as r:
                return [a["name"] for a in json.load(r).get("assets", [])]
        except Exception:
            return None


def probe(t, latest, net):
    """-> (live value or None, detail)."""
    p = t["probe"]
    kind = p["kind"]
    if kind == "release_asset":
        assets = net.release_assets(latest)
        if assets is None:
            return None, f"could not read release {latest}"
        hit = [a for a in assets if fnmatch.fnmatchcase(a, p["pattern"])]
        return (latest, hit[0]) if hit else (None, f"release {latest} has no asset matching {p['pattern']}")
    status, body = net.get(p["url"])
    if status != 200:
        return None, f"HTTP {status} from {p['url']}"
    if kind == "served":
        return "served", p["url"]
    if kind == "text":
        v = body.strip().splitlines()[0].strip() if body.strip() else ""
        return (v or None), p["url"]
    if kind == "regex":
        m = re.search(p["pattern"], body)
        return (m.group(1) if m else None), (p["url"] if m else f"no match for {p['pattern']} at {p['url']}")
    if kind == "json":
        try:
            v = json.loads(body)
            for part in p["path"].split("."):
                v = v[int(part)] if isinstance(v, list) else v[part]
            return str(v), p["url"]
        except Exception:
            return None, f"no '{p['path']}' in {p['url']}"
    return None, f"unknown probe kind {kind}"


def pin_reason(t, net):
    if not t.get("pin"):
        return None
    status, body = net.get(t["pin"])
    if status != 200:
        return None
    try:
        return json.loads(body).get("reason")
    except Exception:
        return None


def evaluate(targets, latest, net):
    rows = []
    for t in targets:
        live, detail = probe(t, latest, net)
        if t.get("tracks") == "none":
            state = "ok" if live == "served" else "unprobeable"
        elif live is None:
            state = "unprobeable"
        elif norm(live) == norm(latest):
            state = "ok"
        else:
            state = "drift"
        row = {"id": t["id"], "rd": t["rd"], "state": state, "live": live,
               "expected": None if t.get("tracks") == "none" else latest, "detail": detail}
        if state != "ok" and t.get("gated"):
            row["pin_reason"] = pin_reason(t, net)
        rows.append(row)
    return rows


def render(rows, latest):
    lines = [f"latest release tag: {latest}"]
    for r in rows:
        tag = {"ok": "OK   ", "drift": "DRIFT", "unprobeable": "NOVER"}[r["state"]]
        lines.append(f"  [{tag}] {r['id']}: live={r['live']} expected={r['expected']} ({r['detail']})")
        if r.get("pin_reason"):
            lines.append(f"          pinned: {r['pin_reason']}")
    bad = [r for r in rows if r["state"] != "ok"]
    lines.append(f"{len(bad)} of {len(rows)} target(s) not on {latest}" if bad
                 else f"all {len(rows)} targets on {latest}")
    return "\n".join(lines)


def gh(*args):
    return subprocess.run(["gh", *args], capture_output=True, text=True)


def sync_issues(rows, latest, repo, run_url):
    gh("label", "create", ISSUE_LABEL, "--repo", repo, "--color", "B60205",
       "--description", "A deploy target is not on the latest release tag (drift sentinel)")
    out = gh("issue", "list", "--repo", repo, "--label", ISSUE_LABEL, "--state", "open",
             "--limit", "200", "--json", "number,title")
    open_by_title = {i["title"]: i["number"] for i in json.loads(out.stdout or "[]")}
    for r in rows:
        title = f"drift: {r['id']}"
        num = open_by_title.get(title)
        if r["state"] == "ok":
            if num:
                gh("issue", "close", str(num), "--repo", repo,
                   "--comment", f"Now on {latest}. Sentinel run: {run_url}")
            continue
        body = (f"Deploy target `{r['id']}` is not on the latest release tag.\n\n"
                f"- expected: `{r['expected']}`\n- live: `{r['live']}`\n- probe: {r['detail']}\n"
                f"- rd: {r['rd']}\n- sentinel run: {run_url}\n")
        if r.get("pin_reason"):
            body += f"- pinned by its gate: {r['pin_reason']}\n"
        body += "\nInventory: `deploy/targets.yaml`. This issue closes itself when the target is current."
        if num:
            gh("issue", "comment", str(num), "--repo", repo, "--body", body)
        else:
            gh("issue", "create", "--repo", repo, "--title", title, "--label", ISSUE_LABEL, "--body", body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", default=os.path.join(ROOT, "deploy", "targets.yaml"))
    ap.add_argument("--latest", help="override the latest tag (default: newest release tag in git)")
    ap.add_argument("--fixture", help="JSON stand-in for the network (tests)")
    ap.add_argument("--json", help="also write the result rows here")
    ap.add_argument("--sync-issues", action="store_true")
    ap.add_argument("--repo", default="3dl-dev/vms")
    ap.add_argument("--run-url", default="")
    a = ap.parse_args()

    targets = load_manifest(a.manifest)
    latest = a.latest or latest_release_tag(ROOT)
    fx = json.load(open(a.fixture)) if a.fixture else None
    rows = evaluate(targets, latest, Net(fx, a.repo))
    print(render(rows, latest))
    if a.json:
        json.dump({"latest": latest, "targets": rows}, open(a.json, "w"), indent=1)
    if a.sync_issues:
        sync_issues(rows, latest, a.repo, a.run_url)
    return 1 if any(r["state"] != "ok" for r in rows) else 0


if __name__ == "__main__":
    sys.exit(main())
