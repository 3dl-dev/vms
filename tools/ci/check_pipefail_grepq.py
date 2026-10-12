#!/usr/bin/env python3
"""check_pipefail_grepq.py - no `PRODUCER | grep -q` in a pipefail script.

Under `set -o pipefail`, `PRODUCER | grep -q PAT` fails when grep finds PAT
early and exits: the producer is still writing, takes SIGPIPE, and the
pipeline's status is 141 -- a MATCH reported as a MISS. It depends on timing
and on how much text there is (a 200000-line $out: 50/50 false misses), which
made vmsdecnet_ncp_executor red intermittently on CI with "Executor node = 1.42
(OVMX)" right there in the output. Any producer does it: echo/printf of a
variable, tail -c of a console log, readelf, kubectl (rd vms-deb0).

Fix by cause: let grep read all of its input -- `PRODUCER | grep >/dev/null PAT`
-- or take the producer out of the pipeline: `grep -q PAT <<<"$VAR"`.

A line that pipes into grep -q ON PURPOSE (a disproof that the old form still
fails) carries the marker `pipefail-grepq: deliberate` in a comment on that
line or on the line just above it.

usage: check_pipefail_grepq.py [repo-root]    (exit 0 clean, 1 found)
       check_pipefail_grepq.py --selftest     (the gate can go red)
"""
import re
import subprocess
import sys

BAD = re.compile(r'\|\s*grep\s+-[A-Za-z]*q')
MARK = "pipefail-grepq: deliberate"


def scan(name, text):
    """Return the offending lines of one script's text."""
    if "pipefail" not in text:
        return []
    hits = []
    lines = text.split("\n")
    for n, line in enumerate(lines, 1):
        prev = lines[n - 2] if n > 1 else ""
        if line.lstrip().startswith("#") or MARK in line or MARK in prev:
            continue
        if BAD.search(line):
            hits.append("%s:%d: %s" % (name, n, line.strip()))
    return hits


def selftest():
    cases = [
        ('set -o pipefail\necho "$out" | grep -q x\n', 1),
        ('set -o pipefail\ntail -c +5 "$LOG" | grep -qF -- "$p" && ok\n', 1),
        ('set -o pipefail\nprintf %s "$v" | grep -Eq "a|b"\n', 1),
        ('set -o pipefail\nprintf %s "$v" | grep >/dev/null -F x\n', 0),
        ('set -o pipefail\ngrep -q x <<<"$v"\n', 0),
        ('set -o pipefail\n# echo "$v" | grep -q x\n', 0),
        ('set -o pipefail\necho "$v" | grep -q x  # pipefail-grepq: deliberate\n', 0),
        ('set -o pipefail\n# pipefail-grepq: deliberate\necho "$v" | grep -q x \\\n', 0),
        ('echo "$v" | grep -q x\n', 0),   # no pipefail: grep -q is harmless
    ]
    bad = 0
    for text, want in cases:
        got = len(scan("case", text))
        if got != want:
            print("SELFTEST FAIL: %r -> %d hit(s), want %d" % (text, got, want))
            bad = 1
    print("selftest: %s" % ("FAIL" if bad else "ok"))
    return bad


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        return selftest()
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    files = subprocess.run(["git", "-C", root, "ls-files", "*.sh"],
                           capture_output=True, text=True, check=True).stdout.split()
    hits = []
    for f in files:
        try:
            text = open("%s/%s" % (root, f), errors="replace").read()
        except OSError:
            continue
        hits += scan(f, text)
    for h in hits:
        print("PIPEFAIL GREP -Q: " + h)
    if hits:
        print("FAIL: %d `... | grep -q` under pipefail -- use `... | grep >/dev/null PAT`"
              " or `grep -q PAT <<<\"$VAR\"`" % len(hits))
        return 1
    print("ok: no pipe into grep -q under pipefail")
    return 0


if __name__ == "__main__":
    sys.exit(main())
