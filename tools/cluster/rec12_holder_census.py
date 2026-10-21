#!/usr/bin/env python3
"""rec12_holder_census.py -- what a HOLDER's op-0x12 record carries (rd vms-33a).

usage: rec12_holder_census.py <pcap|dir> ...        the per-cell census
       rec12_holder_census.py --selftest <repo>     pin the headline figures

WHY THIS EXISTS. remaster_census.py measures what the NEW MASTER's answer
rewrites -- the half OVMX already builds (rd vms-380 rule 2). rd vms-33a is the
other half: OVMX as a HOLDER must SEND op-0x12 records when a real VAX
remasters a tree it holds locks in, or the real new master waits forever for an
op-0x13 that never comes (rule W2, measured over 61 episodes). A builder for
that record needs to know what a real holder puts in every cell, split ROOT vs
SUB, and which cells are the same for every record of one episode -- which is
what this prints.

It reports, per body offset: `const` with the value when every record agrees,
otherwise the number of distinct values and the commonest few. It makes no
claim about what a cell MEANS: naming a cell is the codec header's job, and
Rule 8 forbids inferring an unpublished algorithm from the values. The census
is the evidence; the header is where a cell gets a name, and only with a
capture or an SDA page behind it.

AND ONE CAUTION THAT MATTERS MORE THAN THE NUMBERS: "constant across every
captured record" IS NOT "constant in the protocol". body[44:46] is zero in all
420 records because every captured holder used a LCK$M_SYSTEM name -- UIC group
0 -- so a builder that wrote a zero there would be writing the corpus's
accident, not the resource's identity. A cell belongs in a builder as a
CONSTANT only when something other than this census says so.

CLEAN-ROOM: reads captured frames only. No VSI/HPE source, no binaries, no
recomputation of an unpublished algorithm.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

BODY_LEN = 132


def records(paths):
    """(request, answer) for every op-0x12 that a real answer paired with."""
    pairs, _notifs = rc.collect(paths)
    return [(q, a) for (s, o, q, a) in pairs if s == 0x12 and o == 0x01]


def split_root_sub(recs):
    root = [(q, a) for (q, a) in recs if rc.is_root_record(q)]
    sub = [(q, a) for (q, a) in recs if not rc.is_root_record(q)]
    return root, sub


def cell_report(group, label):
    print("\n=== op-0x12 REQUEST cells, %s  n=%d" % (label, len(group)))
    for off in range(BODY_LEN):
        vals = collections.Counter(q[off] for (q, _a) in group)
        if len(vals) == 1:
            k = next(iter(vals))
            print("  body[%3d] const %02x  (%d/%d)" % (off, k, vals[k], len(group)))
        else:
            top = ", ".join("%02x:%d" % (v, n) for v, n in vals.most_common(6))
            print("  body[%3d] %2d distinct  %s" % (off, len(vals), top))


def report(paths):
    recs = records(paths)
    root, sub = split_root_sub(recs)
    print("op-0x12 records paired with a real 82/01 answer: %d "
          "(ROOT %d, SUB %d)" % (len(recs), len(root), len(sub)))
    cell_report(root, "ROOT")
    cell_report(sub, "SUB")
    return 0


def selftest(repo):
    """Pin the figures rd vms-33a's own planning rests on. If the corpus grows
    these move, and whatever quotes them must move with them."""
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    recs = records(paths)
    root, sub = split_root_sub(recs)
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))

    check('records paired with an answer', len(recs), 420)
    check('ROOT records', len(root), 9)
    check('SUB records', len(sub), 411)

    # The cells every real record agrees on, root and sub alike -- the ones a
    # builder may write as constants, and the only ones.
    agree = []
    for off in range(BODY_LEN):
        vals = {q[off] for (q, _a) in recs}
        if len(vals) == 1:
            agree.append((off, next(iter(vals))))
    # MEASURED, not expected: the list below is what the corpus says, and the
    # selftest exists so a corpus change has to move it deliberately.
    #
    # READ IT WITH THE CAUTION IN THE HEADER: "constant across every captured
    # record" is NOT "constant in the protocol". body[44:46] is 0 in all 420
    # because every captured holder used a LCK$M_SYSTEM name (UIC group 0) --
    # a builder must still write the RSB's own group there, not a zero. The
    # same goes for body[63:66] (0x20, the name padding of names shorter than
    # the span) and body[38]/[42] (the high byte of a lock id, which the
    # minted-form census already describes as 0 in real ids).
    check('cells constant across ALL 420 records',
          [off for off, _v in agree],
          [5, 8, 9, 12, 13, 15, 17, 18, 19, 26, 33, 34, 35, 38, 42, 44, 45,
           63, 64, 65, 71, 86, 87, 106, 107, 108, 109, 110, 112, 113, 114,
           115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127])
    check('and their values',
          [v for _off, v in agree],
          [0x00, 0x02, 0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x20, 0x20, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff,
           0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff])

    # body[36:44] is the PARENT PAIR, and it is what tells the two apart.
    check('every ROOT record has an all-zero body[36:44]',
          all(q[36:44] == b'\0' * 8 for (q, _a) in root), True)
    check('every SUB record has a NON-zero body[36:44]',
          all(q[36:44] != b'\0' * 8 for (q, _a) in sub), True)
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    return report(rc.walk_inputs(argv[1:]))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
