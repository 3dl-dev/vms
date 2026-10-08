#!/usr/bin/env python3
"""ksdiff.py - OVMX keystroke transcripts vs the real-OpenVMS goldens (rd vms-370).

ksplay.py plays each case script (tools/oracle/keystroke/cases/<ID>.ks) into a
terminal and writes a step-segmented transcript, <ID>.ks.txt. The SAME scripts
ran on the console (OPA0:) of real OpenVMS nodes; those transcripts are the
goldens:

  docs/oracle/keystroke/<ID>/alpha84.ks.txt   OpenVMS Alpha V8.4  (the reference)
  docs/oracle/keystroke/<ID>/vax73.ks.txt     OpenVMS VAX V7.3    (cross-check)

OVMX (a 64-bit runtime) is compared with the Alpha V8.4 golden; a case with no
Alpha golden falls back to VAX. Where VAX and Alpha themselves disagree the step
is reported ARCH-DIVERGENT (informational: the reference still decides).

The unit of comparison is one STEP of one case: the exact bytes the terminal
showed (rendered, masked) between that keystroke and the next. A step differs
when OVMX showed anything else -- an echo that came early, a ^U that printed
backspaces instead of a fresh prompt, an *INTERRUPT* that never appeared.

It is a RATCHET, like semantic_diff.py: docs/oracle/keystroke/known-diff.txt
lists "<CASE> <STEP>  # <rd item>: <why>" lines whose OVMX result is known to
differ. The gate FAILS when
  * a step differs (or is missing from the OVMX transcript) and is NOT listed,
  * a listed step now matches (the list is stale: delete the line), or
  * a listed line names a case/step the goldens do not have.
So the list can only shrink.

  ksdiff.py <ovmx-transcript-dir>               gate (exit 1 on failure)
  ksdiff.py <ovmx-transcript-dir> --report      every step, classified, with diffs
  ksdiff.py --selftest                          prove the gate can go red
  ksdiff.py --goldens                           check the goldens are well formed
  ksdiff.py --known                             check known-diff.txt names real steps + rd items
  ksdiff.py <dir> --write-known                 (re)write known-diff.txt (keeps reasons)
"""
import difflib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ksplay  # noqa: E402  (the case language + the symmetric mask)
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GOLD = os.path.join(ROOT, "docs", "oracle", "keystroke")
KNOWN = os.path.join(GOLD, "known-diff.txt")
REF_ORDER = ("alpha84", "vax73")
STEP_RE = re.compile(r"^=== STEP (\S+)( .*)?$")


def parse(path):
    """-> (start_line, ordered {step: (quiet, [lines])})"""
    start, steps, cur = None, {}, None
    for ln in open(path, errors="replace"):
        ln = ln.rstrip("\n")
        if ln.startswith("@start "):
            start = ln
            continue
        m = STEP_RE.match(ln)
        if m:
            rest = m.group(2) or ""
            quiet = rest.endswith(" quiet") or "(merged into next)" in rest
            cur = m.group(1)
            steps[cur] = (quiet, [])
            continue
        if cur is not None and ln.startswith("| "):
            steps[cur][1].append(ln[2:])
        elif cur is not None and ln == "|":
            steps[cur][1].append("")
    return start, steps


def goldens():
    """-> {case: {arch: path}}"""
    out = {}
    if not os.path.isdir(GOLD):
        return out
    for case in sorted(os.listdir(GOLD)):
        d = os.path.join(GOLD, case)
        if not os.path.isdir(d):
            continue
        for arch in REF_ORDER:
            p = os.path.join(d, arch + ".ks.txt")
            if os.path.exists(p):
                out.setdefault(case, {})[arch] = p
    return out


def read_known(path=KNOWN):
    known = {}
    if not os.path.exists(path):
        return known
    for n, ln in enumerate(open(path), 1):
        body = ln.split("#", 1)[0].strip()
        if not body:
            continue
        parts = body.split()
        if len(parts) != 2:
            raise SystemExit("%s:%d: want '<CASE> <STEP>  # <rd>: <why>'" % (path, n))
        why = ln.split("#", 1)[1].strip() if "#" in ln else ""
        known[(parts[0], parts[1])] = why
    return known


_CASES = {}


def norm(case, lines):
    """Apply the case script's symmetric mask (ksplay.normalize)."""
    if lines is None:
        return None
    if case not in _CASES:
        p = os.path.join(ksplay.CASES, case + ".ks")
        _CASES[case] = ksplay.Case(p) if os.path.exists(p) else None
    c = _CASES[case]
    return ksplay.normalize(c, lines) if c else lines


def compare(ovmx_dir, gold=None):
    """-> list of (case, step, verdict, ref_arch, ref_lines, ovmx_lines, arch_div)
    verdict in MATCH | DIFF | MISSING"""
    gold = gold if gold is not None else goldens()
    rows = []
    for case, archs in sorted(gold.items()):
        ref = next(a for a in REF_ORDER if a in archs)
        rstart, rsteps = parse(archs[ref])
        other = [a for a in REF_ORDER if a in archs and a != ref]
        osteps = parse(archs[other[0]])[1] if other else None
        p = os.path.join(ovmx_dir, case + ".ks.txt")
        ostart, ovsteps = parse(p) if os.path.exists(p) else (None, {})
        for step, (quiet, rlines) in rsteps.items():
            if quiet:
                continue
            rl = norm(case, rlines)
            div = bool(osteps is not None and step in osteps and norm(case, osteps[step][1]) != rl)
            ol = norm(case, ovsteps[step][1]) if step in ovsteps else None
            if ol is None:
                rows.append((case, step, "MISSING", ref, rl, None, div))
            elif ol == rl:
                rows.append((case, step, "MATCH", ref, rl, ol, div))
            else:
                rows.append((case, step, "DIFF", ref, rl, ol, div))
    return rows


def gate(rows, known, gold=None):
    gold = gold if gold is not None else goldens()
    errs = []
    seen = set()
    for case, step, verdict, *_ in rows:
        seen.add((case, step))
        k = (case, step)
        if verdict != "MATCH" and k not in known:
            errs.append("NEW DIFFERENCE  %s %s (%s) -- fix OVMX, or list it in known-diff.txt with an rd item"
                        % (case, step, verdict))
        if verdict == "MATCH" and k in known:
            errs.append("NOW MATCHES     %s %s -- delete its known-diff.txt line (the ratchet only shrinks)"
                        % (case, step))
    for k in known:
        if k not in seen:
            errs.append("UNKNOWN ENTRY   %s %s -- no such (non-quiet) step in the goldens" % k)
    return errs


def report(rows, known):
    n = {"MATCH": 0, "DIFF": 0, "MISSING": 0}
    for case, step, verdict, ref, rl, ol, div in rows:
        n[verdict] += 1
        tag = " [ARCH-DIVERGENT]" if div else ""
        kn = "  (known: %s)" % known[(case, step)] if (case, step) in known else ""
        print("%-8s %s %s vs %s%s%s" % (verdict, case, step, ref, tag, kn))
        if verdict != "MATCH":
            for d in difflib.unified_diff(rl, ol or [], "real-" + ref, "ovmx", lineterm="", n=2):
                print("    " + d)
    tot = sum(n.values())
    print("== keystroke oracle: %d steps -- %d match, %d differ, %d missing ==" %
          (tot, n["MATCH"], n["DIFF"], n["MISSING"]))


def write_known(rows, known):
    lines = [
        "# known-diff.txt - the keystroke-oracle ratchet (rd vms-370).",
        "# \"<CASE> <STEP>  # <rd item>: <why>\" -- a step whose OVMX terminal output is KNOWN to",
        "# differ from real OpenVMS (docs/oracle/keystroke/<CASE>/). Read by",
        "# tools/oracle/keystroke/ksdiff.py: an unlisted difference fails the gate, and so does",
        "# a listed step that now matches. Fix the bug, delete the line.",
    ]
    for case, step, verdict, *_ in rows:
        if verdict != "MATCH":
            why = known.get((case, step), "vms-4eba: OVMX has no VMS terminal driver")
            lines.append("%s %s  # %s" % (case, step, why))
    with open(KNOWN, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote %s (%d entries)" % (KNOWN, len(lines) - 5))


def check_goldens():
    gold = goldens()
    bad = 0
    if not gold:
        print("no goldens under %s" % GOLD)
        return 1
    for case, archs in gold.items():
        for arch, p in archs.items():
            start, steps = parse(p)
            if not start or not start.endswith(" OK"):
                print("BAD %s/%s: @start not OK (%s)" % (case, arch, start))
                bad += 1
            if not steps:
                print("BAD %s/%s: no steps" % (case, arch))
                bad += 1
            # the golden must have been played from the case script as it is now
            cp = os.path.join(ksplay.CASES, case + ".ks")
            if not os.path.exists(cp):
                print("BAD %s/%s: no case script %s" % (case, arch, cp))
                bad += 1
                continue
            want = ksplay.Case(cp).sha
            got = re.search(r"^# case-sha256 (\S+)", open(p).read(), re.M)
            if not got or got.group(1) != want:
                print("BAD %s/%s: golden was not captured from the current case script"
                      " (re-capture it: tools/oracle/keystroke/capture_lab.sh)" % (case, arch))
                bad += 1
    print("goldens: %d cases, %s" % (len(gold), "OK" if not bad else "%d problems" % bad))
    return 1 if bad else 0


def selftest():
    import tempfile
    t = tempfile.mkdtemp()
    g = os.path.join(t, "gold", "C1")
    os.makedirs(g)
    gold_txt = "@start loggedin OK\n=== STEP A\n| abc<CR><LF>\n=== STEP Q quiet\n=== STEP B\n| $ \n"
    open(os.path.join(g, "alpha84.ks.txt"), "w").write(gold_txt)
    gold = {"C1": {"alpha84": os.path.join(g, "alpha84.ks.txt")}}
    ov = os.path.join(t, "ov")
    os.makedirs(ov)
    fails = 0

    def chk(name, cond):
        nonlocal fails
        print(("  PASS: " if cond else "  FAIL: ") + name)
        fails += 0 if cond else 1

    open(os.path.join(ov, "C1.ks.txt"), "w").write(gold_txt)
    chk("identical transcript -> gate clean", not gate(compare(ov, gold), {}, gold))
    open(os.path.join(ov, "C1.ks.txt"), "w").write(gold_txt.replace("abc<CR>", "abc<BS> <BS><CR>"))
    rows = compare(ov, gold)
    chk("a changed step -> NEW DIFFERENCE", any("NEW DIFFERENCE  C1 A" in e for e in gate(rows, {}, gold)))
    chk("listed in known-diff -> gate clean", not gate(rows, {("C1", "A"): "x"}, gold))
    open(os.path.join(ov, "C1.ks.txt"), "w").write(gold_txt)
    chk("listed but now matching -> NOW MATCHES",
        any("NOW MATCHES" in e for e in gate(compare(ov, gold), {("C1", "A"): "x"}, gold)))
    chk("listed unknown step -> UNKNOWN ENTRY",
        any("UNKNOWN ENTRY" in e for e in gate(compare(ov, gold), {("C1", "Z"): "x"}, gold)))
    chk("quiet step never compared", all(r[1] != "Q" for r in compare(ov, gold)))
    os.remove(os.path.join(ov, "C1.ks.txt"))
    chk("absent transcript -> MISSING, gate red",
        any("MISSING" in e for e in gate(compare(ov, gold), {}, gold)))
    print("=== %s ===" % ("selftest OK" if not fails else "selftest FAILED (%d)" % fails))
    return 1 if fails else 0


def main(a):
    if not a:
        sys.exit(__doc__)
    if a[0] == "--selftest":
        return selftest()
    if a[0] == "--goldens":
        return check_goldens()
    if a[0] == "--known":
        # every known-diff line must name a compared (non-quiet) golden step
        steps = set()
        for case, archs in goldens().items():
            ref = next(x for x in REF_ORDER if x in archs)
            for st, (quiet, _) in parse(archs[ref])[1].items():
                if not quiet:
                    steps.add((case, st))
        known = read_known()
        bad = [k for k in known if k not in steps]
        for k in bad:
            print("UNKNOWN ENTRY   %s %s -- no such (non-quiet) step in the goldens" % k)
        nowhy = [k for k, why in known.items() if not re.match(r"vms-[0-9a-f]+\b", why)]
        for k in nowhy:
            print("NO RD ITEM      %s %s -- each line needs '# vms-xxx: why'" % k)
        print("known-diff: %d entries, %s" % (len(known), "OK" if not bad and not nowhy else "FAILED"))
        return 1 if bad or nowhy else 0
    d = a[0]
    if not os.path.isdir(d):
        sys.exit("ksdiff: no transcript dir %s" % d)
    rows = compare(d)
    if not rows:
        sys.exit("ksdiff: no goldens under %s" % GOLD)
    known = read_known()
    if "--write-known" in a:
        write_known(rows, known)
        return 0
    if "--report" in a:
        report(rows, known)
    errs = gate(rows, known)
    for e in errs:
        print("ksdiff: " + e)
    n = sum(1 for r in rows if r[2] != "MATCH")
    print("ksdiff: %d/%d steps differ from real OpenVMS, %d known; gate %s"
          % (n, len(rows), len(known), "FAILED" if errs else "OK"))
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
