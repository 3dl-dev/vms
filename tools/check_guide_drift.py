#!/usr/bin/env python3
"""
check_guide_drift.py - assert docs/install-guide.md and docs/upgrade-guide.md
cannot silently drift from the e2e gates that actually prove them
(vms-55a, epic vms-a84 RELEASE ENGINEERING).

THE PROBLEM THIS REPLACES: before this bead, OVMX had no install/upgrade
guides at all -- and any prose a human wrote by hand could describe a
procedure that no longer matches what tests/qemu/test_product_install_e2e.sh
/ tests/qemu/test_upgrade_e2e.sh actually drive under QEMU. A guide that
"looks tested" but isn't checked against the gate is exactly the LARP shape
this repo's authenticity invariants exist to kill, applied to documentation.

THE MECHANISM: the gate scripts are the single source of truth for the DCL
command sequence. Every `send '<command>'` line in a gate script that is
part of the real install/upgrade procedure (as opposed to setup scaffolding,
negative-control probes like PRODUCT CONFIGURE FOO, or read-only
verification like TYPE/DIRECTORY) carries a trailing `# GUIDE-STEP` comment.
This script:

  1. Extracts the ordered list of GUIDE-STEP-annotated commands from the
     gate script (the ground truth -- what actually ran under QEMU).
  2. Extracts the ordered list of DCL commands from the guide's own
     <!-- ovmx:guide-steps:begin/end --> fenced code block (what the guide
     tells a human to type).
  3. Diffs them. Any difference -- reordering, a dropped step, a step the
     guide invented that the gate never ran, or so much as one character of
     drift in a command string -- is a FAIL, not a warning.

This is intentionally NOT prose-similarity or fuzzy matching: an install
guide that types a subtly wrong qualifier is worse than a guide that admits
it might be out of date. See tests/integration/test_guide_drift.sh for the
real-guide gate and tests/integration/test_guide_drift_negctl.sh for the
negative control that proves this comparator can actually go red.

Usage:
    tools/check_guide_drift.py --gate GATE_SCRIPT --guide GUIDE_MD

Exit 0 = the guide's GUIDE-STEPS block is byte-identical, in order, to the
gate's GUIDE-STEP commands. Exit 1 = drift detected (diagnostic printed).
Exit 2 = a fixture problem (no GUIDE-STEP lines in the gate, or no
GUIDE-STEPS block in the guide) -- this is not the same as "drift found":
it means one side of the comparison could not even be built.
"""
import argparse
import re
import sys

GUIDE_STEP_RE = re.compile(r"send\s+'((?:[^'\\])*)'.*#\s*GUIDE-STEP")
GUIDE_BLOCK_RE = re.compile(
    r"<!--\s*ovmx:guide-steps:begin\s*-->\s*```(?:[a-zA-Z0-9_-]*)\n(.*?)```\s*<!--\s*ovmx:guide-steps:end\s*-->",
    re.S,
)


def extract_gate_steps(path):
    steps = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = GUIDE_STEP_RE.search(line)
            if m:
                steps.append(m.group(1))
    return steps


def extract_guide_steps(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = GUIDE_BLOCK_RE.search(text)
    if not m:
        print(
            f"FATAL: {path} has no <!-- ovmx:guide-steps:begin --> ... "
            f"<!-- ovmx:guide-steps:end --> fenced code block", file=sys.stderr)
        return None
    steps = []
    for raw in m.group(1).splitlines():
        line = raw.strip()
        if not line or line.startswith("!"):
            continue
        if line.startswith("$"):
            line = line[1:].strip()
        steps.append(line)
    return steps


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gate", required=True, help="e2e gate script that is the ground truth")
    ap.add_argument("--guide", required=True, help="guide markdown to check for drift")
    args = ap.parse_args()

    gate_steps = extract_gate_steps(args.gate)
    if not gate_steps:
        print(f"FATAL: no '# GUIDE-STEP' annotated send lines found in {args.gate} "
              f"-- nothing to compare the guide against", file=sys.stderr)
        return 2

    guide_steps = extract_guide_steps(args.guide)
    if guide_steps is None:
        return 2

    if gate_steps == guide_steps:
        print(f"OK: {args.guide} matches all {len(gate_steps)} GUIDE-STEP command(s) "
              f"in {args.gate}, in order")
        return 0

    print(f"FAIL: {args.guide} has DRIFTED from {args.gate}", file=sys.stderr)
    print(f"--- gate ground truth ({args.gate}) ---", file=sys.stderr)
    for s in gate_steps:
        print(f"  $ {s}", file=sys.stderr)
    print(f"--- guide as written ({args.guide}) ---", file=sys.stderr)
    for s in guide_steps:
        print(f"  $ {s}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
