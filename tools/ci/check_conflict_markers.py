#!/usr/bin/env python3
"""check_conflict_markers.py - no tracked file carries a merge-conflict marker.

A conflict resolution that leaves a marker line behind (<<<<<<<, |||||||,
=======, >>>>>>> at the start of a line, followed by a space or the end of the
line) ships a corrupted file: docs/clean-room/reference-captures.sha256 carried
a stray diff3 base marker from #1383 (vms-f29) for days. This fails on any
such line in any tracked text file.

usage: check_conflict_markers.py [repo-root]     (exit 0 clean, 1 found)
       check_conflict_markers.py --self-test

EXCLUSIONS: binary files (git grep -I). A tracked file that must contain a
marker line on purpose (a fixture for a merge tool) goes in ALLOW below, by
path, with the reason.
"""
import re
import subprocess
import sys

MARKER = re.compile(r'^(<{7}|\|{7}|={7}|>{7})( |$)')
ALLOW = {
    # "path": "why this file contains a marker line on purpose",
}


def scan(root):
    r = subprocess.run(
        ['git', '-C', root, 'grep', '-nIE', r'^(<{7}|\|{7}|={7}|>{7})( |$)'],
        capture_output=True, text=True)
    if r.returncode not in (0, 1):
        sys.stderr.write(r.stderr)
        sys.exit(2)
    hits = []
    for line in r.stdout.splitlines():
        path, _, rest = line.partition(':')
        if path in ALLOW:
            continue
        hits.append(line)
    return hits


def self_test():
    must = ['<<<<<<< HEAD', '|||||||', '||||||| parent of 3282a7ff (x)', '=======',
            '>>>>>>> 1f2952704 (wip)']
    mustnot = ['========', '==========================================', ' =======',
               '<<<<<<<<', 'a <<<<<<< b', '=======x', '>>>>>>>>%d']
    bad = [s for s in must if not MARKER.match(s)] + [s for s in mustnot if MARKER.match(s)]
    for s in bad:
        print('SELF-TEST FAIL: %r' % s)
    print('self-test: %s' % ('FAIL' if bad else 'ok'))
    return 1 if bad else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--self-test':
        return self_test()
    root = sys.argv[1] if len(sys.argv) > 1 else '.'
    hits = scan(root)
    for h in hits:
        print('CONFLICT MARKER: %s' % h)
    if hits:
        print('FAIL: %d merge-conflict marker line(s) in tracked files -- finish the '
              'conflict resolution (or, for a deliberate fixture, add it to ALLOW '
              'with the reason).' % len(hits))
        return 1
    print('ok: no merge-conflict marker in any tracked text file')
    return 0


if __name__ == '__main__':
    sys.exit(main())
