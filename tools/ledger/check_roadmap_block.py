#!/usr/bin/env python3
"""Per-PR roadmap drift gate (INV-LEDGER, vms-8465) -- the WEAK, rd-free check.

WHY THIS IS NOT `reconcile.py --check`
    The roadmap GENERATED block derives from LIVE rd (rd items + rel-* labels +
    gate-epic rollups), and rd is NOT in the repo -- it mutates continuously as
    other sessions close items. Running `reconcile.py --check` in per-PR CI would
    regenerate from CURRENT rd and red on drift the PR author never caused. That
    is unacceptable: it would red every innocent PR. So the LIVE-rd reconcile is
    NOT a per-PR gate -- it runs where rd lives (the conductor's checkpoint,
    vms-8747; and the schedule/dispatch job in .github/workflows/ledger.yml).

WHAT THIS CHECKS INSTEAD (all repo-local, no rd, no false-reds on rd drift):
    1. Marker/structural integrity: exactly one GENERATED:BEGIN and one
       GENERATED:END (in order), the block is non-empty, and it carries the
       "do not edit by hand / run tools/roadmap/reconcile.py" provenance line.
       This proves the block was PRODUCED BY THE TOOL and not hand-mangled.
    2. Shipped-releases sub-list matches `git tag`: the releases section is
       derived from git tags (repo-local), so it CAN be checked per-PR. A
       hand-added / removed / reordered release entry reds.

    STALE BRANCHES (vms-aaf): a release tag + its reconcile land on main AFTER
    open PRs branched, so comparing a PR's block to LIVE `git tag` redded every
    open PR on each release. With `--base REF` (pull_request / merge_group) the
    gate first compares the checked-out (merge-result) release list to REF's: if
    the change under test did not touch the list, it cannot have caused drift,
    and it passes. If it DID touch the list, the list must equal `git tag` --
    so a hand-edit in a PR still reds. Without `--base` (push to main) the check
    is strict: a drifted main always reds.

    Together these catch the M2 failure mode (hand-editing a derived surface) on
    the repo-local parts, without ever comparing the rd-derived numbers to live
    rd. A subtle hand-edit of an rd-derived count is caught by the scheduled
    regen (it will be overwritten and the diff surfaced), not here.

Exit 0 = clean; non-zero = drift.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "roadmap"))

import reconcile  # noqa: E402  (repo-local, stdlib-only)

DOC = reconcile.ROADMAP_DOC
GEN_BEGIN = reconcile.GEN_BEGIN
GEN_END = reconcile.GEN_END
PROVENANCE = "run tools/roadmap/reconcile.py"


def fail(msg: str) -> None:
    print(f"ROADMAP-BLOCK DRIFT: {msg}", file=sys.stderr)
    print("  The GENERATED block is machine-output. Do not hand-edit it; run\n"
          "      python3 tools/roadmap/reconcile.py\n"
          "  (INV-LEDGER, vms-8465).", file=sys.stderr)
    sys.exit(1)


def release_tags_of(text: str) -> list[str]:
    """Structural checks on the roadmap doc text; returns its release list."""
    # 1. marker/structural integrity
    n_begin = text.count(GEN_BEGIN)
    n_end = text.count(GEN_END)
    if n_begin != 1 or n_end != 1:
        fail(f"expected exactly one GENERATED:BEGIN and one GENERATED:END marker "
             f"in {os.path.relpath(DOC, REPO)}; found begin={n_begin} end={n_end}.")
    i_begin = text.index(GEN_BEGIN)
    i_end = text.index(GEN_END)
    if i_begin >= i_end:
        fail("GENERATED:BEGIN must precede GENERATED:END.")
    block = text[i_begin:i_end + len(GEN_END)]
    if PROVENANCE not in block:
        fail("generated block is missing its provenance line "
             f"('{PROVENANCE}') -- someone replaced the tool header.")
    # the block must carry the derived tables (non-trivial output)
    if "### Milestone ladder" not in block or "### Shipped releases" not in block:
        fail("generated block is missing its derived sections "
             "(milestone ladder / shipped releases) -- not tool output.")
    m = re.search(r"### Shipped releases[^\n]*\n(.*?)(?:\n### |\n<!-- GENERATED:END)",
                  block, re.S)
    if not m:
        fail("could not locate the 'Shipped releases' sub-list in the block.")
    return re.findall(r"^- \*\*(.+?)\*\* —", m.group(1), re.M)


def base_release_tags(base: str) -> list[str] | None:
    """The release list at BASE, or None if BASE has no parseable block."""
    rel = os.path.relpath(DOC, REPO)
    r = subprocess.run(["git", "-C", REPO, "show", f"{base}:{rel}"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    m = re.search(r"### Shipped releases[^\n]*\n(.*?)(?:\n### |\n<!-- GENERATED:END)",
                  r.stdout, re.S)
    if not m:
        return None
    return re.findall(r"^- \*\*(.+?)\*\* —", m.group(1), re.M)


def verdict(block_tags: list[str], git_tags: list[str],
            base_tags: list[str] | None) -> str | None:
    """None = clean; otherwise the reason it is drift.

    base_tags is None in strict mode (push to main). When given, an unchanged
    release list passes: the change under test did not touch it.
    """
    if base_tags is not None and block_tags == base_tags:
        return None
    if block_tags != git_tags:
        return "'Shipped releases' does not match `git tag`."
    return None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", help="pull_request/merge_group base SHA: pass when "
                    "the release list is unchanged from it (vms-aaf)")
    args = ap.parse_args(argv)

    with open(DOC) as fh:
        text = fh.read()
    block_tags = release_tags_of(text)

    # 2. shipped-releases sub-list matches git tags (repo-local -> checkable)
    git_tags = [r["tag"] for r in reconcile.git_releases()[:12]]
    base_tags = base_release_tags(args.base) if args.base else None
    why = verdict(block_tags, git_tags, base_tags)
    if why:
        print(f"ROADMAP-BLOCK DRIFT: {why}", file=sys.stderr)
        print(f"  in block: {block_tags}", file=sys.stderr)
        print(f"  git tags: {git_tags}", file=sys.stderr)
        if base_tags is not None:
            print(f"  base {args.base}: {base_tags} (this change edited the list)",
                  file=sys.stderr)
        print("  Releases derive from git tags (repo-local). Regenerate:\n"
              "      python3 tools/roadmap/reconcile.py\n"
              "  Never hand-edit the release list (INV-LEDGER, vms-8465).",
              file=sys.stderr)
        sys.exit(1)

    if base_tags is not None and block_tags == base_tags and block_tags != git_tags:
        print(f"clean: release list unchanged from base {args.base}; it lags "
              f"`git tag` ({git_tags[0] if git_tags else '-'}), which main's own "
              f"release reconcile fixes and the push gate enforces.")
    else:
        print(f"clean: roadmap GENERATED block is well-formed; "
              f"{len(block_tags)} shipped releases match `git tag`.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
