#!/usr/bin/env python3
"""semantic_diff.py - OVMX service transcripts vs the real-OpenVMS goldens (rd vms-8d1).

Each semantic-oracle probe (tools/oracle/semantic/specs/<family>.py, built by
spgen.py) prints one line per case: "<CASE-ID> <observations>". The SAME probe
ran on a real OpenVMS node; its transcript is the golden:

  docs/oracle/semantics/<family>/alpha84.txt   OpenVMS Alpha V8.4  (the reference)
  docs/oracle/semantics/<family>/vax73.txt     OpenVMS VAX V7.3    (cross-check)

OVMX is compared with the Alpha V8.4 golden (the newest real system, and 64-bit
like OVMX's own x86_64 runtime); a family with no Alpha golden falls back to VAX.
Where VAX and Alpha themselves disagree the case is reported as ARCH-DIVERGENT
(informational: the reference still decides).

It is a RATCHET, like tools/compat/check_oracle_constants.py:
docs/oracle/semantics/known-diff.txt lists "<family> <CASE-ID>" lines (with an rd
item in a trailing comment) whose OVMX result is known to differ from the
golden. The gate FAILS when
  * a case differs (or is missing from the OVMX transcript) and is NOT listed, or
  * a listed case now matches (the list is stale: delete the line), or
  * a listed case names a family/case the goldens do not have.
So the list can only shrink.

  semantic_diff.py <ovmx-transcript-dir>               gate (exit 1 on failure)
  semantic_diff.py <ovmx-transcript-dir> --report      every case, classified
  semantic_diff.py --selftest                          prove the gate can go red
  semantic_diff.py --goldens                           check goldens are well formed
  semantic_diff.py <dir> --write-known                 (re)write known-diff.txt
"""
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GOLD = os.path.join(ROOT, "docs", "oracle", "semantics")
KNOWN = os.path.join(GOLD, "known-diff.txt")
CASE_RE = re.compile(r"^([A-Z0-9][A-Z0-9_.$-]*)(?: (.*))?$")


def parse(path):
    """-> (ordered {case: observation}, family, complete?)"""
    cases, fam, complete, begun = {}, None, False, False
    for ln in open(path, errors="replace"):
        ln = ln.rstrip("\r\n")
        if not ln or ln.startswith("#"):
            continue
        m = re.match(r"^=== SEMPROBE (\S+) (BEGIN|END) ===$", ln)
        if m:
            if m.group(2) == "BEGIN":
                fam, begun = m.group(1), True
            else:
                complete = m.group(1) == fam
            continue
        if not begun:
            continue
        m = CASE_RE.match(ln)
        if m:
            cases.setdefault(m.group(1), (m.group(2) or "").strip())
    return cases, fam, complete


def goldens():
    """{family: {'alpha84': path?, 'vax73': path?}}"""
    out = {}
    if not os.path.isdir(GOLD):
        return out
    for fam in sorted(os.listdir(GOLD)):
        d = os.path.join(GOLD, fam)
        if not os.path.isdir(d):
            continue
        g = {k: os.path.join(d, k + ".txt") for k in ("alpha84", "vax73")
             if os.path.exists(os.path.join(d, k + ".txt"))}
        if g:
            out[fam] = g
    return out


def reference(g):
    return g.get("alpha84") or g.get("vax73")


def load_known(path=KNOWN):
    known = {}
    if os.path.exists(path):
        for ln in open(path):
            s = ln.split("#", 1)[0].strip()
            if s:
                fam, case = s.split()
                known[(fam, case)] = ln.rstrip("\n")
    return known


def classify(tdir, gold=None):
    """-> list of (family, case, verdict, ref_obs, ovmx_obs, vax_obs)"""
    rows = []
    gold = gold if gold is not None else goldens()
    for fam, g in gold.items():
        ref, _, _ = parse(reference(g))
        vax = parse(g["vax73"])[0] if "vax73" in g and "alpha84" in g else None
        tp = os.path.join(tdir, fam + ".txt")
        ovmx = parse(tp)[0] if os.path.exists(tp) else {}
        for case, robs in ref.items():
            o = ovmx.get(case)
            v = vax.get(case) if vax is not None else None
            if o is None:
                verdict = "MISSING"
            elif o == robs:
                verdict = "MATCH"
            else:
                verdict = "DIFF"
            rows.append((fam, case, verdict, robs, o, v))
    return rows


def gate(tdir, report=False, known_path=KNOWN, gold=None):
    gold = gold if gold is not None else goldens()
    rows = classify(tdir, gold)
    known = load_known(known_path)
    bad, stale, fams = [], [], {}
    seen = set()
    for fam, case, verdict, robs, o, v in rows:
        f = fams.setdefault(fam, [0, 0, 0])
        f[0] += 1
        key = (fam, case)
        seen.add(key)
        if verdict == "MATCH":
            if key in known:
                stale.append(key)
        else:
            f[1] += 1
            if key in known:
                f[2] += 1
            else:
                bad.append((fam, case, verdict, robs, o))
        if report:
            tag = verdict + (" (known)" if key in known and verdict != "MATCH" else "")
            arch = "  [ARCH-DIVERGENT vax: %s]" % v if v is not None and v != robs else ""
            print("%-8s %-28s %s" % (fam, case, tag) + arch)
            if verdict != "MATCH":
                print("           real VMS: %s" % robs)
                print("           OVMX:     %s" % (o if o is not None else "<absent>"))
    orphan = [k for k in known if k not in seen]
    print("semantic oracle: per-family mismatches (vs %s)" % ", ".join(
        "%s=%s" % (f, os.path.basename(reference(g))) for f, g in gold.items()))
    for fam, (n, d, k) in fams.items():
        print("  %-10s %4d cases  %4d differ  (%d known, %d new)" % (fam, n, d, k, d - k))
    ok = True
    if bad:
        ok = False
        print("FAIL: %d case(s) differ from real OpenVMS and are not in %s:" % (len(bad), os.path.relpath(known_path, ROOT)))
        for fam, case, verdict, robs, o in bad:
            print("  %s %s %s\n      real VMS: %s\n      OVMX:     %s" % (fam, case, verdict, robs, o if o is not None else "<absent>"))
    if stale:
        ok = False
        print("FAIL: %d known-diff line(s) now MATCH real OpenVMS -- delete them (the list only shrinks):" % len(stale))
        for fam, case in stale:
            print("  %s %s" % (fam, case))
    if orphan:
        ok = False
        print("FAIL: %d known-diff line(s) name no golden case:" % len(orphan))
        for k in orphan:
            print("  " + known[k])
    if ok:
        print("OK: every OVMX result matches real OpenVMS or is a tracked known difference")
    return ok


def check_goldens():
    ok = True
    for fam, g in goldens().items():
        for k, p in g.items():
            cases, pf, complete = parse(p)
            if pf != fam:
                print("FAIL: %s names family %r" % (p, pf)); ok = False
            if not cases:
                print("FAIL: %s has no cases" % p); ok = False
            head = open(p).read(4000)
            if "# provenance:" not in head:
                print("FAIL: %s has no '# provenance:' header" % p); ok = False
            spec = os.path.join(HERE, "specs", fam + ".py")
            if not os.path.exists(spec):            # a DCL family (comgen.py)
                spec = os.path.join(HERE, "specs-dcl", fam + ".py")
            m = re.search(r"^# spec: \S+ sha256=([0-9a-f]{64})$", head, re.M)
            if not os.path.exists(spec):
                print("FAIL: %s has no spec %s" % (p, os.path.relpath(spec, ROOT))); ok = False
            elif not m or m.group(1) != hashlib.sha256(open(spec, "rb").read()).hexdigest():
                print("FAIL: %s was captured from a different %s -- re-capture it on the real node "
                      "(tools/oracle/semantic/capture.py)" % (os.path.relpath(p, ROOT), os.path.relpath(spec, ROOT)))
                ok = False
            print("golden %-8s %-8s %4d cases%s" % (fam, k, len(cases), "" if complete else " (ends in a guard abort)"))
    return ok


def write_known(tdir):
    rows = classify(tdir)
    old = load_known()
    with open(KNOWN, "w") as f:
        f.write(HEADER)
        for fam, case, verdict, robs, o, v in rows:
            if verdict != "MATCH":
                prev = old.get((fam, case))
                f.write((prev if prev else "%s %s  # %s" % (fam, case, verdict)) + "\n")


HEADER = """# known-diff.txt - the semantic-oracle ratchet (rd vms-8d1).
# "<family> <CASE-ID>  # <rd item>: <why>" -- a case whose OVMX result is KNOWN to
# differ from real OpenVMS (docs/oracle/semantics/<family>/). Read by
# tools/oracle/semantic/semantic_diff.py: an unlisted difference fails the gate,
# and so does a listed case that now matches. Fix the bug, delete the line.
"""


def selftest():
    import tempfile
    t = tempfile.mkdtemp()
    gd = os.path.join(t, "gold", "zz")
    os.makedirs(gd)
    open(os.path.join(gd, "alpha84.txt"), "w").write(
        "# provenance: selftest\n=== SEMPROBE zz BEGIN ===\nZZ.A st=00000001\nZZ.B st=00000009 x=1\n=== SEMPROBE zz END ===\n")
    gold = {"zz": {"alpha84": os.path.join(gd, "alpha84.txt")}}
    td = os.path.join(t, "ovmx")
    os.makedirs(td)
    kn = os.path.join(t, "known.txt")
    res = []

    def run(ovmx, known):
        open(os.path.join(td, "zz.txt"), "w").write(ovmx)
        open(kn, "w").write(known)
        sys.stdout = open(os.devnull, "w")
        try:
            return gate(td, known_path=kn, gold=gold)
        finally:
            sys.stdout = sys.__stdout__
    same = "=== SEMPROBE zz BEGIN ===\nZZ.A st=00000001\nZZ.B st=00000009 x=1\n=== SEMPROBE zz END ===\n"
    diff = same.replace("x=1", "x=2")
    short = "=== SEMPROBE zz BEGIN ===\nZZ.A st=00000001\n"
    res.append(("identical transcript passes", run(same, "") is True))
    res.append(("an unlisted difference fails", run(diff, "") is False))
    res.append(("a listed difference passes", run(diff, "zz ZZ.B  # x\n") is True))
    res.append(("a stale listed case fails", run(same, "zz ZZ.B  # x\n") is False))
    res.append(("a missing case fails", run(short, "") is False))
    res.append(("an orphan known line fails", run(same, "zz ZZ.NOPE  # x\n") is False))
    ok = all(r for _, r in res)
    for name, r in res:
        print("%s: %s" % ("PASS" if r else "FAIL", name))
    return ok


def main(a):
    if a[1:] == ["--selftest"]:
        return 0 if selftest() else 1
    if a[1:] == ["--goldens"]:
        return 0 if check_goldens() else 1
    if len(a) >= 2 and os.path.isdir(a[1]):
        if a[2:] == ["--write-known"]:
            write_known(a[1]); return 0
        return 0 if gate(a[1], report=a[2:] == ["--report"]) else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
