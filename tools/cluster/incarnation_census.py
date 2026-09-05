#!/usr/bin/env python3
"""incarnation_census.py -- re-derive the spec sec 4(h)(4c) grounding for abs 36.

WHAT IT MEASURES (pure observation, no model). For every ordered pair of MAC
addresses in a capture it reports two things:

  1. the value at abs 36 (LE u16), split by frame class -- 0x41
     START/STACK/ACK, the sequenced classes 0x4b/0x5b/0x7b, and the 0x48
     credit-return;
  2. the value at abs 92 (payload [78:80]) of that sender's DIRECTED discovery
     frames (0xb2/0xb3/0xb4), which sec 4(i).B grounds as the node-incarnation
     the sender ADVERTISES for the frame's destination.

Two facts fall straight out and neither is inferred:

  * a real node carries ONE value at abs 36 across all three classes of a
    circuit (it changes only when the circuit re-forms), and
  * that value is what its PEER advertised to it at abs 92 -- so the pair is
    asymmetric whenever the two ends have re-formed a different number of
    times.

Any pair whose 0x41 set and sequenced set are disjoint is flagged MISMATCH:
that is the E66 defect shape (a port stamping the real echo on its handshake
and a constant everywhere else), and no reference node produces it.

CLEAN-ROOM (rule 8): every label here is either an on-wire observation or a
citation of docs/cluster-protocol-spec.md sec 4(i).B / 4(h)(4c). Nothing is
computed from an unpublished VMS algorithm.

Dependency-free (stdlib only). Classic libpcap files, LE or BE, us or ns.

Usage:
    tools/cluster/incarnation_census.py <pcap> [<pcap> ...]
"""
import collections
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"

OFF_MSGTYPE = 30        # abs 30, spec sec 4(g)
OFF_INCARNATION = 36    # abs 36, spec sec 4(h)(4c)
OFF_HELLO_ADVERT = 92   # abs 92 == payload [78:80], spec sec 4(i).B

MT_START = 0x41
MT_CREDIT = 0x48
MT_SEQ = (0x4b, 0x5b, 0x7b)
MT_DIRECTED_DISC = (0xb2, 0xb3, 0xb4)


def read_pcap(path):
    """Yield raw Ethernet frames from a classic libpcap file."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 24:
        raise ValueError("%s: too short to be a pcap file" % path)
    magic = data[0:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        raise ValueError("%s: unrecognized pcap magic %r" % (path, magic))
    off, n = 24, len(data)
    while off + 16 <= n:
        _s, _u, incl, _orig = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        yield data[off:off + incl]
        off += incl


def mac(raw):
    return ":".join("%02x" % b for b in raw)


def le16(frame, off):
    return struct.unpack("<H", frame[off:off + 2])[0]


def is_sca(frame, need):
    return len(frame) >= need and frame[12:14] == ETHERTYPE_SCA


def classify(msgtype):
    """The frame class abs 36 is being reported for, or None to skip."""
    if msgtype == MT_START:
        return "0x41"
    if msgtype in MT_SEQ:
        return "seq"
    if msgtype == MT_CREDIT:
        return "0x48"
    return None


def census(path):
    """(stamped, advertised) keyed by (src, dst)."""
    stamped = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    advertised = collections.defaultdict(collections.Counter)
    for frame in read_pcap(path):
        if not is_sca(frame, OFF_MSGTYPE + 1):
            continue
        pair = (mac(frame[6:12]), mac(frame[0:6]))
        mt = frame[OFF_MSGTYPE]
        if mt in MT_DIRECTED_DISC:
            if len(frame) >= OFF_HELLO_ADVERT + 2:
                advertised[pair][le16(frame, OFF_HELLO_ADVERT)] += 1
            continue
        cls = classify(mt)
        if cls is None or len(frame) < OFF_INCARNATION + 2:
            continue
        stamped[pair][cls][le16(frame, OFF_INCARNATION)] += 1
    return stamped, advertised


def one_class_note(per_class):
    """MISMATCH iff this sender split abs 36 between its handshake and its
    sequenced traffic -- the E66 defect shape."""
    s41 = set(per_class["0x41"])
    sseq = set(per_class["seq"])
    if s41 and sseq and not (s41 & sseq):
        return "   <<< MISMATCH: 0x41 and sequenced disagree (E66 shape)"
    return ""


def echo_note(pair, per_class, advertised):
    """Does abs 36 equal what the PEER advertised to this sender (4(i).B)?"""
    reverse = advertised.get((pair[1], pair[0]))
    if not reverse:
        return ""
    seen = set()
    for counter in per_class.values():
        seen |= set(counter)
    if not seen:
        return ""
    want = set(reverse)
    if seen == want:
        verdict = "MATCH"
    elif seen & want:
        verdict = "PARTIAL"       # some classes echo it, some do not
    else:
        verdict = "DIFFERS"
    return "  echo=%s (stamped %s, peer advertised %s)" % (
        verdict, sorted(seen), sorted(want))


def report(path):
    stamped, advertised = census(path)
    print("== %s" % path.split("/")[-1])
    for pair in sorted(stamped):
        per_class = stamped[pair]
        print("   %s -> %s%s" % (pair[0], pair[1], one_class_note(per_class)))
        print("      abs36  0x41 %s  seq %s  0x48 %s%s" % (
            dict(per_class["0x41"]), dict(per_class["seq"]),
            dict(per_class["0x48"]), echo_note(pair, per_class, advertised)))
    for pair in sorted(advertised):
        print("   %s -> %s  advertises abs92 %s"
              % (pair[0], pair[1], dict(advertised[pair].most_common(6))))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
