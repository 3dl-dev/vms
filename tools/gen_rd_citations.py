#!/usr/bin/env python3
"""gen_rd_citations.py - regenerate tracking/rd-citations.tsv from live rd.

WHY THIS EXISTS (rd vms-8cc). The vms_kif caller census
(tests/integration/test_kif_caller_census.sh) lets a source declaration buy an
exemption by CITING AN rd ITEM:

    OVMX-UNWIRED: vms_kif_foo (<rd item id>) -- why

It validated only the SHAPE of the id -- that a well-formed `vms-xxx` token was
present. MEASURED on the branch this replaces: repointing one declaration at
`vms-q9z9`, an id that has never existed, left the census rc=0 and its counts
unchanged; repointing another at `vms-fb9`, whose status is `done`, did the
same. The exemption was sold for a token, not for tracked work.

WHICH GATES READ THE LEDGER: run `grep -rn rd_cite_check tests/` and read the
answer off the tree. This docstring twice named a list that did not match the
code -- once claiming the userspace service register read the ledger when it
did not (rd vms-32e), then correcting that to a standing "it never will". Both
were wrong in the same way: a caller list frozen in a comment.

The register (tests/integration/test_userspace_service_register.sh) accepts
OVMX-USERSPACE / OVMX-PARTIAL / OVMX-EXECUTIVE in the same shape (plus
OVMX-LOCAL, which carries no id of its own) and now resolves those ids through
the same rd_cite_check as the census, which is why this script's scan is not
gate-specific: it resolves every id cited under src/ and tools/ regardless of
which marker carries it, and either gate's independent rescan reds when the
ledger does not cover all of them.

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
could not be run at all, the output could not be written, or this script's own
findings contradict the rows it just wrote (see check_self_consistent): an
unresolvable ledger is never written over a good one.

THIS SCRIPT IS NOT A TRUST ANCHOR AND MUST NOT BE READ AS ONE. It is one file
in the repo, and both the CI-side check and the freshness test used to reduce
to it: the freshness test regenerated with THIS code and compared the result to
the committed ledger, so a generator that lied agreed with itself. MEASURED:
one edit here -- the `absent` branch of main() writing ("open", "active")
instead of ("absent", "-") -- plus `sed s/(vms-a86)/(vms-q9z9)/` on one
declaration and a regenerate took the census to rc=0 PASS ("13 sites cite 5
ids -- 5 open, 0 closed, 0 unknown") AND the freshness test to rc=0 PASS. It
read as a false-absent bugfix. Two things now stand between that edit and a
green, and NEITHER of them is in this file:

  1. test_rd_citations_fresh.sh asks rd about every ledger row ITSELF, in its
     own code, and never through this script.
  2. it READS this script's stderr instead of echoing it, and reds when a
     CLOSED:/ABSENT: line contradicts the row written for the same id -- which
     is exactly what that one edit produces, because it left the reporting
     path alone.

check_self_consistent() below is a third, weaker tripwire that lives here: it
catches the same edit at the source. It is trivially removed by the same
attacker, and is worth having only because it costs six lines and turns a
one-edit attack into a three-edit one.
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


def check_self_consistent(rows, closed, absent):
    """The rows written must agree with the findings reported on stderr.

    The two are built from the same branch of the same loop, so they can only
    disagree if that branch was edited to write one verdict and report another
    -- which is precisely the shape of the measured one-edit attack described
    in the module docstring. Dying here is not a defense (the attacker owns
    this file too); it is a tripwire that makes the cheap version of the attack
    stop being cheap.
    """
    by_id = {r[0]: r[1] for r in rows}
    for cid, _st in closed:
        if by_id.get(cid) != "closed":
            die("INTERNAL: reported %s as CLOSED but wrote the row as %r. This "
                "script contradicts itself, so nothing it produced can be "
                "trusted; refusing to let the ledger stand."
                % (cid, by_id.get(cid)))
    for cid in absent:
        if by_id.get(cid) != "absent":
            die("INTERNAL: reported %s as ABSENT but wrote the row as %r. This "
                "script contradicts itself, so nothing it produced can be "
                "trusted; refusing to let the ledger stand."
                % (cid, by_id.get(cid)))
    for cid, verdict in by_id.items():
        if verdict == "closed" and cid not in {c for c, _ in closed}:
            die("INTERNAL: wrote %s as closed without reporting it. The stderr "
                "report is what the freshness test cross-checks; a row that "
                "does not appear there is invisible to it." % cid)
        if verdict == "absent" and cid not in set(absent):
            die("INTERNAL: wrote %s as absent without reporting it. The stderr "
                "report is what the freshness test cross-checks; a row that "
                "does not appear there is invisible to it." % cid)


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

    check_self_consistent(rows, closed, absent)

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
