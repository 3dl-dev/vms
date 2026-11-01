"""Self-tests for the compat drift gate (tools/compat/render_compat.py --check).

The churn fix: a compat PR edits ONLY docs/compat/*.yaml; the tracked
docs/compatibility-surface.md is a dated snapshot refreshed by --publish. So the
gate must (a) pass a YAML-only change that leaves the md alone, (b) red a change
that hand-edits the md, (c) pass a --publish refresh, (d) red YAML that does not
validate, and (e) never rewrite the tracked md on a plain render.

Each case runs the real tool inside a throwaway git repo (copied tool + the real
docs/compat source), so `git diff <base>` is exercised for real, not mocked.
"""
import os
import shutil
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MD = "docs/compatibility-surface.md"

import yaml  # noqa: F401,E402  -- absent PyYAML is a failure, never a skip


def git(cwd, *args):
    return subprocess.run(["git", "-C", cwd, *args], check=True,
                          capture_output=True, text=True).stdout.strip()


@pytest.fixture
def repo(tmp_path):
    root = str(tmp_path / "r")
    os.makedirs(os.path.join(root, "tools", "compat"))
    shutil.copy(os.path.join(REPO, "tools", "compat", "render_compat.py"),
                os.path.join(root, "tools", "compat"))
    shutil.copytree(os.path.join(REPO, "docs", "compat"),
                    os.path.join(root, "docs", "compat"))
    git(root, "init", "-q")
    git(root, "config", "user.email", "t@example.invalid")
    git(root, "config", "user.name", "t")
    # The snapshot as a release bump would leave it.
    run(root, "--publish", check=True)
    git(root, "add", "-A")
    git(root, "commit", "-qm", "base")
    return root


def run(root, *args, check=False):
    r = subprocess.run([sys.executable, os.path.join(root, "tools/compat/render_compat.py"),
                        *args], capture_output=True, text=True)
    if check:
        assert r.returncode == 0, r.stderr
    return r


def first_yaml(root):
    d = os.path.join(root, "docs", "compat", "facilities")
    return os.path.join(d, sorted(os.listdir(d))[0])


def bump_a_status(root):
    """A real YAML-only change: flip one item's status so the render differs."""
    p = first_yaml(root)
    s = open(p).read()
    for a, b in (("status: partial", "status: implemented"),
                 ("status: implemented", "status: partial")):
        if a in s:
            open(p, "w").write(s.replace(a, b, 1))
            return
    pytest.fail("no status line to flip in " + p)


def test_yaml_only_change_passes_without_regenerating(repo):
    base = git(repo, "rev-parse", "HEAD")
    bump_a_status(repo)
    git(repo, "commit", "-qam", "yaml only")
    r = run(repo, "--check", "--base", base)
    assert r.returncode == 0, r.stderr
    assert "not touched" in r.stdout


def test_hand_edited_md_reds(repo):
    # negative control: the md is edited, the YAML is not.
    base = git(repo, "rev-parse", "HEAD")
    p = os.path.join(repo, MD)
    s = open(p).read()
    open(p, "w").write(s.replace("| ✅ verified |", "| ✅ verified | 999 |", 1))
    git(repo, "commit", "-qam", "hand edit")
    r = run(repo, "--check", "--base", base)
    assert r.returncode == 1
    assert "DRIFT" in r.stderr


def test_stale_snapshot_in_a_change_that_touches_it_reds(repo):
    # negative control: YAML changed AND the md touched, but not re-rendered from it.
    base = git(repo, "rev-parse", "HEAD")
    bump_a_status(repo)
    with open(os.path.join(repo, MD), "a") as fh:
        fh.write("\n")
    git(repo, "commit", "-qam", "stale md")
    assert run(repo, "--check", "--base", base).returncode == 1


def test_publish_refresh_passes(repo):
    base = git(repo, "rev-parse", "HEAD")
    bump_a_status(repo)
    run(repo, "--publish", check=True)
    git(repo, "commit", "-qam", "regen")
    assert "docs/compatibility-surface.md" in git(repo, "diff", "--name-only", base)
    r = run(repo, "--check", "--base", base)
    assert r.returncode == 0, r.stderr
    assert "matches a fresh render" in r.stdout


def test_plain_render_does_not_touch_tracked_md(repo):
    bump_a_status(repo)
    run(repo, check=True)
    assert git(repo, "status", "--porcelain", "--", MD) == ""
    assert os.path.exists(os.path.join(repo, "build", "compatibility-surface.md"))
    assert os.path.exists(os.path.join(repo, "build", "compat-surface.json"))


def test_invalid_yaml_reds(repo):
    p = first_yaml(repo)
    s = open(p).read()
    open(p, "w").write(s.replace("status: ", "status: not-a-status-", 1))
    assert run(repo, "--check").returncode == 2
