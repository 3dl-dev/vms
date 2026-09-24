#!/usr/bin/env python3
"""CI guard: fail if any GitHub Actions workflow file is near GitHub's
silent size ceiling.

Background (rd vms-1af): GitHub silently refuses to run a workflow file once
it crosses a size ceiling somewhere around 511,986-512,030 raw bytes. The run
is created and immediately ends with conclusion=startup_failure and ZERO
jobs; a pull_request event produces NO RUN AT ALL. There is no error
surfaced anywhere in the UI or API for this -- the only tell is the run's
display name reverting to the file path. ci.yml hit exactly this at 511,343
bytes and had to be split into the ci-*.yml siblings.

NOTE on the "non-comment content" theory: vms-1af's bisection also observed
that padding the file with 657 bytes of pure comments (511,343 -> 512,000
raw bytes) still ran. That is NOT evidence that GitHub excludes comments from
whatever it measures -- 512,000 raw bytes is still under the demonstrated
512,030-byte failure point, so that data point is equally consistent with a
plain raw-byte ceiling. This checker therefore guards on RAW FILE SIZE, not
a comment-stripped approximation: raw bytes are a strict upper bound on
whatever GitHub actually measures, so this guard can only trip earlier than
the real ceiling, never later.

Usage:
    check_workflow_sizes.py [--dir DIR] [--threshold BYTES] [--quiet]

Exit status: 0 if every *.yml/*.yaml file in DIR is at or under the
threshold, 1 otherwise (with the offending file(s) and their sizes printed
to stderr).
"""
import argparse
import glob
import os
import sys

DEFAULT_THRESHOLD = 450_000


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default=".github/workflows",
                     help="directory of workflow files to check (default: .github/workflows)")
    ap.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD,
                     help=f"max allowed raw bytes per file (default: {DEFAULT_THRESHOLD})")
    ap.add_argument("--quiet", action="store_true",
                     help="only print output for files over threshold")
    args = ap.parse_args(argv)

    paths = sorted(
        p for pat in ("*.yml", "*.yaml")
        for p in glob.glob(os.path.join(args.dir, pat))
    )
    if not paths:
        print(f"no workflow files found under {args.dir}", file=sys.stderr)
        return 1

    failed = []
    for path in paths:
        size = os.path.getsize(path)
        over = size > args.threshold
        if over or not args.quiet:
            print(f"{'FAIL' if over else 'ok  '} {path}: {size} bytes, threshold {args.threshold}")
        if over:
            failed.append((path, size))

    if failed:
        print(file=sys.stderr)
        print(f"{len(failed)} workflow file(s) over the {args.threshold}-byte "
              "guard threshold (rd vms-1af: GitHub silently drops runs of "
              "files over ~512000 bytes with conclusion=startup_failure and "
              "ZERO jobs). Split the file further before merging.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
