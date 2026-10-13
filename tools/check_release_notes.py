#!/usr/bin/env python3
"""check_release_notes.py - a release's notes list exactly what the tag ships.

  check_release_notes.py --tag V0.7-9 --notes RELEASE-NOTES-0.7-9.md [--repo-root .]

The commit bullets under the generated sections (Features / Fixes /
Reverse-Engineering / Protocol) of the notes must be the same multiset as
`git log <previous release tag>..<tag> --no-merges` -- the commits the TAGGED
SHA contains, with the same attestation filter tools/gen_release_notes.py
applies. The previous tag is found the way the generator finds it.

Why (V0.7-9): the cut PR's notes were generated from the cut-branch commit,
and the merge queue folded two more PRs (#1683, #1684) into the commit that
was tagged. Notes must come from the tagged SHA -- release.yml generates them
with --ref <tag> -- and this check makes any other source a red publish.
Exit 0 when they match; 1 with the missing / extra subjects otherwise.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_release_notes as g  # noqa: E402

SECTIONS = ('## Features', '## Fixes', '## Reverse-Engineering / Protocol')


def listed_commits(text):
    out, on = [], False
    for line in text.splitlines():
        if line.startswith('## '):
            on = line.strip() in SECTIONS
            continue
        if on and line.startswith('- '):
            out.append(line[2:])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tag', required=True)
    ap.add_argument('--notes', required=True)
    ap.add_argument('--repo-root', default='.')
    a = ap.parse_args()

    since = g.previous_release_tag(a.repo_root, a.tag)
    want = collections.Counter(g.collect_commits(a.repo_root, since, a.tag))
    with open(a.notes, encoding='utf-8') as f:
        have = collections.Counter(listed_commits(f.read()))
    missing = want - have
    extra = have - want
    if not missing and not extra:
        print(f'check_release_notes: {a.notes} lists exactly the {sum(want.values())} '
              f'commits {since}..{a.tag} ships')
        return 0
    print(f'check_release_notes: FAIL: {a.notes} does not list what {a.tag} ships '
          f'({since}..{a.tag})', file=sys.stderr)
    for s in sorted(missing):
        print(f'  missing: {s}', file=sys.stderr)
    for s in sorted(extra):
        print(f'  extra:   {s}', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
