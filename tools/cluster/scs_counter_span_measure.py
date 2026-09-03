#!/usr/bin/env python3
"""scs_counter_span_measure.py -- census the SCS TRANSPORT COUNTER SPAN
(abs 36..55) carried by every sequenced frame in a set of captures.

WHY. `vms_cluster_codec_vc.h` asserts a rule for those ten 16-bit positions
and cites a figure for each. This script is where that figure comes from, so
the citation is re-derivable rather than remembered. Point it at the
reference captures and it prints the same table.

CLEAN-ROOM (CLAUDE.md rule 8). It only counts bytes observed on the wire and
compares them with the frame's own recv_ack/send_seq. It decodes nothing that
is not already grounded in docs/cluster-protocol-spec.md sec 4(d)/4(h)(4),
computes no VMS algorithm, and labels a position that does not resolve to a
constant or a mirror as VARIES rather than guessing a generator.

Usage:
    scs_counter_span_measure.py <pcap|dir> [...] [--by-length] [--exclude S]

    --exclude S   skip captures whose filename contains S. The figures
                  vms_cluster_codec_vc.h cites are the REFERENCE ones -- what
                  real VAX ports put on the wire -- so they are taken with
                  `--exclude ovmx`, which drops our own emissions from the
                  population being used to judge our own emissions.

Stdlib only.
"""
import collections
import os
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"
SCS_FORMAT_V13 = 0x13

# Absolute frame offsets, the same convention as docs/cluster-protocol-spec.md
# (0 == first byte of the Ethernet destination address).
OFF_MSGTYPE = 30
OFF_FORMAT = 31
OFF_RECV_ACK = 32
OFF_SEND_SEQ = 34
SPAN_START = 36
SPAN_END = 56          # exclusive

# The sequenced classes: 0x4b application, 0x5b connection-setup, 0x7b the
# retransmit form of either (spec sec 4(h)).
SEQ_MSGTYPES = (0x4B, 0x5B, 0x7B)

SPAN_NAMES = {
    36: "msg count",
    38: "NISCS_LAN_OVRHD",
    40: "ack mirror 1",
    42: "zero",
    44: "send_seq mirror",
    46: "zero",
    48: "ack mirror 2",
    50: "zero",
    52: "const 1",
    54: "const 2",
}


def read_pcap(path):
    """Classic libpcap reader (pcapng not supported). Yields frame bytes."""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 24:
        return
    magic = data[0:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        return
    off, n = 24, len(data)
    while off + 16 <= n:
        _s, _u, incl, _orig = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        yield data[off:off + incl]
        off += incl


def sequenced_frames(paths):
    """Every frame in `paths` that is an SCS sequenced message."""
    for path in paths:
        for fr in read_pcap(path):
            if len(fr) < SPAN_END:
                continue
            if fr[12:14] != ETHERTYPE_SCA or fr[OFF_FORMAT] != SCS_FORMAT_V13:
                continue
            if fr[OFF_MSGTYPE] not in SEQ_MSGTYPES:
                continue
            yield fr


def classify(fr, off):
    """Label one span position of one frame: 'ack', 'seq', both, or the
    literal value. A position whose value happens to equal BOTH counters is
    reported as 'ack=seq' rather than silently attributed to whichever the
    reader was hoping for."""
    val = struct.unpack_from("<H", fr, off)[0]
    ack = struct.unpack_from("<H", fr, OFF_RECV_ACK)[0]
    seq = struct.unpack_from("<H", fr, OFF_SEND_SEQ)[0]
    is_ack = bool(ack) and val == ack
    is_seq = bool(seq) and val == seq
    if is_ack and is_seq:
        return "ack=seq"
    if is_ack:
        return "ack"
    if is_seq:
        return "seq"
    return str(val)


def expand(paths, exclude):
    """Accept files or directories of .pcap."""
    out = []
    for p in paths:
        if os.path.isdir(p):
            out.extend(sorted(os.path.join(p, f) for f in os.listdir(p)
                              if f.endswith(".pcap")))
        else:
            out.append(p)
    if exclude:
        out = [p for p in out if exclude not in os.path.basename(p)]
    return out


def main(argv):
    by_length = "--by-length" in argv
    exclude = None
    args = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "--exclude" and i + 1 < len(argv):
            exclude = argv[i + 1]
            i += 2
            continue
        if not a.startswith("--"):
            args.append(a)
        i += 1
    if not args:
        print(__doc__)
        return 2

    per_off = collections.defaultdict(collections.Counter)
    per_len = collections.defaultdict(collections.Counter)
    total = 0
    for fr in sequenced_frames(expand(args, exclude)):
        total += 1
        key = tuple(classify(fr, o) for o in range(SPAN_START, SPAN_END, 2))
        per_len[len(fr)][key] += 1
        for i, o in enumerate(range(SPAN_START, SPAN_END, 2)):
            per_off[o][key[i]] += 1

    if total == 0:
        print("no sequenced SCS frames found")
        return 1

    print("sequenced SCS frames (msgtype 0x4b/0x5b/0x7b): %d\n" % total)
    print("%-5s  %-17s  %-14s  %s" % ("abs", "field", "dominant", "share"))
    for o in range(SPAN_START, SPAN_END, 2):
        top, n = per_off[o].most_common(1)[0]
        print("%-5d  %-17s  %-14s  %d/%d (%.1f%%)"
              % (o, SPAN_NAMES[o], top, n, total, 100.0 * n / total))
        rest = per_off[o].most_common()[1:4]
        if rest:
            print("%-5s  %-17s  also: %s" % ("", "",
                  ", ".join("%s x%d" % (v, c) for v, c in rest)))

    if by_length:
        print("\nby wire length:")
        for ln in sorted(per_len):
            top, n = per_len[ln].most_common(1)[0]
            print("  len=%-5d n=%-7d %s" % (ln, sum(per_len[ln].values()), top))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
