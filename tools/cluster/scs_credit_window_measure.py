#!/usr/bin/env python3
"""scs_credit_window_measure.py -- re-derive the two E75 send-credit figures.

WHAT IT MEASURES (pure observation, no model). For every ordered pair of MAC
addresses in a capture, two independent counts:

  1. WHO RETURNS THE CREDIT. Walk the peer's frames in capture order and, every
     time its cumulative `recv_ack` (abs 32, LE u16) ADVANCES, attribute the
     messages that advance covers to the frame class that carried it -- the
     41-byte 0x48 credit-return short, or a `recv_ack` PIGGYBACKED on the
     peer's own sequenced message (0x41/0x4b/0x5b/0x7b).

     *VAXcluster Principles* p. 2-43 makes one message cost one credit and the
     acknowledgement return it; docs/cluster-protocol-spec.md sec 4(h)(3)
     grounds the 0x48 as returning "exactly one message's worth", and sec
     4(h)(4) grounds the piggybacked-ack lockstep. This count says which of the
     two channels the credit actually comes back on -- and it is overwhelmingly
     the piggyback, which is why a port that replenishes on the 0x48 alone
     starves (integration note E75).

  2. THE WINDOW CEILING. For each new sequenced message a node sends
     (retransmits of a seq already seen are excluded -- sec 4(h)(4a): a
     retransmit reuses its send_seq and is not a new message), how many of its
     messages were outstanding at that moment: `send_seq - last recv_ack seen
     from the peer`. The histogram's ceiling is the peer's grant, which sec
     4(g) grounds at abs 95 of the START/config body (byte-exact to SYSGEN
     CLUSTER_CREDITS). The script prints the grant each node advertised beside
     the ceiling so the two can be compared without a decoder ring.

CLEAN-ROOM (rule 8): every label here is either an on-wire observation or a
citation of docs/cluster-protocol-spec.md / the page-cited book. Nothing is
computed from an unpublished VMS algorithm.

Dependency-free (stdlib only). Classic libpcap files, LE or BE, us or ns.

Usage:
    tools/cluster/scs_credit_window_measure.py <pcap> [<pcap> ...]
"""
import collections
import struct
import sys

ETHERTYPE_SCA = b"\x60\x07"

OFF_MSGTYPE = 30        # abs 30, spec sec 4(g)
OFF_RECV_ACK = 32       # abs 32, spec sec 4(h)(4)
OFF_SEND_SEQ = 34       # abs 34, spec sec 4(h)(4)
OFF_START_CREDITS = 95  # abs 95, spec sec 4(g): SYSGEN CLUSTER_CREDITS

MT_START = 0x41
MT_CREDIT = 0x48
MT_SEQ = (0x41, 0x4b, 0x5b, 0x7b)


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


def ack_class(msgtype):
    """The channel an advancing acknowledgement rode in on, or None if this
    class carries no acknowledgement at all.

    Only the SCS classes have a `recv_ack` at abs 32. The directed discovery
    frames 0xb2/0xb3/0xb4 (sec 4(i).B) reuse those bytes for something else
    entirely, so reading one as an ack would manufacture credit out of a HELLO.
    """
    if msgtype == MT_CREDIT:
        return "0x48"
    if msgtype in MT_SEQ:
        return "piggyback"
    return None


def grants(frames):
    """What each node advertised at abs 95 of its 0x41 identity body."""
    out = collections.defaultdict(collections.Counter)
    for frame in frames:
        if len(frame) < OFF_START_CREDITS + 1:
            continue
        if frame[12:14] != ETHERTYPE_SCA or frame[OFF_MSGTYPE] != MT_START:
            continue
        out[mac(frame[6:12])][frame[OFF_START_CREDITS]] += 1
    return out


def measure(frames, a, b):
    """For the stream a->b: who returned the credit, and how deep it ran."""
    acked_by = collections.Counter()   # messages acknowledged, per class
    frames_by = collections.Counter()  # advancing frames, per class
    window = collections.Counter()     # outstanding-at-transmit histogram
    seen = set()                       # send_seq values already transmitted
    high = 0                           # peer's cumulative recv_ack high-water
    for frame in frames:
        if len(frame) < OFF_SEND_SEQ + 2 or frame[12:14] != ETHERTYPE_SCA:
            continue
        src, dst = mac(frame[6:12]), mac(frame[0:6])
        mt = frame[OFF_MSGTYPE]
        if mt == MT_START and {src, dst} == {a, b}:
            # sec 4(h)(4a): a 0x41 in EITHER direction re-forms the circuit and
            # resets its counters, so everything measured so far belongs to a
            # DIFFERENT circuit and carrying it across would invent a window.
            high = 0
            seen = set()
            continue
        if src == b and dst == a:
            cls = ack_class(mt)
            if cls is None:
                continue
            ack = le16(frame, OFF_RECV_ACK)
            if ack > high:
                acked_by[cls] += ack - high
                frames_by[cls] += 1
                high = ack
        elif src == a and dst == b and mt in MT_SEQ:
            seq = le16(frame, OFF_SEND_SEQ)
            if seq == 0 or seq in seen:
                continue          # 0 = no sequence; repeat = retransmit
            seen.add(seq)
            window[max(0, seq - high)] += 1
    return acked_by, frames_by, window


def pairs(frames):
    out = set()
    for frame in frames:
        if len(frame) >= 14 and frame[12:14] == ETHERTYPE_SCA:
            out.add((mac(frame[6:12]), mac(frame[0:6])))
    return out


def report(path):
    frames = list(read_pcap(path))
    advertised = grants(frames)
    print("== %s (%d frames)" % (path, len(frames)))
    for node in sorted(advertised):
        print("   %s advertises abs-95 grant %s" %
              (node, dict(advertised[node])))
    for a, b in sorted(pairs(frames)):
        acked_by, frames_by, window = measure(frames, a, b)
        total = sum(window.values())
        if total < 32:
            continue          # too little traffic to say anything
        acked = sum(acked_by.values())
        if acked > total:
            # More acknowledged than sent: this MAC pair is not one circuit.
            # A LAVC node answers from its hardware address on one leg and its
            # DECnet-logical address on another, so keying by (src, dst) splits
            # one node's stream in two and the ack half lands on the other key.
            # The figures cannot be attributed and are NOT reported (INV-6).
            print("   %s -> %s: SPLIT STREAM (%d acknowledged vs %d sent) "
                  "-- not one circuit under a (src, dst) key; skipped" %
                  (a, b, acked, total))
            continue
        print("   %s -> %s: %d new sequenced messages" % (a, b, total))
        for cls in sorted(acked_by):
            print("      acknowledged by %-10s %6d messages (%5.1f%%) "
                  "in %d frames" %
                  (cls, acked_by[cls],
                   100.0 * acked_by[cls] / acked if acked else 0.0,
                   frames_by[cls]))
        print("      outstanding-at-transmit ceiling: %d   histogram %s" %
              (max(window), dict(sorted(window.items()))))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for path in argv[1:]:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
