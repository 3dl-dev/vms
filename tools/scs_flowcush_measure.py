#!/usr/bin/env python3
"""
scs_flowcush_measure.py -- re-derive the SCSFLOWCUSH / type-8 experiment (vms-f03).

WHAT THIS MEASURES

vms-f03 ran the decisive experiment of docs/design-mscp-direction.md sec 1.3 on a
lab-2 replica: hold a heavy two-node SYSAP flow constant and vary ONE documented
knob -- the local SYSGEN parameter SCSFLOWCUSH on VAX1 -- across 0, 1, 8, 16 and
back to 1. *VAXcluster Principles* p. 2-44 defines that knob as exactly half of
the special-credit trigger: "The VMS implementation of SCS considers the local
Receive Credit count to be dangerously low if it is less than the sum of the
local SYSGEN parameter SCSFLOWCUSH and the remote value for Minimum Send
Credits."

EXPECTED below is the checked-in record of what the five captures measured on
2026-08-06. Every figure appears verbatim in docs/cluster-protocol-spec.md
sec 4(h)(1g) and docs/design-mscp-direction.md sec 1.3; the ctest gate
tests/vmsscs/test_scs_flowcush_figures.py pins prose to this table, and on a
host that HAS the captures calls rederive() so that green means
wire == EXPECTED == prose.

CLEAN ROOM (CLAUDE.md rule 8). Inputs are our own lab-2 captures plus public
OpenVMS documentation (VAXcluster Principles ch.2; SYSGEN's own SHOW output on
the node). Nothing here reads a VSI/HPE binary or source artifact.

THE CAPTURES ARE LAB-2 AND MUST STAY OUT OF THE LAB-1 LIBRARY (vms-096). They
live in the captures-lab2 sibling and every filename carries the `-lab2-`
marker that tools/scs_credit_measure.py's lab fence keys on.
"""
import argparse
import os
import struct
import sys

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures-lab2/vms-f03"

# Node identity was proven ON THE NODE, not inferred from the OUI: VAX1's own
# `NCP SHOW EXECUTOR STATUS` printed `Physical address = AA-00-04-00-01-04`, and
# VAX2 answered the same command %SYSTEM-W-NOSUCHDEV (no DECnet), so it keeps
# the SIMH hardware MAC. See spec sec 4(h)(1g).
VAX1 = "aa0004000104"   # the node whose SCSFLOWCUSH was varied
VAX2 = "08002b620209"   # the matched same-wire control node, cushion fixed at 1

# tag -> SCSFLOWCUSH on VAX1 during that capture (read back from SYSGEN SHOW).
RUNS = (
    ("B1-cush1-pre", 1),
    ("D1-cush0", 0),
    ("E1-cush8", 8),
    ("C1-cush16", 16),
    ("B2-cush1-post", 1),
)

FMT_WORD = 0x0004

# ---------------------------------------------------------------------------
# The recorded measurement.
# ---------------------------------------------------------------------------
EXPECTED = {
    # 1. DOSE-RESPONSE. type-8 frames emitted by VAX1 per run, bracketed by a
    #    cushion-1 run on BOTH sides. Zero is zero, not "few".
    "dose_response": {
        "B1-cush1-pre": (1, 0),
        "D1-cush0": (0, 0),
        "E1-cush8": (8, 26719),
        "C1-cush16": (16, 64305),
        "B2-cush1-post": (1, 0),
    },
    # 2. EMITTER ASYMMETRY. Only the cushion-varied node emits 8; only its peer
    #    emits 9; never the reverse, in any run.
    "emitters": {
        "E1-cush8": {(VAX1, 8): 26719, (VAX2, 9): 26719},
        "C1-cush16": {(VAX1, 8): 64305, (VAX2, 9): 64305},
    },
    # 3. PAIRING. Every type 8 is answered by exactly one type 9 from the peer.
    #    (matched, unmatched_9, unanswered_8)
    "pairing": {
        "E1-cush8": (26719, 0, 0),
        "C1-cush16": (64305, 0, 0),
    },
    # 4. THE ECHO. credit[48:50] on the type 9 equals credit[48:50] on the type
    #    8 it answers, in every pair. Recorded as the (c8, c9) histogram.
    "credit_pairs": {
        "E1-cush8": {(0, 0): 862, (1, 1): 15008, (2, 2): 10848, (3, 3): 1},
        "C1-cush16": {(0, 0): 7388, (1, 1): 56823, (2, 2): 93, (3, 3): 1},
    },
    # 5. FORWARD CONSERVATION -- the decode.
    #    credit(type-10 from VAX1) + credit(type-8 from VAX1)
    #        == messages VAX1 received from VAX2.
    #    Per run: (v2_t10_msgs, v1_t10_credit, v1_t8_credit, delta).
    "conservation_forward": {
        "B1-cush1-pre": (223975, 224020, 0, 45),
        "D1-cush0": (221413, 221439, 0, 26),
        "E1-cush8": (215873, 179192, 36707, 26),
        "C1-cush16": (196411, 139429, 57012, 30),
        "B2-cush1-post": (217799, 217849, 0, 50),
    },
    # 6. REVERSE ACCOUNTING -- what classifies type 9, and what type 8 costs.
    #    (v1_t10_msgs, v1_t8_msgs, v2_t10_credit, v2_t9_credit)
    #    v2_t10_credit - v1_t10_msgs stays a small constant in EVERY run, so
    #    (a) a type-8 does NOT consume a send credit and (b) the type-9 value is
    #    an echo, not currency -- adding it overshoots by exactly its own total.
    "conservation_reverse": {
        "B1-cush1-pre": (216392, 0, 216359, 0),
        "D1-cush0": (215067, 0, 215044, 0),
        "E1-cush8": (210521, 26719, 210496, 36707),
        "C1-cush16": (190847, 64305, 190815, 57012),
        "B2-cush1-post": (210159, 0, 210129, 0),
    },
    # 7. VALUE DOSE-RESPONSE. Mean credit carried by a type 8. A HIGHER cushion
    #    fires the message EARLIER, so it carries a SMALLER accumulated pending
    #    count. Rounded to 4 dp.
    "mean_credit_per_type8": {"E1-cush8": 1.3738, "C1-cush16": 0.8866},
    # 8. The 58-content shape is unchanged by the condition: inner length 14,
    #    content length 58, no payload.
    "shape": {"inner_len": 14, "content_len": 58},
    # NON-WIRE: recorded from the node's own console, not derivable from a pcap.
    "sysgen": {"default": 1, "min": 0, "max": 16, "dynamic": True},
}

WIRE_KEYS = ("dose_response", "emitters", "pairing", "credit_pairs",
             "conservation_forward", "conservation_reverse",
             "mean_credit_per_type8", "shape")
NON_WIRE_KEYS = ("sysgen",)


# ---------------------------------------------------------------------------
# pcap reading -- deliberately self-contained (tools/ must not depend on docs/).
# ---------------------------------------------------------------------------
def frames(path):
    with open(path, "rb") as fh:
        data = fh.read()
    nano = struct.unpack("<I", data[:4])[0] == 0xA1B23C4D
    off, out = 24, []
    while off + 16 <= len(data):
        ts_s, ts_u, incl, _orig = struct.unpack("<IIII", data[off:off + 16])
        off += 16
        out.append((ts_s + ts_u / (1e9 if nano else 1e6), data[off:off + incl]))
        off += incl
    return out


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def events(path):
    """Yield (ts, src, mtype, credit, dsthandle, srchandle, innerlen, clen).

    Only frames that actually carry the SCS envelope: ethertype 0x6007 and the
    0x0004 format word at content[44:46]. That guard is not optional -- the
    70-content class fails it and reading [46:48] there is a misread field
    (spec sec 4(h)(1d)).
    """
    out = []
    for ts, pkt in frames(path):
        if len(pkt) < 14 or pkt[12:14] != b"\x60\x07":
            continue
        c = pkt[14:]
        if len(c) < 58 or u16(c, 44) != FMT_WORD:
            continue
        out.append((ts, pkt[6:12].hex(), u16(c, 46), u16(c, 48),
                    c[50:54].hex(), c[54:58].hex(), u16(c, 42), len(c)))
    out.sort(key=lambda e: e[0])
    return out


def cap(capdir, tag):
    return os.path.join(capdir, "f03-lab2-%s.pcap" % tag)


# ---------------------------------------------------------------------------
def measure(capdir):
    m = {"dose_response": {}, "emitters": {}, "pairing": {}, "credit_pairs": {},
         "conservation_forward": {}, "conservation_reverse": {},
         "mean_credit_per_type8": {}, "shape": None}
    shapes = set()
    for tag, cush in RUNS:
        ev = events(cap(capdir, tag))
        n8 = sum(1 for e in ev if e[2] == 8 and e[1] == VAX1)
        m["dose_response"][tag] = (cush, n8)

        em = {}
        for _ts, src, mt, _cr, _d, _s, _il, _cl in ev:
            if mt in (8, 9):
                em[(src, mt)] = em.get((src, mt), 0) + 1
        if em:
            m["emitters"][tag] = em

        # pairing + echo: a type 8 is answered by a type 9 whose handle pair is
        # the reverse of the 8's. NOTE both handles are the same 4-byte value on
        # this lab, so this reversal is NOT a discriminating test here -- the
        # pairing that IS proven is by direction and adjacency in time.
        pend, matched, unmatched, pairs = {}, 0, 0, {}
        for _ts, _src, mt, cr, dh, sh, _il, _cl in ev:
            if mt == 8:
                pend.setdefault((sh, dh), []).append(cr)
            elif mt == 9:
                q = pend.get((dh, sh))
                if q:
                    c0 = q.pop(0)
                    matched += 1
                    pairs[(c0, cr)] = pairs.get((c0, cr), 0) + 1
                    if not q:
                        del pend[(dh, sh)]
                else:
                    unmatched += 1
        if matched or unmatched:
            m["pairing"][tag] = (matched, unmatched, sum(len(v) for v in pend.values()))
            m["credit_pairs"][tag] = pairs

        v2_t10_msgs = v1_t10_cr = v1_t8_cr = 0
        v1_t10_msgs = v1_t8_msgs = v2_t10_cr = v2_t9_cr = 0
        t8cr = t8n = 0
        for _ts, src, mt, cr, _d, _s, il, cl in ev:
            if mt in (8, 9):
                shapes.add((il, cl))
            if src == VAX2:
                if mt == 10:
                    v2_t10_msgs += 1
                    v2_t10_cr += cr
                elif mt == 9:
                    v2_t9_cr += cr
            elif src == VAX1:
                if mt == 10:
                    v1_t10_msgs += 1
                    v1_t10_cr += cr
                elif mt == 8:
                    v1_t8_msgs += 1
                    v1_t8_cr += cr
                    t8cr += cr
                    t8n += 1
        m["conservation_forward"][tag] = (v2_t10_msgs, v1_t10_cr, v1_t8_cr,
                                          v1_t10_cr + v1_t8_cr - v2_t10_msgs)
        m["conservation_reverse"][tag] = (v1_t10_msgs, v1_t8_msgs, v2_t10_cr, v2_t9_cr)
        if t8n:
            m["mean_credit_per_type8"][tag] = round(t8cr / t8n, 4)
    if len(shapes) == 1:
        il, cl = shapes.pop()
        m["shape"] = {"inner_len": il, "content_len": cl}
    else:
        m["shape"] = {"inner_len": None, "content_len": None}
    return m


def verify(m):
    r = []

    def eq(label, got, want):
        r.append((got == want, label, got, want))

    eq("dose-response: type-8 count per SCSFLOWCUSH",
       m["dose_response"], EXPECTED["dose_response"])
    # The claim the dose-response exists to make, stated so it can fail on its own.
    zero_at_baseline = all(n == 0 for c, n in m["dose_response"].values() if c <= 1)
    r.append((zero_at_baseline,
              "type 8 is ABSENT at SCSFLOWCUSH <= 1 in every run (both brackets)",
              {t: v for t, v in m["dose_response"].items() if v[0] <= 1}, "all 0"))
    raised = sorted((c, n) for c, n in m["dose_response"].values() if c > 1)
    r.append((len(raised) >= 2 and all(raised[i][1] < raised[i + 1][1]
                                       for i in range(len(raised) - 1)),
              "type-8 count is strictly monotonic in SCSFLOWCUSH", raised, "increasing"))

    eq("emitter asymmetry: only VAX1 emits 8, only VAX2 emits 9",
       m["emitters"], EXPECTED["emitters"])
    eq("pairing: (matched, unmatched 9, unanswered 8)",
       m["pairing"], EXPECTED["pairing"])
    eq("credit echo: (credit on 8, credit on 9) histogram",
       m["credit_pairs"], EXPECTED["credit_pairs"])
    echoes = all(a == b for tag in m["credit_pairs"] for (a, b) in m["credit_pairs"][tag])
    r.append((echoes, "the type-9 credit value ALWAYS equals the type-8 it answers",
              sorted({(a, b) for t in m["credit_pairs"] for (a, b) in m["credit_pairs"][t]}),
              "a == b"))

    eq("forward conservation: (v2 t10 msgs, v1 t10 credit, v1 t8 credit, delta)",
       m["conservation_forward"], EXPECTED["conservation_forward"])
    worst = max((abs(v[3]) / v[0] for v in m["conservation_forward"].values()), default=1)
    r.append((worst < 0.001,
              "credit(t10)+credit(t8) from VAX1 accounts for every message it "
              "received, to better than 0.1 percent", round(worst * 100, 4), "< 0.1%"))

    eq("reverse accounting: (v1 t10 msgs, v1 t8 msgs, v2 t10 credit, v2 t9 credit)",
       m["conservation_reverse"], EXPECTED["conservation_reverse"])
    # A type-8 does not consume a send credit: VAX2's ordinary piggyback covers
    # VAX1's type-10 messages ALONE, in the runs with type 8 as in those without.
    d_t10 = {t: v[2] - v[0] for t, v in m["conservation_reverse"].items()}
    r.append((max(abs(x) for x in d_t10.values()) < 100 and
              all(abs(v[2] - (v[0] + v[1])) > 1000
                  for v in m["conservation_reverse"].values() if v[1]),
              "a type-8 does NOT consume a send credit (peer's piggyback covers "
              "type-10 alone, in every run)", d_t10, "small in all runs"))
    # The type-9 value is not currency: adding it overshoots by its own total.
    over = {t: (v[2] + v[3]) - v[0] for t, v in m["conservation_reverse"].items()}
    r.append((all(abs(over[t] - m["conservation_reverse"][t][3]) < 100
                  for t in over),
              "the type-9 credit value is an ECHO, not a credit return (adding it "
              "overshoots by exactly its own total)", over, "== v2 t9 credit"))

    eq("mean credit carried by a type 8", m["mean_credit_per_type8"],
       EXPECTED["mean_credit_per_type8"])
    mc = sorted((m["dose_response"][t][0], v)
                for t, v in m["mean_credit_per_type8"].items())
    r.append((len(mc) >= 2 and all(mc[i][1] > mc[i + 1][1] for i in range(len(mc) - 1)),
              "a HIGHER cushion fires earlier, so the mean pending count carried "
              "is SMALLER", mc, "decreasing in cushion"))

    eq("58-content shape unchanged (inner length, content length)",
       m["shape"], EXPECTED["shape"])
    return r


def rederive(capdir, **_kw):
    """THE ctest GATE'S ENTRY POINT (vms-371 contract)."""
    m = measure(capdir)
    return [(ok, label) for ok, label, _g, _w in verify(m)], set(WIRE_KEYS)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("--print", dest="dump", action="store_true")
    args = ap.parse_args()
    if not os.path.isdir(args.captures):
        sys.exit("captures not found: %s\nThese are host-only lab-2 data, not in "
                 "git. See CLAUDE.md rule 8." % args.captures)
    missing = [t for t, _ in RUNS if not os.path.exists(cap(args.captures, t))]
    if missing:
        sys.exit("missing capture(s): %s" % ", ".join(missing))
    m = measure(args.captures)
    if args.dump:
        import pprint
        pprint.pprint(m)
        return 0
    bad = 0
    for ok, label, got, want in verify(m):
        if ok:
            print("ok   %s" % label)
        else:
            bad += 1
            print("FAIL %s\n       got  %r\n       want %r" % (label, got, want))
    print("\n%d checks, %d failures" % (len(verify(m)), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
