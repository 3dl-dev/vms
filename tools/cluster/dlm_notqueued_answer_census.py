#!/usr/bin/env python3
"""dlm_notqueued_answer_census.py -- what a REAL master answers to a cat-0x02
lock request that asked LCK$M_NOQUEUE and could not be granted, and which
request bit says NOQUEUE (rd vms-1e58 C4).

usage: dlm_notqueued_answer_census.py <pcap|dir> ...     the census
       dlm_notqueued_answer_census.py --selftest <repo>  pin the figures

WHY THIS EXISTS. On the ci.6 lab fire (tests/lab/captures/
vms-ci6-evac-20261103, frames 7310/7312) OVMX mastered EVAC$WORKLOAD and held
it EX; a real VAX asked for EX with LCK$M_NOQUEUE and OVMX answered QUEUED, so
the VAX's $ENQW completed SS$_NORMAL inside OVMX's hold. OVMX's master never
read the request's options at all. Two things had to be grounded before it
could answer honestly, and this census grounds both from the wire alone:

  1. WHICH REQUEST BIT IS NOQUEUE. body[28:30] of a request is the sender's
     option set (rd vms-6e6). Pair every real answer with its request: every
     DENY (body[34] = 0xfc) answers a request with bit 0x0004 SET, and every
     QUEUED (0xfb) answers one with it CLEAR -- op 0x01 and op 0x07 alike, no
     exception either way. (The driven pair EVACWL/EVPROBE, which differ only
     in FLAGS=#LCK$M_NOQUEUE, put 00 00 and 04 00 there: frames 5212/7310.)
  2. WHAT A DENY LOOKS LIKE. Every real deny carries b8 09 fc at body[32:35]
     (0x09b8 = 2488 = SS$_NOTQUEUED, 0xfc the outcome). The ROOT denies echo
     the request -- its PID-form placeholder and its own handle at
     body[20:28], its identity and its whole name -- with body[28:32] cleared
     on 2 of the 3 (the third carries S0-looking residue there). Every
     SUB-lock deny rewrites body[40:44] with an 0x87.. S0 address, which this
     executive does not hold, so it refuses to compose one.

PAIRING is by transaction (body[4:8] and the (src,dst) pair), through
dlm_queued_answer_census.py's own pairing, so a pair counted here is a pair
counted there. REAL -> REAL only.

CLEAN-ROOM: reads captured frames only. It reports what the bytes are and what
co-varies with what; the meaning of the other option bits is NOT assigned.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dlm_queued_answer_census as qc  # noqa: E402
import remaster_census as rc           # noqa: E402

GRANTED, QUEUED, NOTQUEUED = 0xfa, 0xfb, 0xfc
NOQUEUE_BIT = 0x0004
RECORD = bytes((0xb8, 0x09, 0xfc))


def flags(req):
    return req[28] | (req[29] << 8)


def pairs(paths, op, status):
    """REAL -> REAL (request, answer) pairs for opcode `op` and outcome."""
    saved = qc.ENQ_OP
    qc.ENQ_OP = op
    try:
        return qc.real_pairs(paths, status)
    finally:
        qc.ENQ_OP = saved


def partition(paths):
    """{(op, status): Counter(noqueue_bit_set -> n)}"""
    out = {}
    for op in (0x01, 0x07):
        for st in (GRANTED, QUEUED, NOTQUEUED):
            out[(op, st)] = collections.Counter(
                bool(flags(r) & NOQUEUE_BIT) for _p, r, _a, _s, _d in
                pairs(paths, op, st))
    return out


def report(paths):
    part = partition(paths)
    print("=== real answers by request opcode, outcome and request bit 0x0004")
    for (op, st), c in sorted(part.items()):
        print("    op %02x  body[34]=%02x  bit set %6d  clear %6d"
              % (op, st, c[True], c[False]))
    d = pairs(paths, 0x01, NOTQUEUED)
    root, sub = qc.split_root_sub(d)
    qc.print_rewrite_report(root, "NOTQUEUED answer, ROOT op-0x01 request")
    qc.print_topology(root, "NOTQUEUED answer, ROOT op-0x01 request")
    qc.print_rewrite_report(sub, "NOTQUEUED answer, SUB-LOCK op-0x01 request")


def expect(ok, what):
    print("  %s %s" % ("ok  " if ok else "FAIL", what))
    return 0 if ok else 1


def selftest(repo):
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    bad = 0
    part = partition(paths)
    # 1. The bit. No deny without it, no queue with it -- both opcodes.
    for op in (0x01, 0x07):
        dn, qu = part[(op, NOTQUEUED)], part[(op, QUEUED)]
        bad += expect(dn[False] == 0 and dn[True] > 0,
                      "op %02x: every real DENY answers a request with bit "
                      "0x0004 set (%d set, %d clear)" % (op, dn[True],
                                                         dn[False]))
        bad += expect(qu[True] == 0 and qu[False] > 0,
                      "op %02x: every real QUEUED answers a request with it "
                      "clear (%d clear, %d set)" % (op, qu[False], qu[True]))
    bad += expect(part[(0x01, NOTQUEUED)][True] == 58,
                  "58 real op-0x01 deny pairs")
    bad += expect(part[(0x07, NOTQUEUED)][True] == 120,
                  "120 real op-0x07 deny pairs")
    bad += expect(part[(0x01, QUEUED)][False] == 111,
                  "111 real op-0x01 queued pairs (dlm_queued_answer_census)")

    # 2. The deny's shape.
    d = pairs(paths, 0x01, NOTQUEUED)
    root, sub = qc.split_root_sub(d)
    bad += expect(len(root) == 3 and len(sub) == 55,
                  "3 ROOT and 55 SUB-lock op-0x01 denies (got %d/%d)"
                  % (len(root), len(sub)))
    bad += expect(all(a[32:35] == RECORD for _p, _r, a, _s, _d in d),
                  "every one carries b8 09 fc at body[32:35] -- 0x09b8 is "
                  "SS$_NOTQUEUED (2488)")
    bad += expect(all(a[8] == 0x82 and a[9] == 0x01
                      for _p, _r, a, _s, _d in d),
                  "every one is a cat-0x82 op-0x01")
    bad += expect(sum(a[28:32] == b'\0' * 4 and a[35] == 0
                      for _p, _r, a, _s, _d in root) == 2,
                  "2 of the 3 ROOT denies clear body[28:32] and carry 0 at "
                  "body[35]")
    bad += expect(all(a[20:28] == r[20:28] for _p, r, a, _s, _d in root),
                  "every ROOT deny echoes body[20:28] -- the PID placeholder "
                  "and the requester's own handle")
    bad += expect(all(a[44:124] == r[44:124] for _p, r, a, _s, _d in root),
                  "every ROOT deny echoes the identity and the WHOLE name "
                  "(no body[52:54] zeroing, unlike a QUEUED answer)")
    bad += expect(len(set(x[4] for x in root)) == 2 and
                  len(set(x[3] for x in root)) == 2,
                  "the ROOT denies come from 2 masters to 2 requesters")
    bad += expect(all(a[43] == 0x87 for _p, _r, a, _s, _d in sub),
                  "every SUB-lock deny writes an 0x87.. S0 address at "
                  "body[40:44] -- which is why this node refuses to compose one")

    # 3. The driven pair on the ci.6 fire, from the committed transcript.
    tx = os.path.join(repo, 'tests', 'lab', 'captures',
                      'vms-ci6-evac-20261103', 'ci6evac-031902',
                      'ci6-frames.txt')
    fr = transcript_frames(tx)
    # payload offset 0x56 = body[28] (body[0] is payload 0x3a)
    bad += expect(fr.get(5212, b'')[0x56:0x58] == b'\0\0',
                  "EVACWL's $ENQW EX (FLAGS 0, frame 5212) carries 00 00")
    bad += expect(fr.get(7310, b'')[0x56:0x58] == b'\x04\0',
                  "EVPROBE's $ENQW EX NOQUEUE (frame 7310) carries 04 00")
    print("\n%s" % ("selftest: all figures hold" if bad == 0
                    else "selftest: %d FAILED" % bad))
    return 1 if bad else 0


def transcript_frames(path):
    """{frame number: Ethernet payload bytes} from a ci6-frames.txt dump."""
    out, cur = {}, None
    with open(path) as fh:
        for line in fh:
            if line.startswith('frame '):
                cur = int(line.split()[1])
                out[cur] = bytearray()
            elif cur is not None and line.startswith('  '):
                out[cur].extend(bytes.fromhex(''.join(line.split()[1:])))
    return {k: bytes(v) for k, v in out.items()}


def main(argv):
    if len(argv) >= 3 and argv[1] == '--selftest':
        return selftest(argv[2])
    if len(argv) < 2:
        print(__doc__)
        return 2
    report(rc.walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
