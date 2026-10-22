#!/usr/bin/env python3
"""dlm_late_grant_census.py -- HOW A REAL MASTER DELIVERS A GRANT IT COULD NOT
GIVE AT ONCE (rd vms-28bd, and the shape question rd vms-f87's design left
open).

usage: dlm_late_grant_census.py <pcap|dir> ...     the census
       dlm_late_grant_census.py --selftest <repo>  pin the headline figures

WHY THIS EXISTS, MEASURED. The rd vms-5cd fix fire (2026-10-22,
tests/lab/captures/vms-5cd-fixfire-20261022) read the door an owed deferred
grant was refused at: `send_failed=1`, with the connection manager's own leg
reporting `refused=0`. The code read that followed names the gate exactly --
cnxman_dlm_peer_proven() admits a frame toward a system that has not proved it
runs this implementation only when vms_dlm_shape_fit_for_any_member() clears its
SHAPE, and that function's first test is `cat != VMS_DLM_CAT_REQUEST` -> 0. The
deferred grant OVMX builds is a cat-0x82 ANSWER, so it is refused by
construction.

THE OBVIOUS "FIX" IS THE WRONG ONE, AND THIS CENSUS IS WHY. Widening the gate
would put on the wire, toward a real V7.3 lock manager, a SECOND answer on a
transaction that has already been answered -- and over the whole committed
corpus that is a thing no real master does:

  111 real->real op-0x01 transactions were answered 0xfb QUEUED.
  ZERO of them were later answered 0xfa GRANTED on the same transaction.

So the frame rd vms-f87 built -- the requester's own queued request echoed back
with the granted mode -- has no instance in the corpus at all, and emitting one
is the minted-shape class that has bugchecked lab VAXes twice.

WHAT A REAL MASTER DOES INSTEAD, as far as the wire shows: the commonest thing
that reaches the requester about that lock afterwards is a **cat-0x02 op-0x02**
message FROM the master, an opcode this codec has no name for. 387 of them are
in the corpus, every one from a real sender, and this census pins what they are:
notifications (no transaction), carrying no resource name, keyed by lock
HANDLES, with a status cell at body[34] whose values include 0xfa -- and 96 of
the 387 name the handles of a request the same pair had queued.

WHAT THIS CENSUS DOES NOT CLAIM, and must not (Rule 8). It does not say that
cat-0x02 op-0x02 IS the deferred grant, nor what selects body[34]'s value, nor
what the other 291 are about. It measures that the shape OVMX wants to send
does not exist on a real cluster and that this other shape is what occupies the
moment, which is the evidence a DESIGN decision needs -- not a substitute for
one.

PAIRING is by transaction token and the (src,dst) pair, the same rule
remaster_census.py and dlm_queued_answer_census.py use, and every frame is
classified by remaster_census's own SCS-MSG discriminators.

CLEAN-ROOM: reads captured frames only.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

BODY_LEN = 132
REQ_CAT, ANS_CAT, ENQ_OP = 0x02, 0x82, 0x01
NOTIFY_OP = 0x02                  # the opcode this codec has no name for
QUEUED, GRANTED = 0xfb, 0xfa
REAL_OUI = ('08002b', 'aa0004')   # same split as dlm_deq_census.py

OFF_TXN = slice(4, 8)
OFF_MASTER_H = slice(20, 24)
OFF_REQ_H = slice(24, 28)
OFF_STATUS = 34
OFF_NAME_LEN = 47


def is_real(mac):
    return mac[:3].hex() in REAL_OUI


def bodies(path):
    """Every SCS-MSG body in one capture, long enough to read, in order."""
    return [(idx, src, dst, bytes(body))
            for idx, src, dst, body in rc.sca_bodies_with_src(path)
            if len(body) >= BODY_LEN]


def transactions(frames):
    """(src, dst, token) -> the ordered list of what happened on it.

    Each entry is ('req' | 'ans<xx>', frame index). Keyed from the REQUESTER's
    direction, so a request and its answer land on one key.
    """
    hist = collections.defaultdict(list)
    for idx, src, dst, body in frames:
        if body[9] != ENQ_OP:
            continue
        tok = bytes(body[OFF_TXN])
        if tok == b'\0' * 4:
            continue
        if body[8] == REQ_CAT:
            hist[(src, dst, tok)].append(('req', idx))
        elif body[8] == ANS_CAT:
            hist[(dst, src, tok)].append(('ans%02x' % body[OFF_STATUS], idx))
    return hist


def real_transaction_outcomes(paths):
    """Counter over REAL->REAL op-0x01 transactions: what the master did."""
    out = collections.Counter()
    for p in paths:
        hist = transactions(bodies(p))
        for (rsrc, asrc, _tok), evs in hist.items():
            if not (is_real(rsrc) and is_real(asrc)):
                continue
            kinds = [k for k, _i in evs]
            has_q = 'ans%02x' % QUEUED in kinds
            has_g = 'ans%02x' % GRANTED in kinds
            if has_q and has_g:
                out['queued THEN granted on one transaction'] += 1
            elif has_q:
                out['queued, never granted on that transaction'] += 1
            elif has_g:
                out['granted at once'] += 1
            else:
                out['neither (lookup answers, unanswered, retransmits)'] += 1
    return out


def queued_keys(frames):
    """{(master, requester, master_handle, requester_handle): frame index}."""
    out = {}
    for idx, src, dst, body in frames:
        if body[8] == ANS_CAT and body[9] == ENQ_OP and \
           body[OFF_STATUS] == QUEUED and is_real(src) and is_real(dst):
            out[(src, dst, bytes(body[OFF_MASTER_H]),
                 bytes(body[OFF_REQ_H]))] = idx
    return out


def notify_frames(paths):
    """Every cat-0x02 op-0x02 body, with whether it names a queued request's
    handles on the same (sender -> receiver) pair."""
    real, ovmx = [], []
    named = 0
    for p in paths:
        frames = bodies(p)
        qk = queued_keys(frames)
        for _idx, src, dst, body in frames:
            if body[8] != REQ_CAT or body[9] != NOTIFY_OP:
                continue
            if not is_real(src):
                ovmx.append(body)
                continue
            real.append(body)
            for (qsrc, qdst, mh, rh) in qk:
                if qsrc == src and qdst == dst and (
                        mh in (body[OFF_MASTER_H], body[OFF_REQ_H]) or
                        rh in (body[OFF_MASTER_H], body[OFF_REQ_H])):
                    named += 1
                    break
    return real, ovmx, named


def report(paths):
    out = real_transaction_outcomes(paths)
    print('REAL->REAL op-0x01 transactions, by what the master did:')
    for k, v in out.most_common():
        print('  %-52s %d' % (k, v))
    real, ovmx, named = notify_frames(paths)
    print()
    print('cat-0x02 op-0x02 (the opcode this codec has no name for):')
    print('  real frames: %d   OVMX-emitted: %d' % (len(real), len(ovmx)))
    print('  opening a transaction at body[4:8]: %d'
          % sum(1 for b in real if b[OFF_TXN] != b'\0' * 4))
    print('  carrying no resource name (body[47] == 0): %d'
          % sum(1 for b in real if b[OFF_NAME_LEN] == 0))
    print('  body[34] values: %s'
          % dict(collections.Counter(b[OFF_STATUS] for b in real).most_common(8)))
    print('  naming a QUEUED request\'s handles on the same pair: %d' % named)


def selftest(repo):
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))

    out = real_transaction_outcomes(paths)

    # 1. THE SHAPE rd vms-f87 BUILT HAS NO REAL INSTANCE. This is the figure
    #    the item turns on: a real master never answers a transaction twice to
    #    flip QUEUED into GRANTED.
    check('real->real transactions answered 0xfb QUEUED',
          out['queued, never granted on that transaction'] +
          out['queued THEN granted on one transaction'], 111)
    check('*** ...of those, later answered 0xfa on the SAME transaction ***',
          out['queued THEN granted on one transaction'], 0)
    check('granted at once (the contrast)', out['granted at once'], 53799)

    # 2. WHAT OCCUPIES THAT MOMENT INSTEAD, measured and not interpreted.
    real, ovmx, named = notify_frames(paths)
    check('real cat-0x02 op-0x02 frames', len(real), 387)
    check('...emitted by an OVMX node, ever', len(ovmx), 0)
    check('...opening a transaction at body[4:8]',
          sum(1 for b in real if b[OFF_TXN] != b'\0' * 4), 0)
    check('...carrying no resource name (body[47] == 0)',
          sum(1 for b in real if b[OFF_NAME_LEN] == 0), 383)
    check('...with body[34] == 0xfa', sum(1 for b in real
                                          if b[OFF_STATUS] == GRANTED), 31)
    check('...with body[34] == 0xfb', sum(1 for b in real
                                          if b[OFF_STATUS] == QUEUED), 12)
    check('...naming a QUEUED request\'s handles on the same pair', named, 96)

    print('\ndlm_late_grant_census selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        repo = argv[2] if len(argv) > 2 else os.path.join(HERE, '..', '..')
        return selftest(repo)
    if len(argv) < 2:
        print(__doc__)
        return 2
    report(rc.walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
