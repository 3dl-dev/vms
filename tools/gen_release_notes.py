#!/usr/bin/env python3
"""
gen_release_notes.py - generate OVMX release notes from merged history since
the previous release tag (vms-55a, epic vms-a84 RELEASE ENGINEERING).

THE PROBLEM THIS REPLACES: docs/release-notes-0.2.md was hand-written -- a
human read the git log, decided what mattered, and wrote prose. That is
exactly the kind of changelog this project's standing rule forbids
hand-maintaining once a tool can derive it: it silently drifts (a shipped
commit never makes it into the notes, or the version quoted in the notes
disagrees with what actually shipped) and nothing in CI can catch that.

WHAT THIS DOES, ground-sourced against git itself (no hand-fed data):

  1. Reads OVMX_PRODUCT_VERSION out of src/libvms/include/ovmx_identity.h at
     the requested --ref via `git show <ref>:<path>` -- the same identity
     single-source-of-truth (INV-1) tools/cut-release.sh reads. The version
     in the notes is therefore whatever the CUT commit actually says, never
     a literal typed into this script.
  2. Finds the previous release tag: the highest-version tag matching
     ^v?[0-9]+(\\.[0-9]+)+(-[0-9]+)?$ that is an ancestor of --ref (the
     optional -N suffix is a VMS-style point/maintenance release off an
     existing tag, e.g. 0.3-1 off 0.3 -- vms-1d28). This deliberately
     excludes non-release tags this repo also carries (e.g.
     wip/migration-*) -- those were never a shipped cut.
  3. Walks `git log <prev-tag>..<ref> --no-merges`, drops this repo's
     "Attest <sha>" attestation-record commits (metadata about a commit,
     not a change in it -- see the git log for what these look like), and
     buckets every remaining commit subject into Features / Fixes /
     Reverse-Engineering-and-Protocol by keyword rules on the subject line.
  4. Renders GitHub-flavored markdown, one bullet per commit subject,
     VERBATIM. This repo's commit convention already carries the rd item id
     and (after squash-merge) the PR number in the subject line itself --
     e.g. "product: kit-format seed-once metadata preserves site config on
     upgrade (vms-2c9) (#281)" -- so no further parsing of PR/issue numbers
     is attempted; the subject line already is the citation.

Deliberately produces NO wall-clock content (no "generated at ..." stamp):
tools/cut-release.sh's byte-reproducibility proof (the cut-release-
reproducible CI job) diffs two independent cuts of the SAME commit and
expects identical bytes, and a timestamp here would break that for no
benefit -- the git history a fixed --ref sees does not change between runs.

Never hand-edit the output; re-run this tool against the same --ref instead.
See tools/check_guide_drift.py for the sibling piece of this bead: guides
tied to gate scripts the same way these notes are tied to git history --
mechanically, not by hand.

Usage:
    tools/gen_release_notes.py [--ref REF] [--since TAG] [--out FILE]
                                [--repo-root DIR]

Exit 0 with notes written (stdout, or --out) on success; nonzero with a
diagnostic on failure (e.g. no identity header at --ref, --repo-root is not
a git repo).
"""
import argparse
import re
import subprocess
import sys

VERSION_RE = re.compile(r'^#define\s+OVMX_PRODUCT_VERSION\s+"([^"]*)"', re.M)
# Release tags are dotted-numeric (0.3, v1.0) with an OPTIONAL VMS-style
# point/maintenance suffix (-N, e.g. 0.3-1 -- vms-1d28, the first point
# release cut through this machinery). Without the optional group here, a
# point-release tag would never be recognized as a candidate "previous
# release" by previous_release_tag() below, so the NEXT release's notes
# would silently skip straight back to the last non-suffixed tag and
# re-list commits the point release already shipped.
RELEASE_TAG_RE = re.compile(r'^v?[0-9]+(\.[0-9]+)+(-[0-9]+)?$')
ATTEST_RE = re.compile(r'^Attest [0-9a-f]{40}$')

# Reverse-engineering / cluster-wire-protocol work: this project's commit
# convention marks these with a subject prefix naming the RE-facing module
# (scs:, cluster:, cluster-spec:, dlm:), a conventional-commit scope
# (feat(cluster): ...), or spells out "RE" explicitly in the subject.
PREFIX_RE = re.compile(r'^([a-zA-Z][a-zA-Z0-9_.-]*)(?:\(([a-zA-Z0-9_.-]+)\))?:')
RE_STEMS = {'scs', 'cluster', 'cluster-spec', 'dlm', 'nisca', 'mscp'}
RE_KEYWORDS = (' re —', ' re:', 'clean-room', 'wire protocol', 'nisca', 'mscp')

FIX_STEMS = {'fix', 'hotfix'}
FIX_KEYWORDS = ('regression', 'bugfix', ' fixes ', ' fixed ')


def subject_prefix(subject):
    """(type, scope) lowercased from a leading 'type:' or 'type(scope):'
    prefix, or ('', '') if the subject carries no such prefix."""
    m = PREFIX_RE.match(subject)
    if not m:
        return '', ''
    return m.group(1).lower(), (m.group(2) or '').lower()


def run(repo_root, *args, check=True):
    return subprocess.run(
        ['git', '-C', repo_root, *args],
        capture_output=True, text=True, check=check,
    )


def product_version(repo_root, ref):
    proc = run(repo_root, 'show', f'{ref}:src/libvms/include/ovmx_identity.h', check=False)
    if proc.returncode != 0:
        sys.exit(f"FATAL: could not read ovmx_identity.h at {ref}: {proc.stderr.strip()}")
    m = VERSION_RE.search(proc.stdout)
    if not m:
        sys.exit(f"FATAL: OVMX_PRODUCT_VERSION not found in ovmx_identity.h at {ref}")
    return m.group(1)


def previous_release_tag(repo_root, ref):
    tags = run(repo_root, 'tag', '-l').stdout.splitlines()
    candidates = [t for t in tags if RELEASE_TAG_RE.match(t)]

    def is_ancestor(tag):
        proc = run(repo_root, 'merge-base', '--is-ancestor', tag, ref, check=False)
        return proc.returncode == 0

    # A commit is its own ancestor, so a candidate tag that points at the SAME
    # commit as --ref would be picked as the "previous" release and yield an
    # empty range -- notes with zero commits. That is exactly what happens when
    # --ref IS a release tag: the tag-push publish path (.github/workflows/
    # release.yml checks out the tag and runs cut-release.sh --ref <tag>) and
    # any retroactive cut of an existing tag. Exclude tags resolving to --ref's
    # own commit; the previous release is the highest STRICT ancestor.
    ref_sha = run(repo_root, 'rev-parse', f'{ref}^{{commit}}').stdout.strip()

    def same_commit(tag):
        return run(repo_root, 'rev-parse', f'{tag}^{{commit}}').stdout.strip() == ref_sha

    ancestors = [t for t in candidates if not same_commit(t) and is_ancestor(t)]
    if not ancestors:
        return None

    def sort_key(tag):
        # Split off the optional -N point-release suffix BEFORE splitting on
        # '.' -- a naive tag.split('.') on "0.3-1" yields ['0', '3-1'], and
        # int('3-1') raises. A tag with no suffix sorts as suffix 0, so
        # "0.3" < "0.3-1" < "0.4" -- point releases of an existing tag rank
        # ahead of it but behind the next dotted release, matching how
        # cut-release.sh names them (vms-1d28).
        main, _, suffix = tag.partition('-')
        key = [int(p) for p in re.sub(r'^v', '', main).split('.')]
        key.append(int(suffix) if suffix else 0)
        return key

    return sorted(ancestors, key=sort_key)[-1]


def classify(subject):
    low = subject.lower()
    ptype, pscope = subject_prefix(subject)
    if ptype in RE_STEMS or pscope in RE_STEMS or any(k in low for k in RE_KEYWORDS):
        return 'Reverse-Engineering / Protocol'
    if ptype in FIX_STEMS or re.match(r'^fix\b', low) or any(k in low for k in FIX_KEYWORDS):
        return 'Fixes'
    return 'Features'


def collect_commits(repo_root, since, ref):
    range_spec = f'{since}..{ref}' if since else ref
    log = run(repo_root, 'log', range_spec, '--no-merges', '--format=%s').stdout
    return [s for s in log.splitlines() if s.strip() and not ATTEST_RE.match(s.strip())]


def render(version, since, subjects):
    buckets = {'Features': [], 'Fixes': [], 'Reverse-Engineering / Protocol': []}
    for s in subjects:
        buckets[classify(s)].append(s)

    lines = [f'# OVMX {version} Release Notes', '']
    lines.append(
        f'Generated by `tools/gen_release_notes.py` from the merged history since '
        f'`{since or "the beginning of history"}` ({len(subjects)} commits). '
        f'Do not hand-edit this file -- re-run the tool against the cut commit instead.'
    )
    lines.append('')
    for name in ('Features', 'Fixes', 'Reverse-Engineering / Protocol'):
        items = buckets[name]
        if not items:
            continue
        lines.append(f'## {name}')
        lines.append('')
        for s in items:
            lines.append(f'- {s}')
        lines.append('')
    if not subjects:
        lines.append('_No commits since the previous release tag._')
        lines.append('')
    return '\n'.join(lines).rstrip('\n') + '\n'


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ref', default='HEAD', help='git ref/commit being cut (default: HEAD)')
    ap.add_argument('--since', default=None,
                     help='override the previous-release tag (default: auto-detect the highest '
                          'release-shaped tag that is an ancestor of --ref)')
    ap.add_argument('--out', default=None, help='write notes here (default: stdout)')
    ap.add_argument('--repo-root', default='.', help='repo root to read git history from (default: .)')
    args = ap.parse_args()

    version = product_version(args.repo_root, args.ref)
    since = args.since or previous_release_tag(args.repo_root, args.ref)
    subjects = collect_commits(args.repo_root, since, args.ref)
    notes = render(version, since, subjects)

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as f:
            f.write(notes)
        print(f'gen_release_notes: wrote {args.out} ({len(subjects)} commits since '
              f'{since or "the beginning of history"})', file=sys.stderr)
    else:
        sys.stdout.write(notes)
    return 0


if __name__ == '__main__':
    sys.exit(main())
