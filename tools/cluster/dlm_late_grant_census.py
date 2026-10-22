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

THE DESIGN DECISION WAS TAKEN (rd vms-a4d7, 2026-10-22: "follow the real
wire"), so this census now also carries THE FIELD MAP the builder is written
from, and the two independent falsifiers of the shape it replaces. All of it is
measured over the committed corpus; none of it is interpreted.

  THE POSITION, which is what makes it the deferred grant and not an analogy:
    111 real->real op-0x01 transactions answered 0xfb QUEUED
     95 of them are followed by a cat-0x02 op-0x02 from the MASTER to the
        REQUESTER naming that lock's own handle pair -- and 0 are PRECEDED by
        one, so the frame sits exactly where a deferred grant has to sit
     87 of those 95 are followed by the REQUESTER's own $DEQ or CONVERT on the
        same handles: it went on to USE a lock it had been told was queued
      1 of 111 reached that later use with no op-0x02 seen, and 15 are
        transactions whose capture ends before anything else happened

  THE TWO FALSIFIERS of the echo rd vms-f87 built (a second 0xfa ANSWER on an
  already-answered transaction), by independent routes:
      0 of 111 queued transactions were later answered 0xfa on that transaction
      0 of 3,974 real 0xfa grants whose request the capture does not hold name
        a handle pair this master had QUEUED -- and all 3,974 carry a live
        transaction token, so they are answers to a request off the capture,
        not unsolicited grants. The "a real master originates grants nobody
        asked for" reading in vms_dlm_scs.c's own header does not survive
        pairing by handle: not one of them is for a queued lock.

  THE FIELD MAP of the 95, cell by cell, with the figure beside each value the
  builder writes (`--selftest` pins every one; frame-grounding.tsv's
  dlm_grant_notify rows quote them):
      body[4:8]   = 0        NO TRANSACTION, 387/387: a NOTIFICATION
      body[8:10]  = 02 02
      body[12:14] = 0x0001   387/387, as on every DLM message
      body[20:24]            THE MASTER's handle; never 0 in 387/387
      body[24:28]            THE REQUESTER's handle; never 0 in 387/387
      body[28]    = 0x14     387/387
      body[29]    = 0        387/387
      body[30]                THE GRANTED MODE: equal to the queued request's
                              own mode in 95 of 95, and never NL in 387/387
      body[31]                a SECOND mode cell, equal to body[30] in 95 of 95
                              -- but not the same cell (41 of 267 differ in the
                              larger, not-cleanly-paired CONVERT population),
                              so the builder writes a second executive read
      body[112:116] = 0           387/387
      body[116:120] = ffffffff    387/387
      body[120:124] = 0           387/387
      body[124:128] = ffffffff    387/387
  Everything else is a real sender's STALE BUFFER and this node writes zero
  there (the rd vms-42de ruling), body[34] included: 0 is its modal real value
  (255 of 387) and the 0xfa/0xfb that appear there are not a status for this op
  -- in the 95 it reads 0x00 (61), 0x43, 0xfa, 0xfb, 0x01, 0x20, 0xf9, 0x72,
  0x9e, which no status would.

WHAT THIS CENSUS STILL DOES NOT CLAIM, and must not (Rule 8): what selects
body[34], what the 291 op-0x02 outside the queued-ENQ population are about, or
what makes the two mode cells differ when they do. Each is recorded as measured
and left alone.

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


def queued_enq_answers(frames):
    """Every real->real op-0x01 answer that said QUEUED, with its own body.

    Keyed exactly as the master addressed it: (master, requester, master
    handle, requester handle). That key is what a later frame about the same
    lock has to carry, which is how the deferred grant is found.
    """
    out = {}
    for idx, src, dst, body in frames:
        if body[8] == ANS_CAT and body[9] == ENQ_OP and \
           body[OFF_STATUS] == QUEUED and is_real(src) and is_real(dst):
            out[(src, dst, bytes(body[OFF_MASTER_H]),
                 bytes(body[OFF_REQ_H]))] = (idx, body)
    return out


def _by_key(frames, pred, flip=False):
    """{(master, requester, master_h, req_h): [(idx, body)..]} for pred frames.

    `flip` reads a REQUESTER-originated frame, whose (src, dst) is the reverse
    of the key the master's answer established.
    """
    out = collections.defaultdict(list)
    for idx, src, dst, body in frames:
        if not pred(body) or not is_real(src):
            continue
        a, b = (dst, src) if flip else (src, dst)
        out[(a, b, bytes(body[OFF_MASTER_H]),
             bytes(body[OFF_REQ_H]))].append((idx, body))
    return out


def deferred_grant_population(paths):
    """The measured deferred grant: what followed each QUEUED answer.

    Returns (n_queued, n_with_notify, n_before_notify, n_later_use,
             n_use_without_notify, pairs) where `pairs` is one
    (queued answer body, the FIRST op-0x02 after it) per queued answer that
    got one -- the population every field row below is counted over.
    """
    n_q = n_notify = n_before = n_use = n_use_only = 0
    pairs = []
    for p in paths:
        frames = bodies(p)
        notify = _by_key(frames, lambda b: b[8] == REQ_CAT and
                         b[9] == NOTIFY_OP)
        use = _by_key(frames, lambda b: b[8] == REQ_CAT and
                      b[9] in (0x03, 0x07), flip=True)
        for key, (qidx, qbody) in queued_enq_answers(frames).items():
            n_q += 1
            after = [x for x in notify.get(key, []) if x[0] > qidx]
            n_before += sum(1 for x in notify.get(key, []) if x[0] < qidx)
            later = [x for x in use.get(key, []) if x[0] > qidx]
            if after:
                n_notify += 1
                pairs.append((qbody, after[0][1]))
            if later:
                n_use += 1
                if not after:
                    n_use_only += 1
    return n_q, n_notify, n_before, n_use, n_use_only, pairs


def unsolicited_grants(paths):
    """The OTHER falsifier: 0xfa grants whose request the capture lacks.

    vms_dlm_scs.c's header read these as "grants nobody just asked for" and
    built an echo on that reading. Returns (total, no_request, no_token,
    naming_a_queued_pair).
    """
    tot = no_req = no_tok = named = 0
    for p in paths:
        frames = bodies(p)
        reqtok = {(src, dst, bytes(body[OFF_TXN]))
                  for _i, src, dst, body in frames
                  if body[8] == REQ_CAT and body[9] == ENQ_OP and is_real(src)}
        qk = set(queued_enq_answers(frames))
        for _idx, src, dst, body in frames:
            if not (body[8] == ANS_CAT and body[9] == ENQ_OP and
                    body[OFF_STATUS] == GRANTED and
                    is_real(src) and is_real(dst)):
                continue
            tot += 1
            if (dst, src, bytes(body[OFF_TXN])) in reqtok:
                continue
            no_req += 1
            if body[OFF_TXN] == b'\0' * 4:
                no_tok += 1
            if (src, dst, bytes(body[OFF_MASTER_H]),
                    bytes(body[OFF_REQ_H])) in qk:
                named += 1
    return tot, no_req, no_tok, named


# ---------------------------------------------------------------------------
# THE FIELD MAP. One entry per span the builder writes, with the predicate the
# real population has to satisfy for the value this node puts there. `pop` is
# 'all' (every real op-0x02: the SHAPE) or 'def' (the 95 deferred grants: the
# ROLE), because a cell grounded on the shape and a cell grounded on the role
# are different claims and the table must not blur them.
# ---------------------------------------------------------------------------
FIELD_MAP = (
    ('all', 'body[4:8] = 0        no transaction: a NOTIFICATION',
     lambda b, q: b[4:8] == b'\0' * 4),
    ('all', 'body[8] = 0x02       category, the request side',
     lambda b, q: b[8] == REQ_CAT),
    ('all', 'body[9] = 0x02       the opcode',
     lambda b, q: b[9] == NOTIFY_OP),
    ('all', 'body[12:14] = 0x0001 as on every DLM message',
     lambda b, q: b[12:14] == b'\x01\x00'),
    ('all', 'body[15] = 0         the DLM sequence is one byte wide here',
     lambda b, q: b[15] == 0),
    ('all', 'body[20:24] != 0     the MASTER\'s handle',
     lambda b, q: b[20:24] != b'\0' * 4),
    ('all', 'body[24:28] != 0     the REQUESTER\'s handle',
     lambda b, q: b[24:28] != b'\0' * 4),
    ('all', 'body[28] = 0x14',
     lambda b, q: b[28] == 0x14),
    ('all', 'body[29] = 0',
     lambda b, q: b[29] == 0),
    ('all', 'body[30] is a real lock mode, never NL',
     lambda b, q: 1 <= b[30] <= 5),
    ('all', 'body[112:116] = 0',
     lambda b, q: b[112:116] == b'\0' * 4),
    ('all', 'body[116:120] = ffffffff',
     lambda b, q: b[116:120] == b'\xff' * 4),
    ('all', 'body[120:124] = 0',
     lambda b, q: b[120:124] == b'\0' * 4),
    ('all', 'body[124:128] = ffffffff',
     lambda b, q: b[124:128] == b'\xff' * 4),
    ('def', 'body[30] == the queued request\'s own mode: THE GRANT',
     lambda b, q: b[30] == q[30]),
    ('def', 'body[31] == body[30] at a deferred grant',
     lambda b, q: b[31] == b[30]),
    ('def', 'body[47] = 0         no resource name: keyed by HANDLES',
     lambda b, q: b[OFF_NAME_LEN] == 0),
)

# The spans a real sender leaves as it found them, which this node writes as
# zero (rd vms-42de). The figure is how many of the population already read
# zero there -- never N of N, which is exactly why the row is stated.
RESIDUE_SPANS = ((10, 12), (16, 20), (32, 36), (36, 48), (48, 112),
                 (128, 132))


def field_map_counts(pairs, allbodies):
    """[(scope, label, hits, n)] over the two populations."""
    rows = []
    for scope, label, pred in FIELD_MAP:
        if scope == 'all':
            n = len(allbodies)
            hits = sum(1 for b in allbodies if pred(b, b))
        else:
            n = len(pairs)
            hits = sum(1 for q, b in pairs if pred(b, q))
        rows.append((scope, label, hits, n))
    return rows


def residue_counts(allbodies):
    return [((lo, hi), sum(1 for b in allbodies if b[lo:hi] == b'\0' * (hi - lo)),
             len(allbodies)) for lo, hi in RESIDUE_SPANS]


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

    nq, nn, nb, nu, nuo, pairs = deferred_grant_population(paths)
    print()
    print('THE POSITION -- what followed each of the %d QUEUED answers:' % nq)
    print('  an op-0x02 on that lock\'s own handle pair AFTER it:  %d' % nn)
    print('  ...and one BEFORE it:                                %d' % nb)
    print('  the requester\'s own DEQ/CONVERT on those handles:    %d' % nu)
    print('  ...with no op-0x02 seen first:                       %d' % nuo)

    tot, noreq, notok, qnamed = unsolicited_grants(paths)
    print()
    print('THE OTHER FALSIFIER -- real 0xfa grants: %d' % tot)
    print('  with no op-0x01 request in the same capture: %d' % noreq)
    print('  ...of those, carrying NO transaction token:  %d' % notok)
    print('  ...naming a pair this master had QUEUED:     %d' % qnamed)

    print()
    print('THE FIELD MAP (scope all = the %d, def = the %d deferred grants):'
          % (len(real), len(pairs)))
    for scope, label, hits, n in field_map_counts(pairs, real):
        print('  [%s] %-56s %d of %d' % (scope, label, hits, n))
    print('  residue spans this node writes as zero (rd vms-42de):')
    for (lo, hi), hits, n in residue_counts(real):
        print('      body[%d:%d] already zero in %d of %d' % (lo, hi, hits, n))


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

    # 3. THE POSITION. This is what makes op-0x02 the deferred grant rather
    #    than a shape that merely exists: it follows the queued answer for
    #    that lock, never precedes it, and the requester then uses the lock.
    nq, nn, nb, nu, nuo, pairs = deferred_grant_population(paths)
    check('queued answers (frames, not transactions)', nq, 111)
    check('*** ...followed by an op-0x02 on that handle pair ***', nn, 95)
    check('*** ...PRECEDED by one (a deferred grant cannot be) ***', nb, 0)
    check('...followed by the requester USING the lock', nu, 88)
    check('...using it with no op-0x02 seen first', nuo, 1)

    # 4. THE SECOND FALSIFIER, independent of the transaction pairing: the
    #    0xfa grants vms_dlm_scs.c's header read as unsolicited.
    tot, noreq, notok, qnamed = unsolicited_grants(paths)
    check('real->real 0xfa grants', tot, 53818)
    check('...with no request in the same capture', noreq, 3974)
    check('...of those, carrying no transaction token', notok, 0)
    check('*** ...naming a pair this master had QUEUED ***', qnamed, 0)

    # 5. THE FIELD MAP the builder is written from. Every row is N of N over
    #    its own population; a row that stops being N of N reds this gate,
    #    which is what pins the frame OVMX emits to the corpus.
    for scope, label, hits, n in field_map_counts(pairs, real):
        check('[%s] %s' % (scope, label), (hits, n), (n, n))

    # 6. THE RESIDUE SPANS, stated rather than hidden: this node writes zero
    #    where a real sender leaves its buffer, and none of these is N of N.
    for (lo, hi), hits, n in residue_counts(real):
        check('residue body[%d:%d] already zero' % (lo, hi), hits,
              {(10, 12): 237, (16, 20): 179, (32, 36): 116, (36, 48): 266,
               (48, 112): 16, (128, 132): 102}[(lo, hi)])

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
