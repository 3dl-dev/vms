#!/usr/bin/env python3
"""check_pipefail_grepq.py - no `echo "$VAR" | grep -q` in a pipefail script.

Under `set -o pipefail`, `echo "$out" | grep -q PAT` fails when grep finds PAT
early and exits: echo is still writing, takes SIGPIPE, and the pipeline's status
is 141 -- a MATCH reported as a MISS. It depends on timing and on how much text
there is (a 200000-line $out: 50/50 false misses), which made
vmsdecnet_ncp_executor red intermittently on CI with "Executor node = 1.42
(OVMX)" right there in the output. Use a here-string instead:
`grep -q PAT <<<"$out"`.

usage: check_pipefail_grepq.py [repo-root]    (exit 0 clean, 1 found)
"""
import re
import subprocess
import sys

BAD = re.compile(r'echo\s+"\$\{?[A-Za-z_][A-Za-z0-9_]*\}?"\s*\|\s*grep\s+-[A-Za-z]*q')


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    files = subprocess.run(["git", "-C", root, "ls-files", "*.sh"],
                           capture_output=True, text=True, check=True).stdout.split()
    hits = []
    for f in files:
        try:
            text = open("%s/%s" % (root, f), errors="replace").read()
        except OSError:
            continue
        if "pipefail" not in text:
            continue
        for n, line in enumerate(text.split("\n"), 1):
            if line.lstrip().startswith("#"):
                continue
            if BAD.search(line):
                hits.append("%s:%d: %s" % (f, n, line.strip()))
    for h in hits:
        print("PIPEFAIL GREP -Q: " + h)
    if hits:
        print("FAIL: %d `echo \"$VAR\" | grep -q` under pipefail -- use `grep -q PAT <<<\"$VAR\"`" % len(hits))
        return 1
    print("ok: no echo-into-grep -q under pipefail")
    return 0


if __name__ == "__main__":
    sys.exit(main())
