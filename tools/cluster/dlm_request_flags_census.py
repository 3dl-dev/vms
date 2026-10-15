#!/usr/bin/env python3
"""dlm_request_flags_census.py -- is body[28:30] of a DLM lock request a FIELD
or the sender's stale buffer? (rd vms-6e6)

CLEAN-ROOM PROVENANCE (project Rule 8). Everything here is counted off bytes
real OpenVMS VAX V7.3 nodes put on a wire we captured, under tests/lab/captures.
It reports WHAT THE BYTES ARE and what they co-vary with. It does NOT say what
any bit MEANS: the layout is unpublished, and only a driven one-variable
capture -- one $ENQ option changed at a time, the method that grounded the
lock-mode byte (docs/cluster-protocol-spec.md 4(f).1, the ac4-* captures) --
may assign a bit to an option.

WHY IT EXISTS. OVMX's builder writes ZERO at body[28:30] and at body[31:44].
For most spans that is the right thing -- a real request leaves residue there
and rd vms-42de says this node sends zeros rather than its own kernel memory.
But a real request is zero at body[28:30] only 36 times in 21,496, and the
reason is that the span is not residue at all:

    the value is determined by the RESOURCE FAMILY

which a stale buffer cannot do. The same corpus's known residue cell takes 237
distinct values over 394 removals of ONE entry (rd vms-e09); here fourteen
different facilities each use essentially ONE value, several at 100%.

usage:
    dlm_request_flags_census.py [family|mode|spans] [<capture-or-dir>...]
    dlm_request_flags_census.py --selftest
"""
import collections
import os
import re
import struct
import sys

ETH_SCA = b"\x60\x07"
FRAME_LEN = 204
OFF_CAT, OFF_OP = 80, 81
OFF_CELL = 100           # body[28:30]
OFF_MODE = 102           # body[30]
OFF_NAME_LEN = 119
OFF_NAME = 120
REAL_OUI = (b"\x08\x00\x2b", b"\xaa\x00\x04")
HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, "..", "..", "tests", "lab", "captures")

# The spans OVMX's vms_dlm_enq_request_build() leaves zero, and what a real
# request has there. (abs offset, length, label)
ZEROED_SPANS = [
    (82, 2, "body[10:12]"),
    (88, 4, "body[16:20]"),
    (100, 2, "body[28:30]"),
    (103, 13, "body[31:44]"),
]


def pcap_frames(path):
    d = open(path, "rb").read()
    if len(d) < 24:
        return
    endian = "<" if d[0:4] in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1") else ">"
    i = 24
    while i + 16 <= len(d):
        cap = struct.unpack(endian + "IIII", d[i:i + 16])[2]
        yield None, d[i + 16:i + 16 + cap]
        i += 16 + cap


def jsonl_frames(path):
    import json
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
            yield r.get("port"), bytes.fromhex(r["hex"])
        except Exception:
            continue


def captures(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            for root, _d, fs in os.walk(a):
                out += [os.path.join(root, f) for f in sorted(fs)
                        if f.endswith((".pcap", ".jsonl"))]
        else:
            out.append(a)
    return sorted(out)


def sent_by_real_vms(port, frame):
    """OVMX's own frames are what this measures AGAINST, so they are excluded.
    A pcap names its sender by OUI; a demo capture names it by port, because a
    node hosted in a browser or under SIMH has whatever MAC it was given."""
    if port is not None:
        return not port.upper().startswith("OVMX")
    return frame[6:9] in REAL_OUI


def requests(paths):
    """Every cat-0x02 op-0x01 request a real VMS node sent, with its name."""
    for path in paths:
        gen = jsonl_frames if path.endswith(".jsonl") else pcap_frames
        for port, f in gen(path):
            if len(f) != FRAME_LEN or f[12:14] != ETH_SCA:
                continue
            if f[OFF_CAT] != 0x02 or f[OFF_OP] != 0x01:
                continue
            if not sent_by_real_vms(port, f):
                continue
            n = f[OFF_NAME_LEN]
            name = (f[OFF_NAME:OFF_NAME + n].decode("latin1")
                    if 1 <= n <= 31 else "")
            yield f, name


def family(name):
    """The lock's family, read off the name alone -- the prefix the owning VMS
    facility uses (F11B$a file, F11B$s volume, CACHE$cm, RMS$, LMF$, ...)."""
    m = re.match(r"^(F11B\$[a-z]|CACHE\$[a-z]{2}|RMS\$|RMSR|[A-Z0-9_$]{1,8})",
                 name)
    return m.group(1) if m else (name[:6] or "(unnamed)")


def by_family(paths):
    tab = collections.defaultdict(collections.Counter)
    for f, name in requests(paths):
        if name:
            tab[family(name)][struct.unpack("<H", f[OFF_CELL:OFF_CELL + 2])[0]] += 1
    return tab


def by_mode(paths):
    tab = collections.defaultdict(collections.Counter)
    for f, _name in requests(paths):
        tab[struct.unpack("<H", f[OFF_CELL:OFF_CELL + 2])[0]][f[OFF_MODE]] += 1
    return tab


def cmd_family(paths):
    tab = by_family(paths)
    print("body[28:30] by resource FAMILY (real senders, cat-0x02 op-0x01):")
    for fam in sorted(tab, key=lambda k: -sum(tab[k].values())):
        c = tab[fam]
        n = sum(c.values())
        if n < 10:
            continue
        top = c.most_common(3)
        print("  %-12s n=%-6d %2d distinct  %-34s %3.0f%% one value"
              % (fam, n, len(c),
                 " ".join("0x%04x x%d" % kv for kv in top),
                 100.0 * top[0][1] / n))
    return tab


def cmd_mode(paths):
    tab = by_mode(paths)
    print("requested MODE at body[30], per body[28:30] value:")
    for v, n in sorted(((v, sum(c.values())) for v, c in tab.items()),
                       key=lambda kv: -kv[1])[:10]:
        print("  0x%04x n=%-6d modes %s"
              % (v, n, " ".join("%d:x%d" % kv for kv in sorted(tab[v].items()))))
    return tab


def cmd_spans(paths):
    """How often each span OVMX zeroes is REALLY zero on a real request."""
    n = 0
    hit = collections.Counter()
    for f, _name in requests(paths):
        n += 1
        for off, ln, label in ZEROED_SPANS:
            if f[off:off + ln] == b"\0" * ln:
                hit[label] += 1
    print("%d real op-0x01 requests; how often a span OVMX zeroes is zero:" % n)
    for _off, _ln, label in ZEROED_SPANS:
        print("  %-14s %6d  (%5.2f%%)" % (label, hit[label],
                                          100.0 * hit[label] / n))
    return n, hit


def selftest():
    """Pin the table rd vms-6e6 reasons from. If the corpus grows these move,
    and the item's numbers must move with them."""
    paths = captures([CORPUS])
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print("  [%s] %s: %r%s" % ("PASS" if good else "FAIL", name, got,
                                   "" if good else " (want %r)" % (want,)))

    n, hit = cmd_spans(paths)
    print("-- body[28:30] and body[31:44] are NOT residue; the other two are")
    check("real op-0x01 requests", n, 21496)
    check("body[10:12] zero (residue, as OVMX assumes)", hit["body[10:12]"], 16586)
    check("body[16:20] zero (residue, as OVMX assumes)", hit["body[16:20]"], 17084)
    check("body[28:30] zero -- a real request almost never is",
          hit["body[28:30]"], 36)
    check("body[31:44] zero -- nor is this one", hit["body[31:44]"], 1649)

    fam = cmd_family(paths)
    print("-- and body[28:30] is decided by the resource FAMILY")
    for name, want_val, want_n in (("F11B$a", 0x007d, 4992),
                                   ("F11B$s", 0x0239, 7929),
                                   ("CACHE$cm", 0x0e70, 1605),
                                   ("RMS$", 0x0e19, 152),
                                   ("LMF$", 0x0011, 122)):
        check("%s -> 0x%04x" % (name, want_val), fam[name][want_val], want_n)

    mode = cmd_mode(paths)
    print("-- and it is NOT the requested mode: one value spans five of them")
    check("0x007d appears with 5 distinct modes", len(mode[0x007d]), 5)
    return 0 if ok else 1


def main(argv):
    if "--selftest" in argv:
        return selftest()
    cmd = argv[1] if len(argv) > 1 else "family"
    paths = captures(argv[2:] or [CORPUS])
    fn = {"family": cmd_family, "mode": cmd_mode, "spans": cmd_spans}.get(cmd)
    if fn is None:
        sys.exit(__doc__)
    fn(paths)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
