#!/usr/bin/env python3
"""dlm_deq_census.py -- what a REAL cat-0x02 op-0x03 RELEASE carries, and where
OVMX's own release differs from it (rd vms-3ac done-condition (d)).

usage: dlm_deq_census.py <pcap|dir> ...        the per-cell census + the diff
       dlm_deq_census.py --selftest <repo>     pin the headline figures

WHY THIS EXISTS, AND WHY IT IS THIS FRAME. The cat-0x02 op-0x03 release is the
frame that bugchecked a real OpenVMS VAX V7.3 node with INVLOCKID on
2026-10-21 (tests/lab/captures/vms-3ac-invlockid-20261021: R1 = 370004A7, the
lock id OVMX named in it). The handle is fixed -- the adoption now converts this
node's own locks out of proxy state, and dlm_proxy_post refuses a post about a
resource this node masters. But the frame itself had never been grounded CELL BY
CELL the way the lock REQUEST was (rd vms-c55, the `dlm_enq_req` rows in
tests/cluster/conformance/frame-grounding.tsv), and the minted-form rule of this
lane says every value OVMX puts on a real wire needs a form taken from a census
of what real nodes put there -- two lab VAXes have already been bugchecked by a
value that was merely plausible.

WHAT IT REPORTS. Per body offset, over every cat-0x02 op-0x03 frame in the
corpus, split by SENDER: REAL (a DEC/Digital OUI, i.e. an OpenVMS VAX) and OVMX
(anything else). `const` with the value when every frame of that group agrees,
otherwise the distinct count and the commonest few. Then the DIFF: the cells
where the two groups disagree, which is the list a builder has to answer for.

IT MAKES NO CLAIM ABOUT WHAT A CELL MEANS. Naming a cell is the codec header's
job and needs a capture or an SDA page behind it; Rule 8 forbids inferring an
unpublished algorithm from values. In particular this census does NOT say what
body[30] is. It says what values real senders put there, which is the only
thing that bounds what OVMX may put there.

AND THE CAUTION THAT MATTERS MORE THAN THE NUMBERS (the same one
rec12_holder_census.py carries): "constant across every captured frame" is NOT
"constant in the protocol". A cell belongs in a builder as a constant only when
something other than this census says so.

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
DEQ_OP = 0x03
DEQ_CAT = 0x02
ANSWER_CAT = 0x82

# The OUIs a real DEC/Digital interface sends from. Everything else in this
# corpus is an OVMX node (QEMU's 52:54:00 and the lab's own assignments), which
# is exactly the split this census is about.
REAL_OUI = ('08002b', 'aa0004')



def deq_frames(paths):
    """(real, ovmx) lists of op-0x03 bodies, by sender OUI.

    Reuses remaster_census.sca_bodies, so a frame counted here passed the same
    SCS-MSG discriminators the codec's own classifier applies -- the census and
    the executive agree on what a frame IS before they disagree about a byte.
    """
    real, ovmx = [], []
    for p in paths:
        for _idx, src, _dst, body in rc.sca_bodies_with_src(p):
            if len(body) < BODY_LEN:
                continue
            if body[8] != DEQ_CAT or body[9] != DEQ_OP:
                continue
            (real if src[:3].hex() in REAL_OUI else ovmx).append(bytes(body))
    return real, ovmx


def deq_answers(paths):
    """Every cat-0x82 op-0x03 in the corpus -- i.e. every ANSWER to a release.

    The count is the evidence for the envelope: a message nobody ever answers
    opens no transaction.
    """
    out = []
    for p in paths:
        for _idx, _src, _dst, body in rc.sca_bodies_with_src(p):
            if len(body) >= BODY_LEN and body[8] == ANSWER_CAT and \
               body[9] == DEQ_OP:
                out.append(bytes(body))
    return out


def cell_report(group, label):
    print("\n=== cat-0x02 op-0x03 cells, %s sender  n=%d" % (label, len(group)))
    if not group:
        return
    for off in range(BODY_LEN):
        vals = collections.Counter(b[off] for b in group)
        if len(vals) == 1:
            k = next(iter(vals))
            print("  body[%3d] (abs %3d) const %02x  (%d/%d)"
                  % (off, 72 + off, k, vals[k], len(group)))
        else:
            top = ", ".join("%02x:%d" % (v, n) for v, n in vals.most_common(6))
            print("  body[%3d] (abs %3d) %3d distinct  %s"
                  % (off, 72 + off, len(vals), top))


def disagreements(real, ovmx):
    """Cells where the value sets of the two senders are DISJOINT -- i.e. OVMX
    writes something no real sender was ever observed writing there."""
    out = []
    for off in range(BODY_LEN):
        rv = {b[off] for b in real}
        ov = {b[off] for b in ovmx}
        if rv and ov and not (rv & ov):
            out.append((off, sorted(rv)[:6], sorted(ov)))
    return out


def report(paths):
    real, ovmx = deq_frames(paths)
    answers = deq_answers(paths)
    print("cat-0x02 op-0x03 releases: REAL %d, OVMX %d" % (len(real), len(ovmx)))
    print("cat-0x82 op-0x03 ANSWERS anywhere in the corpus: %d" % len(answers))
    cell_report(real, "REAL")
    cell_report(ovmx, "OVMX")
    print("\n=== cells where OVMX's value was never seen from a real sender")
    for off, rv, ov in disagreements(real, ovmx):
        print("  body[%3d] (abs %3d) real %s  OVMX %s"
              % (off, 72 + off, ["%02x" % v for v in rv],
                 ["%02x" % v for v in ov]))
    return 0


def selftest(repo):
    """Pin the figures rd vms-3ac's (d) and the `dlm_deq` grounding rows rest
    on. If the corpus grows these move, and whatever quotes them moves too."""
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    real, ovmx = deq_frames(paths)
    answers = deq_answers(paths)
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                  '' if good else ' (want %r)' % (want,)))

    check('real releases in the corpus', len(real), 9858)
    check('OVMX releases in the corpus', len(ovmx), 11)

    # 1. THE ENVELOPE. A release is a NOTIFICATION: no real sender opens a
    #    transaction on one, and nobody answers one.
    check('real releases carrying a transaction at body[4:8]',
          sum(1 for b in real if b[4:8] != b'\0' * 4), 0)
    check('answers to a release, anywhere in the corpus', len(answers), 0)
    check('OVMX releases carrying a transaction at body[4:8]',
          sum(1 for b in ovmx if b[4:8] != b'\0' * 4), len(ovmx))

    # 2. body[30], AND ITS ENVELOPE. No claim about what it is; the measured
    #    range is the whole point, because OVMX writes a LOCK MODE there and a
    #    lock mode runs 0..5.
    check('real body[30] value set', sorted({b[30] for b in real}), [0, 1])
    check('OVMX body[30] value set', sorted({b[30] for b in ovmx}), [5])

    # 3. body[32]. Equal to body[30] on every real release; zero on every
    #    OVMX one, because the builder does not write it.
    check('real releases with body[32] == body[30]',
          sum(1 for b in real if b[32] == b[30]), len(real))
    check('OVMX releases with body[32] == 0',
          sum(1 for b in ovmx if b[32] == 0), len(ovmx))

    # 4. The cells both senders already agree on -- the cheap half of the row
    #    set, and the half a builder must not get wrong.
    check('body[8:10] is 02 03 on every real release',
          sum(1 for b in real if b[8] == 0x02 and b[9] == 0x03), len(real))
    check('body[12:14] is 0x0001 on every real release',
          sum(1 for b in real if b[12] == 0x01 and b[13] == 0x00), len(real))

    # 5. The spans a real sender leaves as its own stale buffer, counted so a
    #    `const:0` row for them is a measured choice and not an assumption.
    check('real releases with an all-zero body[16:20]',
          sum(1 for b in real if b[16:20] == b'\0' * 4), 5700)
    check('real releases with an all-zero body[36:44]',
          sum(1 for b in real if b[36:44] == b'\0' * 8), 354)
    check('real releases with ffffffff at body[124:128]',
          sum(1 for b in real if b[124:128] == b'\xff' * 4), 5117)
    check('real releases with an all-zero body[124:128]',
          sum(1 for b in real if b[124:128] == b'\0' * 4), 4095)

    print('dlm_deq_census selftest: %s' % ('PASS' if ok else 'FAIL'))
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
