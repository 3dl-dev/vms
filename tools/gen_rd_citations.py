#!/usr/bin/env python3
"""gen_rd_citations.py - regenerate tracking/rd-citations.tsv from live rd.

WHY THIS EXISTS (rd vms-8cc). Several standing gates let a source declaration
buy an exemption by CITING AN rd ITEM: the vms_kif caller census
(tests/integration/test_kif_caller_census.sh) accepts

    OVMX-UNWIRED: vms_kif_foo (<rd item id>) -- why

and the userspace service register accepts OVMX-USERSPACE / OVMX-PARTIAL /
OVMX-LOCAL / OVMX-EXECUTIVE in the same shape. Both gates validated only the
SHAPE of the id -- that a well-formed `vms-xxx` token was present. MEASURED on
the branch this replaces: repointing one declaration at `vms-q9z9`, an id that
has never existed, left the census rc=0 and its counts unchanged; repointing
another at `vms-fb9`, whose status is `done`, did the same. The exemption was
sold for a token, not for tracked work.

The gates cannot ask rd themselves: rd is nostr-backed and is not reachable
from CI. So the resolution happens HERE, on a host that has rd, and the result
is committed as tracking/rd-citations.tsv -- a derived artifact CI can read
with nothing but grep. The gates read that ledger; this script keeps it true;
tests/integration/test_rd_citations_fresh.sh reds when the two disagree.

WHAT `open` MEANS HERE IS rd's OWN ANSWER, not a status whitelist. `rd list`
is documented as "all open items"; an id is recorded open if and only if it
appears there. A status this script has never seen therefore cannot be
mis-sorted into `open` by an out-of-date list of status names.

Usage:
    tools/gen_rd_citations.py                 # rewrite tracking/rd-citations.tsv
    tools/gen_rd_citations.py --out -         # write to stdout (freshness check)
    tools/gen_rd_citations.py --root DIR      # scan a different tree

Exit status: 0 when a ledger was produced (including one that records ids rd
does not know -- that is data, and the gates red on it). Non-zero only when rd
could not be run at all, or the output could not be written: an unresolvable
ledger is never written over a good one.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

# A declaration line is one that carries an OVMX-<TOKEN>: marker. The item ids
# are then read off that line. Prose that merely mentions "OVMX-UNWIRED" with
# no colon is not a declaration and is not scanned -- that distinction is the
# gates' own, and this scan follows it rather than inventing a second one.
DECL_RE = re.compile(r"OVMX-[A-Z]+:")
ID_RE = re.compile(r"\bvms-[0-9a-z]+(?:\.[0-9a-z]+)?\b")

SCAN_DIRS = ("src", "tools")
SCAN_SUFFIXES = (".c", ".h", ".inc", ".S", ".py", ".sh", ".cmake", ".txt", ".md")

TITLE_MAX = 96


def die(msg):
    sys.stderr.write("gen_rd_citations: %s\n" % msg)
    sys.exit(2)


def scan_citations(root):
    """Every rd id cited by an OVMX-<TOKEN>: declaration under src/ and tools/.

    Returns {id: [ "relpath:lineno", ... ]}.  A superset is harmless: the gates
    check the ids THEY parse against this ledger, so an extra row costs a row.
    A subset is not harmless, and shows up as a loud "NOT IN the citation
    ledger" red rather than as a silent pass -- so the scan errs wide.
    """
    cites = {}
    for d in SCAN_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames if not x.startswith(".")]
            for fn in sorted(filenames):
                if not fn.endswith(SCAN_SUFFIXES):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root)
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as fh:
                        for lineno, line in enumerate(fh, 1):
                            if not DECL_RE.search(line):
                                continue
                            for m in ID_RE.finditer(line):
                                cites.setdefault(m.group(0), []).append(
                                    "%s:%d" % (rel, lineno))
                except OSError as e:
                    die("cannot read %s: %s" % (rel, e))
    return cites


def rd_json(args, cwd):
    """Run `rd <args> --json` WITH cwd INSIDE THE TREE. Returns (rc, parsed).

    The cwd is not incidental. rd auto-detects which board it is talking to
    from the working directory, and it does NOT fail when it cannot find one --
    it answers about no board at all, with rc=0 and an empty list. MEASURED:
    run from a cmake build directory outside the repo -- which is exactly where
    ctest runs a test from -- `rd list --json` returned `[]` and every
    `rd show` returned rc=1, so the first version of this script recorded all
    12 cited ids as `absent` and the freshness test went red for a reason that
    had nothing to do with the ledger. A wrong answer with rc=0 is the worst
    shape available, so every rd call here is pinned to the tree being scanned,
    and main() additionally refuses an empty open set.
    """
    try:
        p = subprocess.run(["rd"] + args + ["--json"],
                           capture_output=True, text=True, cwd=cwd)
    except FileNotFoundError:
        die("rd is not on PATH. This script must run on a host that has rd; "
            "that is the whole reason its output is committed.")
    if p.returncode != 0:
        return p.returncode, None
    try:
        return 0, json.loads(p.stdout)
    except ValueError as e:
        die("rd %s --json did not return JSON: %s" % (" ".join(args), e))


def clean_title(t):
    t = " ".join((t or "").split())
    if len(t) > TITLE_MAX:
        t = t[:TITLE_MAX - 3] + "..."
    return t or "(no title)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--out", default=None,
                    help="output path, or - for stdout")
    args = ap.parse_args()

    root = args.root or os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.abspath(root)

    cites = scan_citations(root)

    rc, open_items = rd_json(["list"], root)
    if rc != 0 or not isinstance(open_items, list):
        die("`rd list --json` failed (rc=%d). Refusing to write a ledger that "
            "would record every citation as unresolvable." % rc)
    if not open_items:
        die("`rd list --json` returned NO open items at all, run from %s.\n"
            "That is not a board with nothing on it; it is overwhelmingly "
            "likely to be rd resolving a different board, or none -- rd answers "
            "rc=0 either way. Writing this out would mark every citation "
            "`closed` or `absent` and red every gate that reads the ledger, and "
            "the obvious way to 'fix' that red is to hand-edit rows, which is "
            "the one thing this mechanism cannot detect. Refusing instead."
            % root)
    open_status = {}
    for it in open_items:
        if isinstance(it, dict) and it.get("id"):
            open_status[it["id"]] = (it.get("status") or "?", it.get("title") or "")

    rows = []
    absent = []
    closed = []
    for cid in sorted(cites):
        if cid in open_status:
            st, title = open_status[cid]
            rows.append((cid, "open", st, clean_title(title)))
            continue
        rc, item = rd_json(["show", cid], root)
        if rc == 0 and isinstance(item, dict) and item.get("id"):
            st = item.get("status") or "?"
            rows.append((cid, "closed", st, clean_title(item.get("title"))))
            closed.append((cid, st))
        else:
            rows.append((cid, "absent", "-", "(rd has no such item)"))
            absent.append(cid)

    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    out = []
    out.append("# OVMX rd citation ledger -- DERIVED ARTIFACT. Do NOT hand-edit.")
    out.append("#")
    out.append("# Every gate that sells an exemption for a cited rd item id checks that id")
    out.append("# against this file. rd is nostr-backed and unreachable from CI, so the")
    out.append("# resolution happens on a host that has rd and the answer is committed.")
    out.append("#")
    out.append("# regenerate:   tools/gen_rd_citations.py")
    out.append("# freshness:    tests/integration/test_rd_citations_fresh.sh (needs rd)")
    out.append("# scanned:      %s -- lines carrying an OVMX-<TOKEN>: marker" %
               ", ".join(d + "/" for d in SCAN_DIRS))
    out.append("# open means:   the id appears in `rd list` (rd's own open set), not a")
    out.append("#               status name matched against a list kept in a gate.")
    out.append("# generated-at: %s" % stamp)
    out.append("#")
    out.append("# <rd id>\t<open|closed|absent>\t<rd status>\t<title>")
    for r in rows:
        out.append("\t".join(r))
    text = "\n".join(out) + "\n"

    dest = args.out
    if dest is None:
        dest = os.path.join(root, "tracking", "rd-citations.tsv")
    if dest == "-":
        sys.stdout.write(text)
    else:
        try:
            with open(dest, "w", encoding="utf-8") as fh:
                fh.write(text)
        except OSError as e:
            die("cannot write %s: %s" % (dest, e))

    sys.stderr.write("gen_rd_citations: %d cited id(s) from %d declaration site(s)"
                     " -- %d open, %d closed, %d absent\n"
                     % (len(rows), sum(len(v) for v in cites.values()),
                        len(rows) - len(closed) - len(absent),
                        len(closed), len(absent)))
    def where(cid, n=3):
        sites = cites[cid]
        head = ", ".join(sites[:n])
        return head if len(sites) <= n else "%s (+%d more)" % (head, len(sites) - n)

    for cid, st in closed:
        sys.stderr.write("  CLOSED: %s (status %s), %d citation(s): %s\n"
                         % (cid, st, len(cites[cid]), where(cid)))
    for cid in absent:
        sys.stderr.write("  ABSENT: rd has no item %s, %d citation(s): %s\n"
                         % (cid, len(cites[cid]), where(cid)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
