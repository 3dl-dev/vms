#!/usr/bin/env python3
"""
test_scs_credit_figures.py -- the WIRE VERDICT prose must match the measurement.

vms-76e, resolution 3. tools/scs_credit_measure.py re-derives every figure in
the WIRE VERDICT of src/vmsscs/include/scs_credit.h from the lab captures, but
those captures are host-only (13 GB, not in git), so ctest cannot run it. This
test runs the half that needs no captures: it takes the EXPECTED table straight
out of that script -- the checked-in record of what the captures measured -- and
asserts every figure still appears in the header and in
docs/cluster-protocol-spec.md.

What that buys: a recorded measurement can no longer silently drift away from
the prose that cites it. Editing a number in the comment without re-running the
measurement now REDS. It does NOT re-derive the numbers from packets; only
tools/scs_credit_measure.py on a lab host does that, and the header says so.

It also pins the two structural facts the review turned on:
  - evidence line 1 (conservation) is scoped to the UNFILTERED population, and
  - the inverted ">>> FILTER, REQUIRED <<<" heading is gone and stays gone.
"""

import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
HEADER = os.path.join(ROOT, "src", "vmsscs", "include", "scs_credit.h")
SPEC = os.path.join(ROOT, "docs", "cluster-protocol-spec.md")
MEASURE = os.path.join(ROOT, "tools", "scs_credit_measure.py")

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


def load_expected():
    spec = importlib.util.spec_from_file_location("scs_credit_measure", MEASURE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.EXPECTED


def num(n):
    """
    Regex for one integer in either rendering: scs_credit.h writes '10842',
    cluster-protocol-spec.md writes '10 842' with thin-space digit grouping.

    Do NOT do this by globally collapsing digit-space-digit across the file --
    that also welds unrelated adjacent numbers together ('792 106-byte' becomes
    '792106-byte') and silently breaks every adjacency check downstream.
    Verified: the first version of this test did exactly that, and a mutated
    106-frame count survived because of it.
    """
    s = str(n)
    head = s[: len(s) % 3 or 3]
    groups = [s[i:i + 3] for i in range(len(head), len(s), 3)]
    return r"\b" + r"[ \u00a0\u2009]?".join([head] + groups) + r"\b"


def plain(s):
    """Markdown emphasis / code marks stripped, for prose (not figure) checks."""
    return s.replace("*", "").replace("`", "")


def main():
    expected = load_expected()
    header = open(HEADER).read()
    spec = open(SPEC).read()
    docs = {"scs_credit.h": header, "cluster-protocol-spec.md": spec}

    # --- 1. conservation figures ------------------------------------------
    # Scoped to the CONSERVATION evidence block in each document, NOT to the
    # whole file: both files also discuss these numbers in the correction note,
    # and a whole-file substring search would let that note mask a typo in the
    # evidence line itself (verified -- an unscoped check passes the mutant).
    # Each granted/peer-sent pair must appear ADJACENT and IN ORDER.
    blocks = {}
    m = re.search(r"1\.\s*CONSERVATION(.*?)2\.\s*VALUE SHAPE", header, re.S)
    check(m is not None, "scs_credit.h: the CONSERVATION evidence line is gone")
    blocks["scs_credit.h"] = m.group(1) if m else ""
    m = re.search(r"1\.\s*\*\*Conservation\*\*(.*?)2\.\s*\*\*Value shape\*\*", spec, re.S)
    check(m is not None, "cluster-protocol-spec.md: the Conservation evidence item is gone")
    blocks["cluster-protocol-spec.md"] = m.group(1) if m else ""

    for cap, nodes in expected["conservation"].items():
        for node, (granted, peer_sent) in nodes.items():
            pair = re.compile(r"%s.{0,40}?%s" % (num(granted), num(peer_sent)), re.S)
            for name, block in blocks.items():
                check(pair.search(block) is not None,
                      "%s: conservation pair %d granted / %d peer-sent (%s, %s) "
                      "missing from the CONSERVATION evidence block"
                      % (name, granted, peer_sent, cap, node))

    # --- 1b. the REFUTATION figures quoted in the correction note ----------
    # The note claims that applying the 0x4B13 filter to line 1 breaks the
    # identity, and quotes numbers. Those numbers are measured too (see
    # conservation_4b13_refuted in the measure script), so pin them here.
    for cap, nodes in expected["conservation_4b13_refuted"].items():
        for node, (granted, peer_sent) in nodes.items():
            pair = re.compile(r"%s.{0,60}?%s" % (num(granted), num(peer_sent)), re.S)
            for name, text in docs.items():
                check(pair.search(text) is not None,
                      "%s: the correction note's refutation pair %d vs %d (%s, %s) "
                      "is missing or has drifted" % (name, granted, peer_sent, cap, node))

    # --- 2. the marker split that justifies the unfiltered population ------
    for marker, n in expected["grounding_190_markers"].items():
        for name, text in docs.items():
            check(re.search(num(n), text) is not None,
                  "%s: 190-byte marker count %d (0x%s) missing"
                  % (name, n, marker.upper()))
    for name, text in docs.items():
        check(re.search(num(expected["grounding_190_total"]), text) is not None,
              "%s: 190-byte total %d missing" % (name, expected["grounding_190_total"]))

    # --- 3. value-shape histograms ----------------------------------------
    for key in ("shape_all_190", "shape_4b13_190"):
        hist = "{%s}" % ", ".join("%d:%d" % (k, v) for k, v in sorted(expected[key].items()))
        for name, text in docs.items():
            check(hist in text, "%s: histogram %s (%s) missing" % (name, key, hist))
    for name, text in docs.items():
        check(re.search(num(expected["shape_4b13_190_n"]), text) is not None,
              "%s: 0x4B13 190-byte n=%d missing" % (name, expected["shape_4b13_190_n"]))

    # --- 4. the per-class table, in each document's own layout ------------
    for klass, (n, distinct, mx) in expected["classes_admitted"].items():
        check(re.search(r"%d\s+%d\s+%d\s+%d\b" % (klass, n, distinct, mx), header) is not None,
              "scs_credit.h: admitted class row %d %d/%d/%d missing"
              % (klass, n, distinct, mx))
        check(re.search(r"%d\s*(?:\u2192|->)\s*%s/%d/%d\b" % (klass, num(n).strip("\\b"), distinct, mx), spec) is not None,
              "cluster-protocol-spec.md: admitted class row %d %d/%d/%d missing"
              % (klass, n, distinct, mx))
    for klass, (n, distinct, mx) in expected["classes_refused"].items():
        check(re.search(r"%d\s+%d\s+%d\s+%d\b" % (klass, n, distinct, mx), header) is not None,
              "scs_credit.h: refused class row %d %d/%d/%d missing"
              % (klass, n, distinct, mx))

    # --- 5. the 110-byte tunables and the 106 refutation ------------------
    tunables = ",".join(str(v) for v in expected["class_110_values"])
    check(tunables in header.replace(", ", ","),
          "scs_credit.h: 110-byte tunable value set %s missing" % tunables)
    # The 106 refutation: the count must sit NEXT TO "106-byte" (either order).
    # A bare `"792" in text` passes on the unrelated "792/792" phrasing, so it
    # cannot detect a drifted count -- verified against a mutant.
    n106 = expected["class_106_markers"]["4113"]
    near = re.compile(r"(%s.{0,80}?106[- ]byte|106[- ]byte.{0,140}?%s)" % (num(n106), num(n106)),
                      re.S)
    for name, text in docs.items():
        check(near.search(text) is not None,
              "%s: the count of %d 106-byte 0x4113 frames is missing or has drifted"
              % (name, n106))
        check("4113" in text, "%s: marker 0x4113 not named" % name)
        check(re.search(r"(?i)zero\s+106[- ]byte", plain(text)) is not None,
              "%s: the 'zero 106-byte 0x4B13 frames' claim missing" % name)

    # --- 6. sibling-marker stats (why the unfiltered population is legal) --
    for (klass, marker), (n, mx) in expected["siblings"].items():
        check(re.search(r"%s.{0,80}?n=%d.{0,40}?max=%d" % (marker.upper(), n, mx),
                        header, re.S | re.I) is not None,
              "scs_credit.h: sibling %d-byte 0x%s n=%d max=%d missing"
              % (klass, marker.upper(), n, mx))

    # --- 7. STRUCTURE: what the review actually turned on ------------------
    check("FILTER, REQUIRED" not in header,
          "scs_credit.h: the inverted '>>> FILTER, REQUIRED <<<' heading is back -- "
          "the 0x4B13 filter REFUTES the conservation identity, it does not enable it")
    check("FILTER, REQUIRED" not in spec and "do not reproduce without the filter" not in spec,
          "cluster-protocol-spec.md: the inverted filter claim is back")

    body = blocks["scs_credit.h"]
    if body:
        check("POPULATION (A)" in body,
              "scs_credit.h: the CONSERVATION line no longer says which population "
              "it uses -- it MUST be (A), all 190-byte frames, NO marker filter")
        check(re.search(r"NO marker filter", body) is not None,
              "scs_credit.h: the CONSERVATION line no longer states 'NO marker filter'")

    for name, text in docs.items():
        check("tools/scs_credit_measure.py" in text,
              "%s: no pointer to the re-derivation script" % name)

    for f in failures:
        print("FAIL %s" % f)
    print("%d checks, %d failures" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
