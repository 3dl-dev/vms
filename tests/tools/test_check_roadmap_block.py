"""Self-tests for the per-PR roadmap-block gate (tools/ledger/check_roadmap_block.py).

vms-aaf: a release tag + its reconcile land on main after open PRs branched, and
the gate used to compare every PR's release list to LIVE `git tag` -- so every
release redded every open PR. With --base the gate passes a change that did not
touch the release list, and still reds one that did (a hand-edit). Push to main
(no --base) stays strict, so a drifted main still reds.

The end-to-end cases run the real checker inside a throwaway git repo (copied
tool + doc, synthetic tags), so the negative controls exercise real `git tag`
and `git show <base>:<doc>`, not a mock.
"""
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "ledger"))

import check_roadmap_block as crb  # noqa: E402

DOC_REL = "docs/release-roadmap-to-1.0.md"


# --- verdict(): the decision table -------------------------------------------

def test_stale_branch_unchanged_list_passes():
    # PR branched before V0.7-10; main has the tag; PR did not touch the list.
    assert crb.verdict(["V0.7-9"], ["V0.7-10", "V0.7-9"], ["V0.7-9"]) is None


def test_hand_edit_in_pr_reds():
    # negative control: the PR changed the list to something git tag disagrees with.
    assert crb.verdict(["V0.7-11", "V0.7-9"], ["V0.7-10", "V0.7-9"], ["V0.7-9"])


def test_reconcile_pr_matching_tags_passes():
    assert crb.verdict(["V0.7-10", "V0.7-9"], ["V0.7-10", "V0.7-9"], ["V0.7-9"]) is None


def test_strict_main_drift_reds():
    # negative control: push to main (no base) with a list lagging git tag.
    assert crb.verdict(["V0.7-9"], ["V0.7-10", "V0.7-9"], None)


def test_strict_clean_passes():
    assert crb.verdict(["V0.7-9"], ["V0.7-9"], None) is None


# --- end-to-end in a throwaway git repo --------------------------------------

def _git(cwd, *args):
    subprocess.run(["git", "-C", str(cwd), *args], check=True,
                   capture_output=True, text=True)


def _set_releases(doc_text, tags):
    head, rest = doc_text.split("### Shipped releases", 1)
    title_line, body = rest.split("\n", 1)
    # drop the existing list items, keep whatever follows them
    lines = body.split("\n")
    i = 0
    while i < len(lines) and (lines[i].startswith("- **") or lines[i] == ""):
        i += 1
    items = [f"- **{t}** — Point release." for t in tags]
    return (head + "### Shipped releases" + title_line + "\n\n"
            + "\n".join(items) + "\n\n" + "\n".join(lines[i:]))


@pytest.fixture
def repo(tmp_path):
    r = tmp_path / "r"
    for rel in ("tools/ledger/check_roadmap_block.py", "tools/roadmap/reconcile.py",
                DOC_REL):
        (r / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(os.path.join(REPO, rel), r / rel)
    _git(r.parent, "init", "-q", str(r))
    _git(r, "config", "user.email", "t@example.invalid")
    _git(r, "config", "user.name", "t")
    doc = r / DOC_REL
    doc.write_text(_set_releases(doc.read_text(), ["V0.1-2", "V0.1-1"]))
    _git(r, "add", "-A")
    _git(r, "commit", "-qm", "base")
    _git(r, "tag", "V0.1-1")
    _git(r, "tag", "V0.1-2")
    return r


def _check(r, *args):
    return subprocess.run([sys.executable, str(r / "tools/ledger/check_roadmap_block.py"),
                           *args], capture_output=True, text=True)


def test_e2e_strict_clean(repo):
    p = _check(repo)
    assert p.returncode == 0, p.stderr


def test_e2e_new_tag_strict_reds_but_stale_pr_passes(repo):
    base = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    _git(repo, "tag", "V0.1-3")  # the release lands; the block does not list it yet
    strict = _check(repo)
    assert strict.returncode != 0 and "ROADMAP-BLOCK DRIFT" in strict.stderr
    stale = _check(repo, "--base", base)
    assert stale.returncode == 0, stale.stderr


def test_e2e_hand_edit_with_base_reds(repo):
    base = subprocess.run(["git", "-C", str(repo), "rev-parse", "HEAD"],
                          capture_output=True, text=True, check=True).stdout.strip()
    doc = repo / DOC_REL
    doc.write_text(_set_releases(doc.read_text(), ["V0.1-9", "V0.1-2", "V0.1-1"]))
    p = _check(repo, "--base", base)
    assert p.returncode != 0 and "edited the list" in p.stderr
