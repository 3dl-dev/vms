#!/usr/bin/env python3
"""score_grantnotify.py <pcap> <ovmx-mac-tail> <repo> -- fire `a4d1`'s scoring read.

Scores the pre-registered predictions of rd vms-a4d7 / rd vms-28bd over the
fire's own capture. Everything is a COUNT, so "the frame went out" and "the echo
did not" are both measurements and not an absence of looking. Uses the shipping
census walker (tools/cluster/remaster_census.py) for the frame/body split -- no
private offsets.

  P1  OVMXE ORIGINATES a cat-0x02 op-0x02, and it does so AFTER the queued
      answer for that lock, never before.
  P2  its cells are the ones the executive holds: body[20:24] / body[24:28] are
      byte-for-byte the pair of the 0xfb queued answer OVMXE sent, and
      body[30] == body[31] == the mode the request asked for.
  P6  no SECOND cat-0x82 op-0x01 on that transaction, and no OVMXE-originated
      0xfa naming that handle pair -- the shape the corpus has no instance of.
  P7  body[4:8] == 0 on every op-0x02 OVMXE sends, and ZERO cat-0x82 op-0x02
      appears from anybody.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.join(sys.argv[3], "tools", "cluster"))
import remaster_census as rc   # noqa: E402

QUEUED, GRANTED = 0xfb, 0xfa


def is_ovmx(mac, tail):
    return mac.hex().lower().endswith(tail)


def frames(pcap):
    return [(i, s, d, bytes(b)) for i, s, d, b in rc.sca_bodies_with_src(pcap)
            if len(b) >= 132]


def name_of(body):
    n = body[47]
    if n == 0 or n > 31:
        return ""
    return bytes(body[48:48 + n]).decode("latin1", "replace").rstrip()


def main():
    pcap, tail = sys.argv[1], sys.argv[2].lower()
    fr = frames(pcap)

    # The queued answers THIS node sent: the key every later frame about that
    # lock has to carry, and the mode it was queued at.
    queued = {}
    for i, s, d, b in fr:
        if b[8] == 0x82 and b[9] == 0x01 and b[34] == QUEUED and is_ovmx(s, tail):
            queued[(d, bytes(b[20:24]), bytes(b[24:28]))] = (i, b[30], name_of(b))
    print("QUEUED ANSWERS OVMXE ORIGINATED: %d" % len(queued))
    for (d, mh, rh), (i, mode, nm) in sorted(queued.items(), key=lambda x: x[1][0]):
        print("  f%-6d -> %s  our handle %s  its handle %s  mode %d  res=%r"
              % (i, d.hex(), mh.hex(), rh.hex(), mode, nm))

    # P1 / P2.
    print()
    n_notify = 0
    matched = 0
    for i, s, d, b in fr:
        if b[8] != 0x02 or b[9] != 0x02 or not is_ovmx(s, tail):
            continue
        n_notify += 1
        key = (d, bytes(b[20:24]), bytes(b[24:28]))
        q = queued.get(key)
        verdict = []
        if q is None:
            verdict.append("NO QUEUED ANSWER ON THIS KEY")
        else:
            qi, qmode, _nm = q
            verdict.append("after f%d" % qi if i > qi else "*** BEFORE f%d ***" % qi)
            verdict.append("mode %d==%d" % (b[30], qmode)
                           if b[30] == qmode else
                           "*** mode %d != queued %d ***" % (b[30], qmode))
            verdict.append("b31==b30" if b[31] == b[30] else
                           "*** b31=%d != b30=%d ***" % (b[31], b[30]))
            if i > qi and b[30] == qmode and b[31] == b[30]:
                matched += 1
        verdict.append("txn=0" if b[4:8] == b"\0" * 4 else "*** TXN OPEN ***")
        verdict.append("namelen=%d" % b[47])
        verdict.append("b28:30=%s" % b[28:30].hex())
        verdict.append("tail=%s/%s" % (b[116:120].hex(), b[124:128].hex()))
        print("op-0x02 f%-6d -> %s  our %s  its %s  %s"
              % (i, d.hex(), b[20:24].hex(), b[24:28].hex(), "  ".join(verdict)))
    print("P1/P2: %d op-0x02 ORIGINATED by OVMXE, %d of them correct on every "
          "registered cell" % (n_notify, matched))

    # P6: the echo must not exist.
    print()
    echo = 0
    for i, s, d, b in fr:
        if b[8] == 0x82 and b[9] == 0x01 and b[34] == GRANTED and is_ovmx(s, tail):
            if (d, bytes(b[20:24]), bytes(b[24:28])) in queued:
                echo += 1
                print("*** P6 FAIL: f%d is a 0xfa naming a pair this node had "
                      "QUEUED ***" % i)
    print("P6: %d OVMXE-originated 0xfa grants naming a queued handle pair "
          "(want 0)" % echo)

    # P7: nobody answers one.
    ans02 = sum(1 for _i, _s, _d, b in fr if b[8] == 0x82 and b[9] == 0x02)
    print("P7: %d cat-0x82 op-0x02 answers from anybody (want 0)" % ans02)

    # Context: what OVMXE put on the wire at all, by (cat, op).
    print()
    c = collections.Counter((b[8], b[9]) for _i, s, _d, b in fr if is_ovmx(s, tail))
    print("EVERY DLM/CM frame OVMXE ORIGINATED, by (cat, op):")
    for (cat, op), n in sorted(c.items()):
        print("  %02x/%02x  %d" % (cat, op, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
