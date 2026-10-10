#!/usr/bin/env python3
"""check_cache_writers.py - every GitHub Actions layer-cache scope has ONE writer
(rd vms-14da).

When several jobs export `cache-to: type=gha` into the same scope they overwrite
one another's cache index concurrently, the index ends up naming blobs no build
has touched lately, the repository's over-quota cache evicts those first, and a
later build fails on a cached layer whose blob is gone ("blob sha256:... not
found"). One writer per scope -- which also reads that scope first -- only ever
names blobs it just uploaded or just downloaded. Every other job imports only.

Fails when, across .github/workflows/*.yml, a scope (the default scope when none
is named) has more than one `cache-to: ... type=gha` step, when a matrix job
writes without restricting itself to one matrix entry (strategy.job-index), or
when a writer is not restricted to main.

  check_cache_writers.py [ROOT]          exit 0 clean, 1 on a violation
"""
import glob
import os
import re
import sys

import yaml


def main(argv):
    root = argv[1] if len(argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..")
    writers = {}
    bad = []
    for f in sorted(glob.glob(os.path.join(root, ".github", "workflows", "*.yml"))):
        y = yaml.safe_load(open(f)) or {}
        for jid, job in (y.get("jobs") or {}).items():
            strat = job.get("strategy")
            is_matrix = isinstance(strat, dict) and strat.get("matrix") is not None
            for st in job.get("steps") or []:
                ct = str((st.get("with") or {}).get("cache-to") or "")
                if "type=gha" not in ct:
                    continue
                m = re.search(r"scope=([\w.-]+)", ct)
                scope = m.group(1) if m else "(default)"
                where = "%s:%s" % (os.path.basename(f), jid)
                writers.setdefault(scope, []).append(where)
                if "refs/heads/main" not in ct:
                    bad.append("%s writes scope %s on every ref, not only main" % (where, scope))
                if is_matrix and "job-index" not in ct:
                    bad.append("%s is a matrix job and every entry writes scope %s" % (where, scope))
    for scope, ws in sorted(writers.items()):
        if len(ws) > 1:
            bad.append("scope %s has %d writers: %s" % (scope, len(ws), ", ".join(ws)))
    if bad:
        print("FAIL: layer-cache scopes without exactly one writer (rd vms-14da):")
        for b in bad:
            print("  " + b)
        return 1
    print("OK: %d layer-cache scope(s), one writer each" % len(writers))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
