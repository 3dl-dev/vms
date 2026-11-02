#!/usr/bin/env python3
"""dlm_rebuild08_census.py -- what a real cat-0x02 op-0x08 record carries (rd vms-874f).

usage: dlm_rebuild08_census.py <pcap|dir> ...        the census
       dlm_rebuild08_census.py --selftest <repo>     pin the headline figures

WHY THIS EXISTS. The codec named op-0x08 VMS_DLM_WIREOP_DIR_LOOKUP_TR off two
samples (rd vms-629): a node's own SYS$SYS_ID root, sent to its directory inside
a formation barrier and answered like an op-0x01 lookup. rd vms-874f's VAX-only
3-node departure (tests/lab/captures/vms-874f-failover-rebuild-20261011) shows
that is one case of a wider message: when a lock tree's MASTER LEAVES, every
SURVIVING HOLDER re-sends each lock it holds in that tree as an op-0x08 record
-- the ROOT first, then each SUB carrying the parent's handle on the NEW master.
OVMX sends none, so a survivor's locks in a departed master's tree die with it.
A builder for that record needs every cell grounded, ROOT vs SUB, and the rules
for who sends, to whom, in what order, and what the answer looks like. This is
that evidence.

WHAT IT MEASURES, over every committed capture that holds a cat-0x02 op-0x08:
  * each record paired with its cat-0x82 answer BY THE SCS TRANSACTION at
    body[4:8] (never by time) -- remaster_census.py's rule;
  * ROOT vs SUB: a SUB record has a non-zero body[36:44], the PARENT PAIR (the
    sender's own parent lkid at [36:40], the parent's lkid on the new master at
    [40:44]) -- the same pair rd vms-b67a measured in the op-0x01 sub request
    and rd vms-33a in the op-0x12 record;
  * the parent of a SUB, resolved inside the capture as the sender's own earlier
    record whose body[24:28] is the SUB's body[36:40], and whether the SUB's
    body[40:44] is that parent's ANSWER body[20:24];
  * the answer's status at body[34]: 0xf9 (the requester masters the tree) or
    0xfa (a master already exists, and it granted);
  * the hash at body[128:132] against tools/cluster/dlm_hash/refhash.py, which
    is pinned to the product's C function by check_heldout_run.py.

It makes no claim about what a cell MEANS beyond what those pairings show.
Naming a cell is the codec header's job and needs a capture or an SDA page
behind it; Rule 8 forbids inferring an unpublished algorithm from the values.
Where a cell agrees in every record but the corpus cannot say WHY (body[32:36],
body[28:30]), the census says so and does not name it.

Clean-room: reads only our own lab captures. No VMS binary or source.
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'dlm_hash'))
import remaster_census as rc  # noqa: E402
import refhash  # noqa: E402

OP_REBUILD = 0x08
CAT_REQ, CAT_ANS = 0x02, 0x82
BODY_LEN = 132
REAL_OUI = ('08002b', 'aa0004')       # a DEC/Digital interface (dlm_deq_census)
ST_YOU_MASTER, ST_GRANTED = 0xf9, 0xfa

RA1B = os.path.join('vms-874f-failover-rebuild-20261011', 'ra1b-departure.pcap.gz')
# tests/lab/captures/vms-380-remaster-arms-20261011/RA1b-truth.txt -- SDA on
# VAX3, before the departure. Own lkid -> (lock, mode).
RA1B_VAX3_TRUTH = {0x0E00049B: ('RMT1', 1), 0x33000498: ('S0', 2),
                   0x010004A5: ('S1', 2), 0x010004A6: ('S2', 2)}
RA1B_VAX2_RMT1 = 0x10000387            # VAX2's own RMT1 lkid, PR


def le32(b, off):
    return struct.unpack_from('<I', b, off)[0]


def is_real(mac6):
    return mac6[:3].hex() in REAL_OUI


def ident(body):
    n = body[47]
    if not 1 <= n <= 31:
        return None
    return (int.from_bytes(body[44:46], 'little'), body[46], bytes(body[48:48 + n]))


def is_root(body):
    return body[36:44] == b'\0' * 8


def capture_records(path):
    """Every op-0x08 record in one capture, as a dict, answer attached."""
    rows = list(rc.sca_bodies_with_src(path))
    answers = {}
    for idx, src, dst, body in rows:
        if body[8] == CAT_ANS and body[4:8] != b'\0\0\0\0':
            answers.setdefault((bytes(dst), bytes(src), bytes(body[4:8])),
                               (idx, bytes(body)))
    out = []
    for idx, src, dst, body in rows:
        if body[8] != CAT_REQ or body[9] != OP_REBUILD:
            continue
        a = answers.get((bytes(src), bytes(dst), bytes(body[4:8])))
        # an answer must FOLLOW its request; a reused token can precede it
        if a is not None and a[0] < idx:
            a = None
        out.append(dict(idx=idx, src=bytes(src), dst=bytes(dst), q=bytes(body),
                        a_idx=a[0] if a else None, a=a[1] if a else None))
    return out


def resolve_parents(recs):
    """Attach `parent` (the sender's own earlier record whose body[24:28] is this
    SUB's body[36:40]) to every SUB record of one capture."""
    for r in recs:
        if is_root(r['q']):
            continue
        want = r['q'][36:40]
        cand = [p for p in recs if p['src'] == r['src'] and p['idx'] < r['idx']
                and p['q'][24:28] == want]
        r['parent'] = cand[-1] if cand else None


def collect(paths):
    """{capture-relpath: [records]} for every capture holding an op-0x08."""
    out = {}
    for p in paths:
        recs = capture_records(p)
        if recs:
            resolve_parents(recs)
            out[p] = recs
    return out


def measure(caps):
    """The figures the selftest pins, as one dict of counters."""
    m = collections.Counter()
    status = collections.Counter()
    for _p, recs in caps.items():
        for r in recs:
            q, a = r['q'], r['a']
            root = is_root(q)
            k = 'root' if root else 'sub'
            m[k] += 1
            m['real_sender'] += is_real(r['src'])
            m['answered'] += a is not None
            idn = ident(q)
            m['ident_ok'] += idn is not None
            if idn is not None and refhash.name_hash(idn[2], idn[1], idn[0]) == le32(q, 128):
                m['hash_ok'] += 1
            m['modes_equal'] += q[30] == q[31]
            if a is None:
                continue
            real_ans = is_real(r['dst'])
            m['answer_by_real'] += real_ans
            m['answer_op01'] += a[9] == 0x01
            m['answer_echo_24'] += a[24:28] == q[24:28]
            if real_ans:
                status[(k, a[34])] += 1
                if a[34] == ST_GRANTED:
                    m[k + '_granted_real'] += 1
                    m[k + '_granted_b28_10'] += a[28] == 0x10
                    m[k + '_granted_b20_new'] += a[20:24] != q[20:24]
                if a[34] == ST_YOU_MASTER:
                    m[k + '_youmaster_real'] += 1
                    m[k + '_youmaster_echo_20'] += a[20:24] == q[20:24]
            if not root:
                par = r.get('parent')
                if par is None:
                    m['sub_no_parent'] += 1
                    continue
                m['sub_parent_found'] += 1
                m['sub_parent_is_root'] += is_root(par['q'])
                if par['a'] is not None:
                    m['sub_40_is_parent_answer_20'] += q[40:44] == par['a'][20:24]
                    m['sub_parent_answer_first'] += par['a_idx'] < r['idx']
                    m['sub_parent_answer_granted'] += par['a'][34] == ST_GRANTED
                m['sub_same_dst_as_parent'] += r['dst'] == par['dst']
    return m, status


def report(paths):
    caps = collect(paths)
    for p, recs in caps.items():
        nodes = sorted({r['src'].hex() for r in recs} | {r['dst'].hex() for r in recs})
        print('%-70s records=%4d root=%4d sub=%4d  nodes=%s'
              % (os.path.relpath(p), len(recs),
                 sum(1 for r in recs if is_root(r['q'])),
                 sum(1 for r in recs if not is_root(r['q'])), ','.join(nodes)))
    m, status = measure(caps)
    print()
    for k in sorted(m):
        print('  %-32s %d' % (k, m[k]))
    print('  answer status (real answers):',
          {'%s/%02x' % k: v for k, v in sorted(status.items())})
    for label, sel in (('ROOT', True), ('SUB', False)):
        grp = [r['q'] for recs in caps.values() for r in recs
               if is_root(r['q']) == sel and is_real(r['src'])]
        print('\n=== op-0x08 REQUEST cells, %s  n=%d' % (label, len(grp)))
        for off in range(BODY_LEN):
            vals = collections.Counter(q[off] for q in grp)
            if len(vals) == 1:
                v = next(iter(vals))
                print('  body[%3d] const %02x' % (off, v))
            else:
                top = ', '.join('%02x:%d' % kv for kv in vals.most_common(5))
                print('  body[%3d] %3d distinct  %s' % (off, len(vals), top))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    return report(rc.walk_inputs(argv[1:]))


def roles(recs):
    """ROOT-record roles in one capture (rd vms-874f).

    `dir_sender`: a ROOT record answered 0xfa by a node that had, EARLIER in
    the capture, been answered 0xf9 for the same identity BY THIS SENDER --
    i.e. the sender is the tree's directory node and is addressing the master
    it has just installed. Returns a Counter.
    """
    t = collections.Counter()
    won = {}
    for r in recs:
        q, a = r['q'], r['a']
        if not is_root(q) or a is None or not is_real(r['dst']):
            continue
        k = ident(q)
        if a[34] == ST_YOU_MASTER:
            won[k] = (r['dst'], r['src'], r['idx'])
            t['f9 parity%d %s' % ((le32(q, 128) >> 16) & 1, r['dst'].hex())] += 1
        elif a[34] == ST_GRANTED:
            w = won.get(k)
            if w and w[0] == r['src'] and w[1] == r['dst'] and w[2] < r['idx']:
                t['dir_sender'] += 1
            else:
                t['dir_answer'] += 1
    return t


def interleaved_0d(path, recs):
    """op-0x0d frames lying between the first and last op-0x08 of a capture."""
    lo, hi = min(r['idx'] for r in recs), max(r['idx'] for r in recs)
    return sum(1 for idx, _s, _d, b in rc.sca_bodies(path)
               if b[8] == CAT_REQ and b[9] == 0x0d and lo < idx < hi)


def barrier_steps(path, recs):
    """rd vms-bcb: for each op-0x08 record, the step of the LAST barrier release
    (cat-0x01 op-0x0c, its step index at body[16]) before it, and whether its
    answer arrived before the NEXT release. Returns (Counter of last-release
    step, Counter of 'closed' verdicts)."""
    rel = [(idx, b[16]) for idx, _s, _d, b in rc.sca_bodies(path)
           if b[8] == 0x01 and b[9] == 0x0c and len(b) > 16]
    steps, closed = collections.Counter(), collections.Counter()
    for r in recs:
        before = [st for idx, st in rel if idx < r['idx']]
        steps[before[-1] if before else None] += 1
        after = [idx for idx, _st in rel if idx > r['idx']]
        if not before:
            closed['no barrier in the capture'] += 1
        elif r['a_idx'] is not None and after and r['a_idx'] < after[0]:
            closed['answered before the next release'] += 1
        else:
            closed['other'] += 1
    return steps, closed


ZERO_PAIR = b'\0' * 20


def holder_pairs(path, rec_op):
    """rd vms-8159: WHERE a holder's record's value block and sequence
    (body[88:108]) came from. For every cat-0x02 `rec_op` record (0x08 or
    0x12) the holder's LAST wire event for that lock (sender, own lkid at
    body[24:28]) before it is found:

      grant  a cat-0x82 op-0x01/0x07 answer with body[34] == 0xfa TO the holder
             naming the lkid -- not the answer to a 0x08/0x12 record itself;
             the pair is its body[36:56];
      1a     a cat-0x02 op-0x1a TO the holder naming the lkid; the pair is
             its body[36:56];
      write  a cat-0x02 op-0x06 FROM the holder naming the lkid (its own
             value-block write); the bytes are its body[36:52]. No real
             op-0x06 is answered, so no frame carries the sequence it makes.

    Returns a Counter keyed (record kind, last event, verdict)."""
    rows = list(rc.sca_bodies_with_src(path))
    recs = {}
    for idx, src, dst, b in rows:
        if b[8] == CAT_REQ and b[9] in (0x08, 0x12) and b[4:8] != b'\0\0\0\0':
            recs[(bytes(src), bytes(dst), bytes(b[4:8]))] = idx
    rec_answers = set()
    for idx, src, dst, b in rows:
        if b[8] == CAT_ANS:
            q = recs.get((bytes(dst), bytes(src), bytes(b[4:8])))
            if q is not None and q < idx:
                rec_answers.add(idx)
    last = {}
    out = collections.Counter()
    for idx, src, dst, b in rows:
        src, dst, lk = bytes(src), bytes(dst), bytes(b[24:28])
        if (b[8] == CAT_ANS and b[9] in (0x01, 0x07) and b[34] == ST_GRANTED
                and idx not in rec_answers):
            last[(dst, lk)] = ('grant', bytes(b[36:56]))
        elif b[8] == CAT_REQ and b[9] == 0x1a:
            last[(dst, lk)] = ('1a', bytes(b[36:56]))
        elif b[8] == CAT_REQ and b[9] == 0x06:
            last[(src, lk)] = ('write', bytes(b[36:52]))
        elif b[8] == CAT_REQ and b[9] == rec_op and (
                rec_op == OP_REBUILD or b[4:8] != b'\0\0\0\0'):
            kind = 'ROOT' if is_root(b) else 'SUB'
            rec = bytes(b[88:108])
            ev = last.get((src, lk))
            if ev is None:
                out[(kind, 'none seen', 'block' if rec != ZERO_PAIR else 'absent')] += 1
                continue
            what, pair = ev
            if what == 'write':
                v = 'written block' if rec[:16] == pair else 'other'
            elif rec == pair:
                v = 'equal, a block' if rec != ZERO_PAIR else 'equal, absent'
            else:
                v = 'later sequence, same block' if (
                    rec[:16] == pair[:16] and le32(rec, 16) > le32(pair, 16)) else 'other'
            out[(kind, what, v)] += 1
    return out


def selftest(repo):
    """Pin the figures rd vms-874f's sender and answerer are built from. A
    corpus change has to move these deliberately, and whatever quotes them
    (the codec header, the capture README, the grounding rows) moves too."""
    root = os.path.join(repo, 'tests', 'lab', 'captures')
    caps = collect(rc.walk_inputs([root]))
    m, status = measure(caps)
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))

    # 0. THE POPULATION, per capture: the topology the rules below rest on.
    #    Departures (2 survivors in every one): RA1b 3-VAX SHUTDOWN REMOVE_NODE,
    #    equal LOCKDIRWT; E4 3-VAX SHUTDOWN REMOVE_NODE, LOCKDIRWT 0/1/0; E5/E5b
    #    3-VAX, a HUNG node removed by the survivors, LOCKDIRWT 0/0/0;
    #    oracle-3node-fault-f1 3-VAX, a node KILLED; ci6 run16 VAX1 SHUTDOWN
    #    REMOVE_NODE with OVMX the only directory (its answers are OVMX-built and
    #    excluded from every answer figure). Formations: the four SYS$SYS_ID
    #    samples (rd vms-629). rd vms-2fa (one pod, VAX3 the sender, VAX1 the
    #    departing master): arm1 LOCKDIRWT 1/1/0 SHUTDOWN REMOVE_NODE, arm3b
    #    0/1/0 VAX1's simulator stopped, arm2 1/1/1 stopped (both survivors
    #    send); each trimmed to the departure's barrier. NOT COVERED: three or
    #    more survivors, a sub of a sub, an Alpha sender.
    per = sorted((os.path.relpath(p, root).replace(os.sep, '/'), len(r),
                  sum(1 for x in r if is_root(x['q'])),
                  len({x['src'] for x in r})) for p, r in caps.items())
    check('captures carrying op-0x08 (path, records, ROOT, senders)', per, [
        ('vms-2fa-rebuild-step-20261102/arm1-departure.pcap.gz', 281, 263, 1),
        ('vms-2fa-rebuild-step-20261102/arm2-departure.pcap.gz', 796, 776, 2),
        ('vms-2fa-rebuild-step-20261102/arm3b-departure.pcap.gz', 1, 1, 1),
        ('vms-629-op08-20261005/cf1-op08.pcap', 1, 1, 1),
        ('vms-6d3d-coldform-ev2-20260924/coldform-ev2-formation.pcap', 1, 1, 1),
        ('vms-874f-failover-rebuild-20261011/ra1b-departure.pcap.gz', 416, 395, 2),
        ('vms-b36-cnxmgrerr-20260925/oracle/oracle-3node-fault-f1.pcap.gz', 164, 1, 1),
        ('vms-cab-op0f-20261010/heldout1.pcap.gz', 1, 1, 1),
        ('vms-cab-op0f-20261010/heldout2.pcap.gz', 1, 1, 1),
        ('vms-cab-op0f-20261010/rule2/heldout-E4.pcap.gz', 137, 135, 1),
        ('vms-cab-op0f-20261010/rule2/heldout-E5.pcap.gz', 158, 6, 1),
        ('vms-cab-op0f-20261010/rule2/heldout-E5b.pcap.gz', 6, 6, 1),
        ('vms-ci6-evac-20261010/run16/wire.pcap.gz', 3, 3, 1),
    ])
    n = m['root'] + m['sub']
    check('records (ROOT, SUB)', (m['root'], m['sub']), (1590, 376))
    check('...every one sent by a real DEC interface', m['real_sender'], n)

    # 1. THE ANSWER: always 82/01, paired by transaction, echoing the holder's
    #    own lkid at body[24:28]; status 0xf9 or 0xfa and nothing else.
    check('records answered', m['answered'], n)
    check('...by an 82/01', m['answer_op01'], n)
    check('...echoing body[24:28] (the holder\'s own lkid)', m['answer_echo_24'], n)
    check('answers built by a real node (3 are OVMX\'s, ci6 run16)',
          m['answer_by_real'], n - 3)
    check('real answer status by kind',
          sorted(('%s/%02x' % k, v) for k, v in status.items()),
          [('root/f9', 409), ('root/fa', 1178), ('sub/fa', 376)])
    check('0xf9 ROOT answers that echo body[20:24] (no new-master handle)',
          m['root_youmaster_echo_20'], m['root_youmaster_real'])
    check('0xfa answers that REWRITE body[20:24] (the new master\'s lkid), '
          'ROOT and SUB', (m['root_granted_b20_new'], m['sub_granted_b20_new']),
          (1178, 376))
    check('0xfa answers with body[28] == 0x10 (ROOT, SUB); the 23 ROOT '
          'exceptions are 0x14 x16 / 0x18 x7, in RA1b and the vms-2fa arms',
          (m['root_granted_b28_10'], m['sub_granted_b28_10']), (1155, 373))

    odd28 = collections.Counter(
        (os.path.basename(p), r['a'][28]) for p, recs in caps.items() for r in recs
        if is_root(r['q']) and r['a'] is not None and is_real(r['dst'])
        and r['a'][34] == ST_GRANTED and r['a'][28] != 0x10)
    check('...the exceptions, by capture and value', sorted(odd28.items()),
          [(('arm1-departure.pcap.gz', 0x14), 5),
           (('arm1-departure.pcap.gz', 0x18), 4),
           (('arm2-departure.pcap.gz', 0x14), 6),
           (('ra1b-departure.pcap.gz', 0x14), 5),
           (('ra1b-departure.pcap.gz', 0x18), 3)])

    # 2. THE RECORD's cells.
    check('ROOT records whose body[128:132] is refhash(own identity)',
          m['hash_ok'], m['root'])
    #    rd vms-2fa FALSIFIED the old "body[30] is always the higher": arm1's
    #    CACHE$ ROOT (record frame 101) carries body[30] 0 < body[31] 3. Every
    #    differing record is still a system lock (CACHE$, AUDRSV$, UCX$, MSCP$);
    #    neither cell is named.
    check('body[30] == body[31] (the 24 that differ are system locks; '
          'which is granted is NOT grounded)',
          m['modes_equal'], n - 24)
    check('...of the 24, body[30] > body[31] / body[30] < body[31]',
          (sum(1 for recs in caps.values() for r in recs
               if r['q'][30] > r['q'][31]),
           sum(1 for recs in caps.values() for r in recs
               if r['q'][30] < r['q'][31])), (23, 1))

    # 3. THE SUB RULE: the parent pair, and the order.
    check('SUB records whose parent (own earlier record, body[24:28] == '
          'body[36:40]) is in the capture', m['sub_parent_found'], m['sub'])
    check('...and that parent is a ROOT (no sub-of-sub in the corpus)',
          m['sub_parent_is_root'], m['sub'])
    check('...whose body[40:44] is the parent\'s ANSWER body[20:24]',
          m['sub_40_is_parent_answer_20'], m['sub'])
    check('...sent AFTER the parent\'s answer arrived',
          m['sub_parent_answer_first'], m['sub'])
    check('...whose parent was answered 0xfa (a holder told 0xf9 sends no subs)',
          m['sub_parent_answer_granted'], m['sub'])
    check('...addressed where the parent went', m['sub_same_dst_as_parent'],
          m['sub'])

    # 4. WHO ANSWERS WHAT (RA1b, the one capture where BOTH survivors send).
    ra = [r for p, r in caps.items() if p.endswith(RA1B)][0]
    rr = roles(ra)
    check('RA1b 0xf9 answers by (hash>>16)&1 and answerer',
          sorted((k, v) for k, v in rr.items() if k.startswith('f9')),
          [('f9 parity0 08002bb836ea', 48), ('f9 parity1 08002b962ac4', 85)])
    check('RA1b ROOT 0xfa: answered by the directory-and-master / sent BY the '
          'directory to the master it had just told 0xf9',
          (rr['dir_answer'], rr['dir_sender']), (141, 121))

    # 5. RA1b's RMT1 tree against SDA (RA1b-truth.txt).
    rmt = [r for r in ra if ident(r['q'])[2] in (b'RMT1', b'S0', b'S1', b'S2')]
    got = [(r['src'].hex(), ident(r['q'])[2].decode(), le32(r['q'], 24),
            r['q'][30], r['q'][31], le32(r['q'], 36), le32(r['q'], 40),
            r['a'][34], le32(r['a'], 20)) for r in rmt]
    check('RA1b RMT1 tree, in wire order', got, [
        ('08002bb836ea', 'RMT1', RA1B_VAX2_RMT1, 3, 3, 0, 0, 0xf9, 0x20400217),
        ('08002b962ac4', 'RMT1', 0x0E00049B, 1, 1, 0, 0, 0xfa, 0x01000563),
        ('08002b962ac4', 'S2', 0x010004A6, 2, 2, 0x0E00049B, 0x01000563, 0xfa, 0x01000564),
        ('08002b962ac4', 'S1', 0x010004A5, 2, 2, 0x0E00049B, 0x01000563, 0xfa, 0x01000565),
        ('08002b962ac4', 'S0', 0x33000498, 2, 2, 0x0E00049B, 0x01000563, 0xfa, 0x01000566),
    ])
    check('...every VAX3 lkid and mode is the SDA truth',
          all(RA1B_VAX3_TRUTH.get(lk) == (nm, m30) for (s, nm, lk, m30, _m31,
              _a, _b, _c, _d) in got if s == '08002b962ac4'), True)
    check('...the ROOT record carries the value block at body[88:108] and the '
          '0xfa answer returns it at body[36:56]',
          (rmt[1]['q'][88:104], rmt[1]['a'][36:52]),
          (b'RMLVB-0123456789', b'RMLVB-0123456789'))

    # 6. SYS$SYS_ID at a formation barrier is the SAME message: a ROOT record of
    #    the node's own lock, answered 0xf9 by its directory.
    sysid = [r for recs in caps.values() for r in recs
             if ident(r['q'])[2].startswith(b'SYS$SYS_ID')]
    check('SYS$SYS_ID records (root, lkid, modes, access mode, answer status)',
          sorted({(is_root(r['q']), le32(r['q'], 24), r['q'][30], r['q'][31],
                   r['q'][46], r['a'][34]) for r in sysid}),
          [(True, 0x01000000, 5, 5, 1, 0xf9)])
    check('...count', len(sysid), 4)

    # 7. PHASE: no op-0x0d registration lies inside a capture's op-0x08 run.
    check('op-0x0d frames between the first and last op-0x08, all captures',
          sum(interleaved_0d(p, r) for p, r in caps.items()), 0)

    # 8. THE BARRIER STEP (rd vms-bcb, grounded by rd vms-2fa). Every record
    #    whose barrier is in the capture is sent inside the transition: after
    #    the release of step 5 when the SENDER's own LOCKDIRWT is non-zero
    #    (RA1b, f1, coldform, vms-2fa arm2), after the release of step 6 when
    #    it is zero (E4, E5, E5b, run16, heldout1/2, vms-2fa arm1/arm3b), and
    #    answered before the next release. vms-2fa flips only VAX3's weight in
    #    one lab: 0 -> release 6 (arm1, arm3b), 1 -> release 5 (arm2). The
    #    weight is not on the wire; the per-sender split is pinned by
    #    tests/lab/captures/vms-2fa-rebuild-step-20261102/rebuild_step.py
    #    --selftest. OVMX's one record after release 12 (lab33a) bugchecked a
    #    real VAX LOCKMGRERR.
    per_step = collections.Counter()
    verdict = collections.Counter()
    for p, recs in caps.items():
        st, cl = barrier_steps(p, recs)
        for k, v in st.items():
            per_step[(os.path.basename(p), k)] += v
        verdict.update(cl)
    check('op-0x08 records by the last barrier release before them',
          sorted(per_step.items(), key=lambda kv: (kv[0][0], str(kv[0][1]))), [
              (('arm1-departure.pcap.gz', 6), 281),
              (('arm2-departure.pcap.gz', 5), 796),
              (('arm3b-departure.pcap.gz', 6), 1),
              (('cf1-op08.pcap', None), 1),
              (('coldform-ev2-formation.pcap', 5), 1),
              (('heldout-E4.pcap.gz', 6), 137),
              (('heldout-E5.pcap.gz', 6), 158),
              (('heldout-E5b.pcap.gz', 6), 6),
              (('heldout1.pcap.gz', 6), 1),
              (('heldout2.pcap.gz', 6), 1),
              (('oracle-3node-fault-f1.pcap.gz', 5), 164),
              (('ra1b-departure.pcap.gz', 5), 416),
              (('wire.pcap.gz', 6), 3),
          ])
    check('...answered before the next release (inside the step)',
          sorted(verdict.items()),
          [('answered before the next release', 1965),
           ('no barrier in the capture', 1)])

    # 9. THE VALUE BLOCK A SURVIVOR RELAYS (rd vms-8159). body[88:108] is the
    #    block and sequence the holder's lock was last TOLD -- by its master's
    #    grant, or an op-0x1a that supersedes it -- and never a block without
    #    its sequence. ROOT and SUB alike: what decides it is the lock's own
    #    history, not its place in the tree (RA1b's S0..S2, taken without
    #    LCK$M_VALBLK under a written root, carry zeros; RMT1 carries
    #    RMLVB-0123456789 at 1 from BOTH survivors -- the master's LVB in SDA,
    #    the only value the holders were ever granted).
    blk = collections.Counter()
    for recs in caps.values():
        for r in recs:
            q = r['q']
            blk[('ROOT' if is_root(q) else 'SUB',
                 q[88:104] != b'\0' * 16, le32(q, 104) != 0)] += 1
    check('op-0x08 records by (kind, block non-zero, sequence non-zero)',
          sorted(blk.items()),
          [(('ROOT', False, False), 1547), (('ROOT', False, True), 3),
           (('ROOT', True, True), 40), (('SUB', False, False), 13),
           (('SUB', False, True), 32), (('SUB', True, True), 331)])
    check('RA1b RMT1 tree body[88:108] (sender, lock, block, sequence)',
          [(r['src'].hex(), ident(r['q'])[2].decode(), r['q'][88:104],
            le32(r['q'], 104)) for r in rmt], [
              ('08002bb836ea', 'RMT1', b'RMLVB-0123456789', 1),
              ('08002b962ac4', 'RMT1', b'RMLVB-0123456789', 1),
              ('08002b962ac4', 'S2', b'\0' * 16, 0),
              ('08002b962ac4', 'S1', b'\0' * 16, 0),
              ('08002b962ac4', 'S0', b'\0' * 16, 0)])
    hp08, hp12 = collections.Counter(), collections.Counter()
    for p in rc.walk_inputs([root]):
        hp08.update(holder_pairs(p, OP_REBUILD))
        hp12.update(holder_pairs(p, 0x12))
    # The holder's LAST event for the lock decides it. After a grant the
    # record IS that grant's body[36:56] in 500 of 508 (op-0x08 142 of 148,
    # op-0x12 358 of 360); the wire shows only three of those with a NON-zero
    # block (op-0x12 ROOTs) -- RA1b's two RMT1 records are the op-0x08 proof,
    # against SDA rather than a captured grant, because the grants predate
    # the window. After an op-0x1a the record is the op-0x1a's (op-0x08 3 of
    # 3, op-0x12 251 of 259). After the holder's OWN op-0x06 write it carries
    # the bytes written (op-0x08 145 of 153, op-0x12 67 of 83) with a sequence
    # no frame delivers -- rd vms-85c1's, so OVMX sends the absent form there.
    # The eight grant exceptions are all heldout-E5/E5b (a hung node removed).
    check('op-0x08: the record against the holder\'s last event for the lock',
          sorted(hp08.items()), [
              (('ROOT', '1a', 'equal, absent'), 3),
              (('ROOT', 'grant', 'equal, absent'), 142),
              (('ROOT', 'grant', 'other'), 1),
              (('ROOT', 'none seen', 'absent'), 1400),
              (('ROOT', 'none seen', 'block'), 40),
              (('ROOT', 'write', 'other'), 2),
              (('ROOT', 'write', 'written block'), 2),
              (('SUB', 'grant', 'other'), 5),
              (('SUB', 'none seen', 'absent'), 11),
              (('SUB', 'none seen', 'block'), 211),
              (('SUB', 'write', 'other'), 6),
              (('SUB', 'write', 'written block'), 143)])
    check('op-0x12: the same rule (after an op-0x1a it; otherwise the grant)',
          sorted(hp12.items()), [
              (('ROOT', '1a', 'equal, a block'), 7),
              (('ROOT', '1a', 'equal, absent'), 6),
              (('ROOT', 'grant', 'equal, a block'), 3),
              (('ROOT', 'grant', 'equal, absent'), 355),
              (('ROOT', 'grant', 'other'), 1),
              (('ROOT', 'none seen', 'absent'), 804),
              (('ROOT', 'none seen', 'block'), 25),
              (('ROOT', 'write', 'other'), 6),
              (('ROOT', 'write', 'written block'), 5),
              (('SUB', '1a', 'equal, a block'), 224),
              (('SUB', '1a', 'equal, absent'), 14),
              (('SUB', '1a', 'later sequence, same block'), 6),
              (('SUB', '1a', 'other'), 2),
              (('SUB', 'grant', 'other'), 1),
              (('SUB', 'none seen', 'absent'), 25),
              (('SUB', 'none seen', 'block'), 2385),
              (('SUB', 'write', 'other'), 10),
              (('SUB', 'write', 'written block'), 62)])

    print('dlm_rebuild08_census selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
