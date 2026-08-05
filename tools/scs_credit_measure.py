#!/usr/bin/env python3
"""
scs_credit_measure.py -- re-derive every figure in the WIRE VERDICT of
src/vmsscs/include/scs_credit.h from the lab captures (vms-76e).

WHY THIS EXISTS. The WIRE VERDICT is a comment, and a comment is not evidence.
Nothing in ctest re-derives its numbers, so a wrong recorded measurement is
invisible to a green run -- which is exactly how a mis-scoped filter claim
survived two review rounds. This script makes every figure runnable.

    tools/scs_credit_measure.py            # re-measure and PASS/FAIL vs EXPECTED
    tools/scs_credit_measure.py --print    # just print what the captures say

Requires the lab captures, which are host-only and NOT in git (13 GB, 47 files):

    /data/training/vax/cluster/captures/*.pcap        (lab-1, see CLAUDE.md r.8)

Override the directory with --captures DIR. A full run reads all 47 files and
takes roughly 5-10 minutes; --quick does the two grounding captures only and
takes a few seconds.

EXPECTED below is the checked-in record of what the captures measured on
2026-08-03. `ctest -R scs_credit_figures` does NOT need the captures: it checks
that every figure in EXPECTED still appears verbatim in scs_credit.h and
docs/cluster-protocol-spec.md, so the comment cannot drift away from the
measurement. Only this script, run on a host with the captures, re-derives
EXPECTED itself.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""

import argparse
import collections
import glob
import os
import struct
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

# THE LAB FENCE (vms-096). DEFAULT_CAPDIR is the LAB-1 grounding library and
# every figure checked below is a lab-1 measurement. lab-2 replicas reuse
# lab-1's SCSSYSTEMIDs and node MACs by design (tests/lab/README.md), so a
# lab-2 capture deposited here silently moves every census -- six 2026-08-05
# vaxlab-4 captures did exactly that and put 11 of this script's 30 checks red.
# They now live in the /data/training/vax/cluster/captures-lab2 SIBLING. Full
# rationale in tools/cluster/scs_disc_measure.py.
LAB2_MARKER = "-lab2-"


def lab1_only(paths):
    """Return `paths` unchanged, or die naming every lab-2 capture in them."""
    foreign = sorted(os.path.basename(p) for p in paths
                     if LAB2_MARKER in os.path.basename(p))
    if foreign:
        raise SystemExit(
            "lab fence: %d lab-2 capture(s) in the lab-1 grounding library. "
            "Move them to a <capdir>-lab2 sibling and re-run; do NOT raise "
            "EXPECTED to absorb them.\n  %s" % (len(foreign), "\n  ".join(foreign)))
    return paths

SCS_MARKER = b"\x4b\x13"
# The SCS message-marker family. All three are SCS messages on the same
# connections and all three carry the credit field at SCA [48:50]; 0x4113 (the
# START/config layer) is NOT a member and is the whole "why 106 is not here"
# story. See the WIRE VERDICT.
SCS_FAMILY = (b"\x4b\x13", b"\x5b\x13", b"\x7b\x13")

GROUNDING_CAPTURES = ("formation-ci1.pcap", "formation-ci1-joinwindow.pcap")

VAX1 = "aa:00:04:00:01:04"
PEER = "08:00:2b:78:56:b9"

# ---------------------------------------------------------------------------
# The recorded measurement. Every entry appears verbatim in the WIRE VERDICT.
# ---------------------------------------------------------------------------
EXPECTED = {
    # 1. CONSERVATION -- computed over ALL 190-byte frames, NO marker filter.
    #    (Equivalently the whole 0x?B13 family: at this length the two sets are
    #    identical, which "family_equals_all" below asserts.)
    "conservation": {
        "formation-ci1.pcap": {VAX1: (10842, 10842), PEER: (6712, 6715)},
        "formation-ci1-joinwindow.pcap": {VAX1: (1601, 1602), PEER: (1300, 1300)},
    },
    # The REFUTATION: the same identity computed with the 0x4B13 filter, which
    # is what an earlier revision of the WIRE VERDICT mandated. It does not
    # hold. These figures are quoted in the correction note in scs_credit.h and
    # in the spec, so they are measured here too rather than asserted.
    "conservation_4b13_refuted": {
        "formation-ci1.pcap": {VAX1: (10817, 10266), PEER: (6369, 6695)},
    },
    # 190-byte frames in the two grounding captures, by marker.
    "grounding_190_markers": {"4b13": 19860, "5b13": 591, "7b13": 8},
    "grounding_190_total": 20459,
    # 2. VALUE SHAPE -- these two DO use the 0x4B13 filter.
    "shape_all_190": {0: 5418, 1: 11042, 2: 2587, 3: 1409, 4: 3},
    "shape_4b13_190": {0: 5174, 1: 10696, 2: 2582, 3: 1405, 4: 3},
    "shape_4b13_190_n": 19860,
    # 3. Per-class sweep over ALL 47 captures, 0x4B13 filter: len -> (n, distinct, max)
    "classes_admitted": {
        58: (1212, 2, 1),
        62: (1087, 1, 0),
        66: (944, 1, 0),
        86: (194, 1, 1),
        94: (3670, 2, 1),
        110: (3999, 5, 10),
        190: (288484, 5, 4),
    },
    "classes_refused": {
        50: (51, 47, 64897),
        70: (917, 752, 65447),
        122: (4, 3, 10709),
        126: (3, 3, 64891),
        142: (8, 8, 21361),
    },
    # The 110-byte tunable values (CLUSTER_CREDITS 10, MSCP_CREDITS 8, ...).
    "class_110_values": [1, 3, 6, 8, 10],
    # 106 is NOT an SCS class: every 106-byte SCA frame is marker 0x4113 and its
    # [48:50] is a constant 0.
    "class_106_markers": {"4113": 792},
    "class_106_values": {0: 792},
    # Sibling markers at the same lengths: (len, marker) -> (n, max)
    "siblings": {
        (190, "5b13"): (18086, 3),
        (190, "7b13"): (100, 3),
        (110, "5b13"): (675, 10),
        (110, "7b13"): (106, 10),
    },
}


# ---------------------------------------------------------------------------
# classic pcap reader (no scapy; the captures are all little-endian classic)
# ---------------------------------------------------------------------------
def frames(path):
    with open(path, "rb") as f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        magic = gh[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif magic == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            raise SystemExit("%s: not a classic pcap (magic %r)" % (path, magic))
        while True:
            rh = f.read(16)
            if len(rh) < 16:
                return
            _, _, incl, _orig = struct.unpack(end + "IIII", rh)
            data = f.read(incl)
            if len(data) < incl:
                return
            yield data


def mac(b):
    return ":".join("%02x" % x for x in b)


def credit(sca):
    """The credit field: SCA [48:50] LE u16 (absolute frame offset [62:64])."""
    return struct.unpack("<H", sca[48:50])[0]


# ---------------------------------------------------------------------------
# measurements
# ---------------------------------------------------------------------------
def measure_grounding(capdir):
    """The two grounding captures: conservation, marker split, value shape."""
    out = {
        "conservation": {},
        "conservation_family": {},
        "conservation_4b13": {},
        "grounding_190_markers": collections.Counter(),
        "shape_all_190": collections.Counter(),
        "shape_4b13_190": collections.Counter(),
    }
    for cap in GROUNDING_CAPTURES:
        granted = collections.Counter()
        sent = collections.Counter()
        fam_granted = collections.Counter()
        fam_sent = collections.Counter()
        m4_granted = collections.Counter()
        m4_sent = collections.Counter()
        for fr in frames(os.path.join(capdir, cap)):
            sca = fr[14:]
            if len(sca) != 190:
                continue
            src = mac(fr[6:12])
            c = credit(sca)
            marker = sca[16:18]
            out["grounding_190_markers"][marker.hex()] += 1
            out["shape_all_190"][c] += 1
            # line 1: ALL 190-byte frames, no marker filter
            granted[src] += c
            sent[src] += 1
            if marker in SCS_FAMILY:
                fam_granted[src] += c
                fam_sent[src] += 1
            if marker == SCS_MARKER:
                out["shape_4b13_190"][c] += 1
                m4_granted[src] += c
                m4_sent[src] += 1

        def pairs(g, s):
            nodes = sorted(set(list(g) + list(s)))
            return {
                n: (g[n], sum(v for k, v in s.items() if k != n)) for n in nodes
            }

        out["conservation"][cap] = pairs(granted, sent)
        out["conservation_family"][cap] = pairs(fam_granted, fam_sent)
        out["conservation_4b13"][cap] = pairs(m4_granted, m4_sent)
    return out


def measure_classes(capdir):
    """Sweep every capture: per-length value tables, 106, sibling markers."""
    per = collections.defaultdict(collections.Counter)
    c106_markers = collections.Counter()
    c106_values = collections.Counter()
    siblings = collections.defaultdict(collections.Counter)
    paths = lab1_only(sorted(glob.glob(os.path.join(capdir, "*.pcap"))))
    for p in paths:
        for fr in frames(p):
            sca = fr[14:]
            n = len(sca)
            if n < 50:
                continue
            marker = sca[16:18]
            c = credit(sca)
            if n == 106:
                c106_markers[marker.hex()] += 1
                c106_values[c] += 1
            if marker == SCS_MARKER:
                per[n][c] += 1
            elif marker in SCS_FAMILY and n in (110, 190):
                siblings[(n, marker.hex())][c] += 1
    return {
        "n_captures": len(paths),
        "classes": {k: (sum(v.values()), len(v), max(v)) for k, v in per.items()},
        "class_110_values": sorted(per[110]) if 110 in per else [],
        "class_106_markers": dict(c106_markers),
        "class_106_values": dict(c106_values),
        "siblings": {k: (sum(v.values()), max(v)) for k, v in siblings.items()},
    }


# ---------------------------------------------------------------------------
def check(results, label, got, want):
    ok = got == want
    results.append((ok, label, got, want))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--quick", action="store_true",
                    help="two grounding captures only; skip the 47-file sweep")
    ap.add_argument("--print", dest="dump", action="store_true",
                    help="print measurements instead of checking them")
    args = ap.parse_args()

    if not os.path.isdir(args.captures):
        sys.exit("captures not found: %s\n"
                 "These are host-only lab-1 data, not in git. See CLAUDE.md rule 8."
                 % args.captures)

    g = measure_grounding(args.captures)
    c = None if args.quick else measure_classes(args.captures)

    if args.dump:
        import pprint
        pprint.pprint(g)
        if c:
            pprint.pprint(c)
        return 0

    r = []

    # --- evidence line 1: conservation, ALL 190-byte frames, NO marker filter
    for cap, want in EXPECTED["conservation"].items():
        check(r, "conservation(all-190) %s" % cap, g["conservation"][cap], want)
        # the equivalence that justifies "all 190-byte frames" == "the 0x?B13
        # family": at this length every frame is a family member.
        check(r, "family_equals_all %s" % cap,
              g["conservation_family"][cap], g["conservation"][cap])

    # ...and the refutation the correction note quotes: filtering BREAKS it.
    for cap, want in EXPECTED["conservation_4b13_refuted"].items():
        check(r, "conservation(0x4B13-filtered, REFUTED) %s" % cap,
              g["conservation_4b13"][cap], want)
        for node, (granted, peer_sent) in want.items():
            check(r, "filtering really does break the identity (%s)" % node,
                  granted != peer_sent, True)

    check(r, "grounding_190_markers",
          dict(g["grounding_190_markers"]), EXPECTED["grounding_190_markers"])
    check(r, "grounding_190_total",
          sum(g["grounding_190_markers"].values()), EXPECTED["grounding_190_total"])

    # --- evidence line 2: value shape (0x4B13-filtered), and the unfiltered one
    check(r, "shape_all_190", dict(sorted(g["shape_all_190"].items())),
          EXPECTED["shape_all_190"])
    check(r, "shape_4b13_190", dict(sorted(g["shape_4b13_190"].items())),
          EXPECTED["shape_4b13_190"])
    check(r, "shape_4b13_190_n", sum(g["shape_4b13_190"].values()),
          EXPECTED["shape_4b13_190_n"])

    if c:
        check(r, "n_captures", c["n_captures"], 47)
        for k, want in EXPECTED["classes_admitted"].items():
            check(r, "class %d (admitted)" % k, c["classes"].get(k), want)
        for k, want in EXPECTED["classes_refused"].items():
            check(r, "class %d (refused)" % k, c["classes"].get(k), want)
        check(r, "class_110_values", c["class_110_values"],
              EXPECTED["class_110_values"])
        check(r, "class_106_markers", c["class_106_markers"],
              EXPECTED["class_106_markers"])
        check(r, "class_106_values", c["class_106_values"],
              EXPECTED["class_106_values"])
        check(r, "siblings", c["siblings"], EXPECTED["siblings"])
        # 106 must contain NO SCS message at all.
        check(r, "no 106-byte 0x4B13 frames", c["classes"].get(106), None)

    bad = 0
    for ok, label, got, want in r:
        if ok:
            print("ok   %s" % label)
        else:
            bad += 1
            print("FAIL %s\n       got  %r\n       want %r" % (label, got, want))
    print("\n%d checks, %d failures%s" % (len(r), bad, " (--quick)" if args.quick else ""))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
