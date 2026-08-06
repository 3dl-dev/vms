#!/usr/bin/env python3
"""check_divider_integrity.py -- catch git's 3-way auto-merge silently
truncating/fusing comment-divider blocks (rd vms-6d7).

WHAT THIS GUARDS AGAINST. vms-371's rebase (2e30da9, squash-merged as
2dcd237) silently corrupted 3 unrelated comment-divider blocks in
tools/cluster/scs_join_capability_measure.py -- OUTSIDE the region git
reported as needing manual resolution, inside the portion git labelled
"Auto-merging..." and treated as clean. Each site: a box-divider line like

    # ===========================================================================

lost its trailing newline and fused onto the next comment line, AND the
equals-run itself was truncated (68 chars vs the file's other dividers at 75)
-- genuine content corruption, not merely a whitespace nit. It sat on main for
~30 minutes undetected: py_compile only checks syntax (comments are invisible
to it), and this repo's wire/data tests validate logic via string search on a
markdown spec file, never comment formatting. No mutant-testing or
self_veracity machinery in this codebase would have caught it either -- it is
specific to how git's automatic 3-way merge mishandles repeated-character
divider lines, not to any implementer's test choices.

WHAT THIS CHECKS, mechanically, over .py/.c/.h files:

  1. FUSION -- a divider run (>=3 repeats of one of `=-#*~` after a comment
     marker) immediately followed on the SAME physical line by more content
     that is not just the same repeated character and not just a comment
     closer (`*/`) or trailing whitespace. That is the exact fused shape: the
     divider's line and the next comment line's text landed on one line
     because a newline went missing.

  2. TRUNCATION -- among a file's PURE divider lines (the entire line, after
     the comment marker, is one repeated character and nothing else) grouped
     by (comment marker, repeated character), any divider whose run length is
     SHORTER than the group's most common ("mode") length. A single file's
     box dividers are drawn to a consistent width; a shorter one at the same
     site is the truncation half of this bug's signature. A LONGER divider is
     not flagged -- widening a divider is not a corruption shape this bug (or
     git's 3-way merge in general) produces, and flagging it would just be
     noise on legitimate mixed-width banners.

This is a mechanical, static, no-lab, no-rd check. It is deliberately narrow:
it does not try to be a general style linter, only to catch the fused/
truncated divider shape observed above. See test_divider_integrity_selftest.py
for the proof (synthetic corrupted fixture must flag; synthetic clean fixture
must not).
"""
import argparse
import re
import sys
from collections import Counter, namedtuple

# Comment markers this check understands: '#' (Python/shell), '//' (C/C++
# line comment), '*' (the continuation line inside a C block comment, as in
# " * ==========="). We deliberately do NOT try to parse '/*'...'*/' as a
# state machine -- a per-line regex is enough for divider lines, which are
# always a single physical line by construction (that is what makes their
# fusion into the FOLLOWING line detectable at all).
MARKERS = (r"#", r"//", r"\*")
MARKER_ALT = "|".join(MARKERS)

DIVIDER_CHARS = "=\\-#*~"

# A PURE divider: optional indent, a comment marker, optional space, a run of
# >=3 of one divider char, then nothing but trailing whitespace to EOL.
PURE_DIVIDER_RE = re.compile(
    r"^(?P<indent>[ \t]*)(?P<marker>" + MARKER_ALT + r")[ \t]*"
    r"(?P<run>(?P<runchar>[=\-#*~])(?P=runchar){2,})[ \t]*$"
)

# A FUSED divider candidate: same prefix, a run of >=3 of one divider char,
# but then more non-whitespace content immediately follows on the same line
# -- the shape produced when a divider line's trailing newline is lost and
# the next comment line's text lands right after it.
FUSED_RE = re.compile(
    r"^(?P<indent>[ \t]*)(?P<marker>" + MARKER_ALT + r")[ \t]*"
    r"(?P<run>(?P<fusedchar>[=\-#*~])(?P=fusedchar){2,})(?P<tail>[^\s].*)$"
)

# Tails that are legitimate (not fusion): a C block-comment closer, possibly
# with trailing space, e.g. "===*/" or "=== */".
BENIGN_TAIL_RE = re.compile(r"^[ \t]*\*/[ \t]*$")

Finding = namedtuple("Finding", "path lineno kind detail")


def _iter_lines(text):
    # splitlines(keepends=False) is fine here: we operate line-by-line on
    # already-split content, so a genuinely fused line is just one long
    # physical line -- exactly what we want to see.
    return text.splitlines()


def check_text(path, text):
    """Return a list of Finding for one file's text. Pure function -- no I/O
    -- so the selftest can feed synthetic content directly."""
    findings = []
    lines = _iter_lines(text)

    # Pass 1: fusion. Independent of pass 2, checked line-by-line.
    for i, line in enumerate(lines, start=1):
        m = FUSED_RE.match(line)
        if not m:
            continue
        tail = m.group("tail")
        runchar = m.group("fusedchar")
        # The run regex is greedy but can still backtrack by up to (run
        # length - 1) characters and still find a satisfying tail, so any
        # number of leftover copies of runchar can land at the FRONT of
        # tail -- not just one. Strip them before judging the tail: what's
        # left is either nothing (a plain pure divider, not fusion -- pass 2
        # already compares its length), a C block-comment closer possibly
        # with a leading run remnant ("== */"), or genuine fused text.
        tail_after_run = tail.lstrip(runchar)
        if tail_after_run == "" or BENIGN_TAIL_RE.match(tail_after_run):
            continue
        # A SPACE (or tab) right after the divider run is this codebase's
        # single-line banner convention -- "==== TITLE ====" -- not a lost
        # newline. The bug this check exists for glues the run DIRECTLY onto
        # the next line's own comment marker/text with no separating
        # whitespace at all (the corrupted line had no trailing space before
        # its dropped newline). Requiring "no whitespace" here is what tells
        # a banner from a fusion; measured against this repo's tree, without
        # it every "# ==== TITLE ====" banner in src/vmsscs/*.[ch] false-
        # positives (63 findings across 12 files).
        if tail_after_run[:1] in (" ", "\t"):
            continue
        run = m.group("run")
        findings.append(Finding(
            path, i, "fused-divider",
            "divider run %r (%d chars) is immediately followed by more "
            "content on the same line: %r -- looks like a lost newline "
            "fused this divider onto the next comment line"
            % (run[0], len(run), tail[:60]),
        ))

    # Pass 2: truncation. Group PURE dividers by (marker, char) within this
    # file, compare each run's length against the group's mode length.
    groups = {}  # (marker, char) -> list[(lineno, run_len)]
    for i, line in enumerate(lines, start=1):
        m = PURE_DIVIDER_RE.match(line)
        if not m:
            continue
        marker = m.group("marker")
        run = m.group("run")
        key = (marker, run[0])
        groups.setdefault(key, []).append((i, len(run)))

    for key, entries in groups.items():
        if len(entries) < 2:
            continue  # nothing to compare a singleton divider against
        lengths = [ln for _, ln in entries]
        mode_len, _count = Counter(lengths).most_common(1)[0]
        for lineno, ln in entries:
            if ln < mode_len:
                findings.append(Finding(
                    path, lineno, "truncated-divider",
                    "divider run is %d chars but this file's matching "
                    "dividers (marker %r, char %r) are %d chars -- looks "
                    "truncated" % (ln, key[0], key[1], mode_len),
                ))

    return findings


def check_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
            text = f.read()
    except OSError as exc:
        return [Finding(path, 0, "unreadable", str(exc))]
    return check_text(path, text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", help="files to check")
    args = ap.parse_args(argv)

    all_findings = []
    for path in args.paths:
        all_findings.extend(check_file(path))

    if not all_findings:
        print("check_divider_integrity: OK (%d file(s), no fused or "
              "truncated comment dividers)" % len(args.paths))
        return 0

    print("check_divider_integrity: FAIL -- %d finding(s)" % len(all_findings))
    for f in all_findings:
        print("  %s:%d [%s] %s" % (f.path, f.lineno, f.kind, f.detail))
    return 1


if __name__ == "__main__":
    sys.exit(main())
