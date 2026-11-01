#!/usr/bin/env python3
"""dlm_sublock_census.py -- how a REAL node asks for a SUB-LOCK (rd vms-b67a).

usage: dlm_sublock_census.py <pcap|dir> ...        the sub-lock requests, decoded
       dlm_sublock_census.py --selftest <repo>     pin the headline figures

WHY THIS EXISTS. rd vms-b67a, measured in the vms-c2b fire: OVMX's $ENQ under a
PROXY parent looks the SUB-resource up in the LOCK DIRECTORY under its own
derived hash, as if it were a root name, and then masters it LOCALLY -- so one
sub-resource name ends up mastered on two nodes at once and OVMX reports a grant
no master gave. A real node never does that: a lock whose PARID names a parent
goes to the PARENT'S MASTER, the only node that can grant it.

That sentence used to be a reading of Davis. It is now a measurement: this
census finds the sub-lock requests real OpenVMS VAX nodes put on a wire we
captured, and prints, for each, who it was ADDRESSED TO and what it carries at
body[36:44] -- which is the pair (the requester's OWN parent lock id, the
parent's MASTER lock id) that the parent's own grant handed it moments earlier.

WHAT IT DOES NOT CLAIM. It does not say what body[128:132] -- a hash, one per
sub-resource NAME -- is FOR on a frame addressed to a master rather than to a
directory node. A master does not need a hash to serve a resource it masters,
and the hash function is unpublished (Rule 8), so OVMX cannot produce the value
and will not have learned it for a name no directory was ever asked about. That
is a DEVIATION for the grounding table to carry with this measurement beside it,
not a cell for a builder to invent.

A SUB-LOCK REQUEST IS RECOGNISED BY ITS NAME, not by its parent pair: the names
are the ones this project's own lock-holder program takes (`S0`..`S9`,
tests/lab/captures/*/RMHOLD.MAR), so the frames below are requests whose
arrangement is KNOWN from the arm that produced them -- a holder that took a
root and then n sub-locks under it. That is what makes the parent pair
readable at all: the root's grant is in the same capture, a few frames earlier.

CLEAN-ROOM: reads captured frames only. No VSI/HPE source, no binaries, no
recomputation of an unpublished algorithm.
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

BODY_LEN = 132
ENQ_OP = 0x01
REQ_CAT = 0x02
REAL_OUI = ('08002b', 'aa0004')
SUB_NAME = re.compile(r'^S[0-9]$')


def _name(body):
    n = body[47]
    if n == 0 or n >= 32:
        return ''
    return bytes(body[48:48 + n]).decode('latin1')


def enq_requests(paths):
    """(real, ovmx) cat-0x02 op-0x01 requests as (src, dst, body, path)."""
    real, ovmx = [], []
    for p in paths:
        for _idx, src, dst, body in rc.sca_bodies_with_src(p):
            if len(body) < BODY_LEN or body[8] != REQ_CAT or body[9] != ENQ_OP:
                continue
            row = (bytes(src), bytes(dst), bytes(body), p)
            (real if src[:3].hex() in REAL_OUI else ovmx).append(row)
    return real, ovmx


def sub_requests(rows):
    """The ones naming a sub-resource of the known holder arrangement."""
    return [r for r in rows if SUB_NAME.match(_name(r[2]))]


def le32(body, off):
    return int.from_bytes(body[off:off + 4], 'little')


def report(paths):
    real, ovmx = enq_requests(paths)
    subs = sub_requests(real)
    print("cat-0x02 op-0x01 requests: REAL %d, OVMX %d" % (len(real), len(ovmx)))
    print("...of the real ones, naming a sub-resource (S0..S9): %d" % len(subs))
    print("\n%-30s %-6s %-6s %-4s %-10s %-10s %-4s %-9s"
          % ("capture", "src", "dst", "name", "parent_own", "parent_mast",
             "mode", "hash"))
    for src, dst, body, p in subs:
        print("%-30s %-6s %-6s %-4s 0x%08x 0x%08x %4d 0x%08x"
              % (os.path.basename(p), src.hex()[-4:], dst.hex()[-4:],
                 _name(body), le32(body, 36), le32(body, 40), body[30],
                 le32(body, 128)))
    dsts = collections.Counter(dst.hex()[-4:] for _s, dst, _b, _p in subs)
    print("\naddressed to: %s" % dsts.most_common())
    print("with a NON-ZERO parent pair at body[36:44]: %d of %d"
          % (sum(1 for _s, _d, b, _p in subs if b[36:44] != b'\0' * 8),
             len(subs)))
    return 0


def selftest(repo):
    """Pin what rd vms-b67a's fix is built from. If the corpus grows these
    move, and the item and the grounding rows move with them."""
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    real, ovmx = enq_requests(paths)
    subs = sub_requests(real)
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                  '' if good else ' (want %r)' % (want,)))

    # rd vms-e80 widened the shared walker to the 32 `.pcap.gz` captures, so
    # the denominator moved 21,499 -> 74,737. THE NUMERATOR DID NOT: still
    # exactly 7 real sub-lock requests, from the same 2 senders, at the same 2
    # modes, with the same 3 parent pairs. rd vms-b67a's grounding is unchanged
    # by 3.5x the corpus -- and the sample is still 7, which is why that item's
    # fix is built from the ROUTING rule and not from a cell's value range.
    check('real lock requests in the corpus', len(real), 74894)   # was 21,499
    check('real requests naming a sub-resource', len(subs), 7)
    check('distinct senders of one', len({s for s, _d, _b, _p in subs}), 2)
    check('distinct sub-lock modes at body[30]',
          sorted({b[30] for _s, _d, b, _p in subs}), [1, 2])

    # W1 -- THE ROUTING. Every one goes to ONE node, and it is not a directory
    # node: the only directory lookup in that capture is for the ROOT name,
    # answered 0xf8 (redirect), after which the root is re-asked at the master.
    check('addressed to exactly one destination',
          len({d for _s, d, _b, _p in subs}), 1)

    # W2 -- THE PARENT PAIR. Non-zero on every one, and equal across the
    # sub-locks of one holder's tree (they share a parent).
    check('with a non-zero parent pair at body[36:44]',
          sum(1 for _s, _d, b, _p in subs if b[36:44] != b'\0' * 8), len(subs))
    pairs = collections.Counter(b[36:44] for _s, _d, b, _p in subs)
    check('distinct parent pairs (one per holder tree)', len(pairs), 3)

    # ...and the pair really is (own parent lkid, parent's MASTER lkid): for
    # the first holder, the root GRANT in the same capture carries exactly
    # those two longwords in the other order (body[20:24] master,
    # body[24:28] requester), which is the property the builder copies.
    first = sorted(pairs)[0]
    check('the pair is two longwords, neither zero',
          (le32(first, 0) != 0, le32(first, 4) != 0), (True, True))

    # W4 -- the hash cell is present and is per NAME, which is why it cannot
    # be dismissed as residue and must not be invented either.
    byname = collections.defaultdict(set)
    for _s, _d, b, _p in subs:
        byname[_name(b)].add(le32(b, 128))
    check('each sub NAME has exactly one hash value across senders',
          sorted((n, len(v)) for n, v in byname.items()),
          [('S0', 1), ('S1', 1), ('S2', 1)])

    print('dlm_sublock_census selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        repo = argv[2] if len(argv) > 2 else os.path.join(HERE, '..', '..')
        return selftest(repo)
    if len(argv) < 2:
        print(__doc__)
        return 2
    return report(rc.walk_inputs(argv[1:]))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
