#!/usr/bin/env python3
"""scs_reason_measure.py - vms-6b3: re-derive the REJECT/DISCONNECT reason-code
measurement from the raw lab captures.

Every number quoted in src/vmsscs/include/scs_reason.h and in
docs/cluster-protocol-spec.md (sec 4(h)(1a) and the sec 5 "reason code" entry)
comes out of this script. It reads pcaps only; it imports nothing from OVMX
except the pure-stdlib pcap reader in dissect_sca.py.

    tools/cluster/scs_reason_measure.py           # re-measure and PASS/FAIL vs EXPECTED
    tools/cluster/scs_reason_measure.py --print   # just print what the captures say

Requires the lab captures, host-only and NOT in git (47 files, see CLAUDE.md
rule 8): /data/training/vax/cluster/captures/*.pcap. Override with --captures.

EXPECTED below is the checked-in record of what the captures measured on
2026-08-05. `ctest -R scs_reason_figures` does NOT need the captures: it asserts
every figure in EXPECTED still appears in scs_reason.h and in the spec, so the
prose cannot drift away from the measurement. Only this script, on a host with
the captures, re-derives EXPECTED itself.

----------------------------------------------------------------------------
WHAT IT MEASURES, AND WHY EACH PART EXISTS
----------------------------------------------------------------------------

(A) THE TWO CARRIER FRAMES. Every SCA (0x6007) frame whose total SCA length is
    62 -- the connection-control class of spec sec 4(h)(1a) -- whose message
    type at payload [46:48] is 4 (REJECT_REQ) or 6 (DISCONNECT_REQ), from a
    VMS-origin source MAC (DEC OUI 08-00-2b, or the LAVC logical
    aa-00-04-00-xx-04) so no OVMX-emitted frame can be counted as a VMS
    observation. Prints a per-offset value census of the whole payload and the
    two 16-bit words that follow the Con.ID pair at [50:58].

(B) THE NEIGHBOUR CENSUS -- ADDED AFTER REVIEW, AND IT REFUTED THE FIRST
    RATIONALE. The first revision of this item justified the OVMX placement as
    "the only slot zero in 100% of observed frames". That is FALSE and this
    part is what falsifies it: the SAME population rule applied to the rest of
    the connection-control envelope (SCA classes 62/66/110, every message type)
    shows payload [58:60] carrying nonzero values on msgtype 3 (ACCEPT_RSP, on
    the SAME 62-byte layout), on msgtype 2 (ACCEPT_REQ) and on msgtype 0
    (CONNECT_REQ). The word is a per-message-type field, not dead space. The
    surviving -- and measured -- claim is narrower: it is zero in 100% of the
    two frames that CARRY the reason code, which is all the placement needs.

(C) THE DEAD-SLOT CENSUS. Which 16-bit-aligned payload slots are 0x0000 in
    100% of msgtype 4 and of msgtype 6. This is what makes the surviving
    "only" claim checkable: [58:60] is the only always-zero 16-bit slot at or
    after payload 50, i.e. outside the sequenced-message counter region
    [18:50] whose low halves demonstrably vary.

Everything here reads captured Ethernet frames only -- no VSI/HPE source or
binary is involved (CLAUDE.md rule 8).
"""
import argparse
import collections
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dissect_sca import read_pcap  # noqa: E402

DEFAULT_CAPDIR = "/data/training/vax/cluster/captures"

# THE LAB FENCE (vms-096). DEFAULT_CAPDIR is the LAB-1 grounding library and
# every figure in EXPECTED is a lab-1 measurement. lab-2 replicas reuse lab-1's
# SCSSYSTEMIDs and node MACs by design (tests/lab/README.md), so a lab-2
# capture deposited here silently moves every census -- six 2026-08-05
# vaxlab-4 captures did exactly that and put 13 of this script's 27 checks red.
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

SCA_ETHERTYPE = b"\x60\x07"
CONNCTL_SCA_LEN = 62
CONNCTL_CLASSES = (62, 66, 110)   # spec sec 4(h)(1a) connection-control lengths
MSGTYPE_ABS = 60          # payload [46:48] -> absolute 60
REASON_ABS = 72           # payload [58:60] -> absolute 72 (the OVMX placement)
NEXT_ABS = 74             # payload [60:62] -> absolute 74
CONID_PAYLOAD_END = 58    # the Con.ID pair occupies payload [50:58]
COUNTER_PAYLOAD_END = 50  # the SCS sequenced-message counter region is [18:50]
CARRIERS = {4: "REJECT_REQ", 6: "DISCONNECT_REQ"}
# The SDA oracle: `SHOW CONNECTIONS` output captured from VAX1, alongside the
# pcaps. Its per-CDT "Rej/Disconn Reason" field is the second oracle.
SDA_EXTRACT = "sda-scs-extract-vax1.txt"
SDA_FIELD = "Rej/Disconn Reason"
MSGTYPE_NAMES = {0: "CONNECT_REQ", 1: "CONNECT_RSP", 2: "ACCEPT_REQ",
                 3: "ACCEPT_RSP", 4: "REJECT_REQ", 6: "DISCONNECT_REQ",
                 10: "APPLICATION"}   # 10 = application data, not conn-control

# ---------------------------------------------------------------------------
# The recorded measurement. Every figure here is quoted in scs_reason.h and in
# docs/cluster-protocol-spec.md, and tests/vmsscs/test_scs_reason_figures.py
# asserts it is still there.
# ---------------------------------------------------------------------------
EXPECTED = {
    "n_captures": 47,

    # (A) the two frames that carry the reason code (p. 2-26).
    #     off58/off60 are {value: count} over the whole population.
    "carriers": {
        4: {"name": "REJECT_REQ", "frames": 453, "pcaps": 19,
            "off58": {0x0000: 453},
            "off60": {0x0001: 453}},
        6: {"name": "DISCONNECT_REQ", "frames": 220, "pcaps": 25,
            "off58": {0x0000: 220},
            "off60": {0x0000: 131, 0x0001: 89}},
    },

    # (B) the neighbour census: (SCA length, msgtype) -> what [58:60] does.
    #     THIS IS THE REFUTATION of "the only slot zero in 100% of observed
    #     frames". Three of these six set it.
    "neighbours": {
        (62, 3): {"name": "ACCEPT_RSP", "frames": 258, "pcaps": 33, "nonzero58": 62},
        (62, 4): {"name": "REJECT_REQ", "frames": 453, "pcaps": 19, "nonzero58": 0},
        (62, 6): {"name": "DISCONNECT_REQ", "frames": 220, "pcaps": 25, "nonzero58": 0},
        (66, 1): {"name": "CONNECT_RSP", "frames": 778, "pcaps": 26, "nonzero58": 0},
        (110, 0): {"name": "CONNECT_REQ", "frames": 1101, "pcaps": 35, "nonzero58": 809},
        (110, 2): {"name": "ACCEPT_REQ", "frames": 324, "pcaps": 25, "nonzero58": 101},
        (110, 10): {"name": "APPLICATION", "frames": 2889, "pcaps": 39, "nonzero58": 2889},
    },
    # The two nonzero values the neighbours put in the slot on the shapes that
    # matter most to the argument: msgtype 3 shares the 62-byte layout with
    # 4 and 6, msgtype 2 is the ACCEPT_REQ half of the same dialogue.
    "neighbour_values": {
        (62, 3): {0x0000: 196, 0x0001: 62},
        (110, 2): {0x0000: 223, 0x0001: 101},
    },

    # (C) 16-bit-aligned payload slots that are 0x0000 in 100% of the frames of
    #     that message type. Identical for both carriers -- that is the point.
    "zero_slots": {4: (28, 32, 36, 48, 58), 6: (28, 32, 36, 48, 58)},
    # ...and the subset at or after payload 50, i.e. past the sequenced-message
    # counter region. This singleton is the ONLY surviving "only" claim.
    "zero_slots_after_counters": (58,),

    # (D) THE SECOND ORACLE. SDA `SHOW CONNECTIONS` prints a per-CDT field
    #     literally named "Rej/Disconn Reason". Counted out of the checked-in
    #     SDA extract so this figure is measured too, not asserted.
    "sda": {"file": SDA_EXTRACT, "cdts": 12, "values": {0: 12}},
}


def is_vms_origin(frame):
    mac = frame[6:12].hex()
    return mac.startswith("08002b") or mac.startswith("aa000400")


def hist(counter):
    return {v: n for v, n in counter.items()}


def measure(capdir):
    files = lab1_only(sorted(glob.glob(os.path.join(capdir, "*.pcap"))))
    rows = collections.defaultdict(list)          # (scalen, msgtype) -> frames
    skipped = []
    for path in files:
        try:
            frames = read_pcap(path)
        except Exception as exc:  # a truncated file must not hide the rest
            skipped.append((os.path.basename(path), str(exc)))
            continue
        base = os.path.basename(path)
        for _s, _u, _o, frame in frames:
            if len(frame) < 76 or frame[12:14] != SCA_ETHERTYPE:
                continue
            scalen = (frame[14] | (frame[15] << 8)) + 2
            if scalen not in CONNCTL_CLASSES or not is_vms_origin(frame):
                continue
            mtype = frame[MSGTYPE_ABS] | (frame[MSGTYPE_ABS + 1] << 8)
            rows[(scalen, mtype)].append((base, bytes(frame[:76])))

    out = {"n_captures": len(files), "skipped": skipped,
           "carriers": {}, "neighbours": {}, "neighbour_values": {},
           "zero_slots": {}, "byte_census": {}}

    for key in sorted(rows):
        rs = rows[key]
        c58 = collections.Counter(r[1][REASON_ABS] | (r[1][REASON_ABS + 1] << 8) for r in rs)
        out["neighbours"][key] = {
            "name": MSGTYPE_NAMES.get(key[1], "?"),
            "frames": len(rs),
            "pcaps": len(set(r[0] for r in rs)),
            "nonzero58": sum(n for v, n in c58.items() if v != 0),
        }
        out["neighbour_values"][key] = hist(c58)

    for mtype in sorted(CARRIERS):
        rs = rows.get((CONNCTL_SCA_LEN, mtype), [])
        c58 = collections.Counter(r[1][REASON_ABS] | (r[1][REASON_ABS + 1] << 8) for r in rs)
        c60 = collections.Counter(r[1][NEXT_ABS] | (r[1][NEXT_ABS + 1] << 8) for r in rs)
        out["carriers"][mtype] = {
            "name": CARRIERS[mtype],
            "frames": len(rs),
            "pcaps": len(set(r[0] for r in rs)),
            "off58": hist(c58),
            "off60": hist(c60),
        }
        # per-byte census over the whole payload, for the printed report
        varying = []
        for off in range(14, 76):
            vals = collections.Counter(r[1][off] for r in rs)
            if len(vals) > 1:
                varying.append((off - 14, off, len(vals), vals.most_common(4)))
        out["byte_census"][mtype] = varying
        # 16-bit-aligned always-zero slots
        zeros = []
        for off in range(14, 75, 2):
            vals = set(r[1][off] | (r[1][off + 1] << 8) for r in rs)
            if vals == {0}:
                zeros.append(off - 14)
        out["zero_slots"][mtype] = tuple(zeros)

    # (D) the SDA oracle, counted rather than asserted.
    sda_path = os.path.join(capdir, SDA_EXTRACT)
    if os.path.isfile(sda_path):
        vals = collections.Counter()
        for line in open(sda_path, errors="replace"):
            if line.startswith(SDA_FIELD):
                rest = line[len(SDA_FIELD):].split()
                if rest:
                    vals[int(rest[0])] += 1
        out["sda"] = {"file": SDA_EXTRACT, "cdts": sum(vals.values()),
                      "values": dict(vals)}
    else:
        out["sda"] = None

    zs = [set(out["zero_slots"].get(m, ())) for m in CARRIERS]
    common = set.intersection(*zs) if zs and all(zs) else set()
    out["zero_slots_after_counters"] = tuple(
        sorted(s for s in common if s >= COUNTER_PAYLOAD_END))
    return out


def report(m):
    print("capture dir scan: %d pcaps" % m["n_captures"])
    for name, exc in m["skipped"]:
        print("SKIP %s: %s" % (name, exc))

    print("\n=== (A) THE TWO CARRIER FRAMES (SCA length 62, VMS-origin) ===")
    for mtype in sorted(CARRIERS):
        c = m["carriers"][mtype]
        print("\nmsgtype %d = %s: %d VMS-origin frames across %d pcaps"
              % (mtype, CARRIERS[mtype], c["frames"], c["pcaps"]))
        if not c["frames"]:
            continue
        print("  per-offset census, payload bytes that take more than one value:")
        for pay, off, ndist, common in m["byte_census"][mtype]:
            print("    payload[%2d] (abs %2d): %d distinct %s" % (pay, off, ndist, common))
        for lbl, key in (("payload[58:60]  <- THE OVMX REASON-CODE SLOT", "off58"),
                         ("payload[60:62]  (the next word after the Con.ID pair)", "off60")):
            print("  %s: %s" % (lbl, ", ".join(
                "0x%04x x%d" % (v, n) for v, n in sorted(c[key].items(), key=lambda kv: -kv[1]))))
        print("  frames with a NONZERO value at payload[58:60]: %d"
              % sum(n for v, n in c["off58"].items() if v != 0))

    print("\n=== (B) NEIGHBOUR CENSUS at payload[58:60]: the slot is NOT dead ===")
    print("    (same population rule, SCA classes 62/66/110, every message type)")
    print("    %-9s %-6s %-8s %-6s %-10s  %s"
          % ("SCA len", "type", "frames", "pcaps", "nonzero58", "name"))
    for key in sorted(m["neighbours"]):
        n = m["neighbours"][key]
        print("    %-9d %-6d %-8d %-6d %-10d  %s"
              % (key[0], key[1], n["frames"], n["pcaps"], n["nonzero58"],
                 MSGTYPE_NAMES.get(key[1], "?")))
    for key in sorted(m["neighbour_values"]):
        if m["neighbours"][key]["nonzero58"]:
            print("      len %d type %d [58:60] values: %s" % (
                key[0], key[1], ", ".join("0x%04x x%d" % (v, n) for v, n in
                                          sorted(m["neighbour_values"][key].items(),
                                                 key=lambda kv: -kv[1])[:4])))

    print("\n=== (C) DEAD-SLOT CENSUS (16-bit aligned, 0x0000 in 100%) ===")
    for mtype in sorted(CARRIERS):
        print("    msgtype %d: payload slots %s"
              % (mtype, list(m["zero_slots"].get(mtype, ()))))
    print("    common to BOTH carriers and at/after payload %d (past the"
          % COUNTER_PAYLOAD_END)
    print("    sequenced-message counter region): %s"
          % list(m["zero_slots_after_counters"]))

    print("\n=== (D) THE SDA ORACLE: per-CDT \"%s\" ===" % SDA_FIELD)
    if m["sda"] is None:
        print("    %s not present under the capture dir" % SDA_EXTRACT)
    else:
        print("    %s: %d CDTs, values %s"
              % (m["sda"]["file"], m["sda"]["cdts"],
                 ", ".join("%d x%d" % (v, n)
                           for v, n in sorted(m["sda"]["values"].items()))))


def check(results, label, got, want):
    results.append((got == want, label, got, want))


def verify(m):
    r = []
    check(r, "n_captures", m["n_captures"], EXPECTED["n_captures"])
    for mtype, want in EXPECTED["carriers"].items():
        got = m["carriers"].get(mtype, {})
        for k in ("name", "frames", "pcaps", "off58", "off60"):
            check(r, "carrier msgtype %d %s" % (mtype, k), got.get(k), want[k])
        check(r, "carrier msgtype %d is zero at [58:60] in 100%%" % mtype,
              sum(n for v, n in got.get("off58", {}).items() if v != 0), 0)
    for key, want in EXPECTED["neighbours"].items():
        check(r, "neighbour len=%d type=%d" % key, m["neighbours"].get(key), want)
    for key, want in EXPECTED["neighbour_values"].items():
        check(r, "neighbour values len=%d type=%d" % key,
              m["neighbour_values"].get(key), want)
    # The structural fact the corrected rationale rests on: at least one
    # NEIGHBOURING message type does set [58:60]. If this ever becomes false the
    # rationale in scs_reason.h is over-cautious and should be revisited.
    check(r, "the slot is in live use by neighbouring message types",
          sorted(k for k, v in m["neighbours"].items()
                 if v["nonzero58"] and k[1] not in CARRIERS),
          [(62, 3), (110, 0), (110, 2), (110, 10)])
    for mtype, want in EXPECTED["zero_slots"].items():
        check(r, "zero slots msgtype %d" % mtype, m["zero_slots"].get(mtype), want)
    check(r, "zero slots at/after the counter region (the surviving 'only')",
          m["zero_slots_after_counters"], EXPECTED["zero_slots_after_counters"])
    check(r, "SDA oracle: per-CDT %r" % SDA_FIELD, m["sda"], EXPECTED["sda"])
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--captures", default=DEFAULT_CAPDIR)
    ap.add_argument("capdir_positional", nargs="?", default=None,
                    help="legacy positional form of --captures")
    ap.add_argument("--print", dest="dump", action="store_true",
                    help="print measurements instead of checking them")
    args = ap.parse_args()
    capdir = args.capdir_positional or args.captures

    if not os.path.isdir(capdir):
        sys.exit("captures not found: %s\n"
                 "These are host-only lab-1 data, not in git. See CLAUDE.md rule 8."
                 % capdir)

    m = measure(capdir)
    report(m)

    if args.dump:
        return 0

    print("\n=== VERIFY against the checked-in EXPECTED record ===")
    bad = 0
    results = verify(m)
    for ok, label, got, want in results:
        if ok:
            print("ok   %s" % label)
        else:
            bad += 1
            print("FAIL %s\n       got  %r\n       want %r" % (label, got, want))
    print("\n%d checks, %d failures" % (len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
