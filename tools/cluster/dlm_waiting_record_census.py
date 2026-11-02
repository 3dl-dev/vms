#!/usr/bin/env python3
"""dlm_waiting_record_census.py -- a holder's WAITING and CONVERTING locks in
op-0x12 (remaster) and op-0x08 (departure rebuild) records (rd vms-0db).

usage: dlm_waiting_record_census.py <pcap|dir> ...     the census
       dlm_waiting_record_census.py --selftest <repo>  pin the rule

WHY THIS EXISTS. OVMX's holder sender (vms_lock.c dlm_handover_fill) refuses a
lock whose `waiting` flag is set, and the op-0x08 rebuild path shares that fill,
so a waiting or converting OVMX lock is never told to a new master and hangs.
The VAX-only capture tests/lab/captures/vms-0db-waiting-records-20261102 drove
real V7.3 holders with locks SDA listed on conversion and waiting queues, and
moved their trees both ways (activity remaster, master SHUTDOWN). This states
what a real holder sends for them.

THE RULE (every cell checked against SDA on the holder before and on the new
master after):
  * A holder sends ONE record per LKB it owns in the tree: granted, converting
    and waiting alike, in op-0x12 and in op-0x08.
  * body[30] = the LKB's REQUESTED mode, body[31] = its GRANTED mode. A waiting
    (never granted) lock carries granted 0. body[30] is NOT a reliable "what is
    pending": a GRANTED lock that was down-converted keeps the earlier request
    (RMHOLD's EX -> CR root is (5,1) and granted), so differing modes do not by
    themselves mean waiting.
  * body[110] = the LKB's QUEUE STATE: 0x01 granted, 0x00 converting, 0xff
    waiting. It is the only cell that separates a converting record from a
    granted down-converted one with the same (5,1) modes.
  * body[28] carries the lock's flags (0x01 VALBLK, 0x02 CONVERT, 0x10 SYSTEM,
    as SDA names them); CONVERT stays set on a granted lock after a conversion,
    so it does not mark state either.
  * Per sender and resource, records go in queue order: granted, then
    converting, then waiting.
  * body[108] carries bit 0x04 on EVERY non-granted record (and on a few
    granted system locks, which also show 0x08 there); what it means is not
    named. The 0xfa answer's body[28] is 0x10 | the record's body[108] -- on
    all 5,694 real 0xfa answers -- which is why a non-granted record is
    answered 0x14 where a granted one is answered 0x10.
  * The answer is otherwise an 82/01 like any other: the new master's lkid at
    body[20:24], status 0xfa (0xf9 when the sender becomes the master). The
    new master puts each lock on the queue it came from (SDA after, both arms).

Clean-room: reads our own lab captures only. No VMS binary or source.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402

CAP = os.path.join('vms-0db-waiting-records-20261102')
GRANTED, CONVERTING, WAITING = 0x01, 0x00, 0xff
RANK = {GRANTED: 0, CONVERTING: 1, WAITING: 2}
REAL_OUI = ('08002b', 'aa0004')

# SDA truth, read on the holder before the move (console-vax3.log and, for
# WQRB's PR root, console-vax2.log): holder lkid -> (queue state, rq, gr).
TRUTH = {
    'armA4-remaster-WQRC.pcap': {
        0x070003F0: (GRANTED, 1, 1), 0x070003EE: (GRANTED, 1, 1),
        0x100003DB: (GRANTED, 1, 1), 0x060000E0: (GRANTED, 1, 1),
        0x28000363: (GRANTED, 1, 1), 0x080003ED: (CONVERTING, 5, 1),
        0x080003EF: (WAITING, 5, 0), 0x050000E8: (GRANTED, 1, 1),
        0x060003DC: (GRANTED, 1, 1)},
    'armA5-remaster-WQRE.pcap': {
        0x4F0003F4: (GRANTED, 1, 1), 0x300003F9: (CONVERTING, 5, 1),
        0x070003FC: (WAITING, 5, 0), 0x100003FB: (GRANTED, 1, 1),
        0x460003F8: (GRANTED, 1, 1)},
    'armB-departure-WQRB.pcap.gz': {
        0x07000401: (GRANTED, 1, 1), 0x170003F0: (GRANTED, 1, 1),
        0x0D0003EE: (GRANTED, 1, 1), 0x270000AB: (GRANTED, 1, 1),
        0x07000403: (CONVERTING, 5, 1), 0x07000404: (WAITING, 5, 0),
        0x100003FC: (GRANTED, 1, 1), 0x3D0003F2: (CONVERTING, 5, 1),
        0x08000400: (WAITING, 5, 0), 0x310003FD: (GRANTED, 1, 1),
        0x300003D3: (GRANTED, 1, 1), 0x16000403: (GRANTED, 3, 3)},
}
TREES = (b'WQRC', b'WQRE', b'WQRB')


def le32(b, off):
    return int.from_bytes(b[off:off + 4], 'little')


def name(b):
    n = b[47]
    return bytes(b[48:48 + n]) if 1 <= n <= 31 else b''


def records(path):
    """Every op-0x12 / op-0x08 record of one capture, its answer attached by
    the SCS transaction at body[4:8] (never by time)."""
    rows = list(rc.sca_bodies_with_src(path))
    ans = {}
    for idx, src, dst, b in rows:
        if b[8] == 0x82 and b[4:8] != b'\0\0\0\0':
            ans.setdefault((bytes(dst), bytes(src), bytes(b[4:8])), (idx, bytes(b)))
    out = []
    for idx, src, dst, b in rows:
        if b[8] != 0x02 or b[9] not in (0x08, 0x12):
            continue
        a = ans.get((bytes(src), bytes(dst), bytes(b[4:8])))
        if a is not None and a[0] < idx:
            a = None
        out.append(dict(idx=idx, src=bytes(src), dst=bytes(dst), q=bytes(b),
                        a=a[1] if a else None))
    return out


def figures(paths):
    f = collections.Counter()
    order_bad = []
    for p in paths:
        recs = records(p)
        groups = collections.defaultdict(list)
        for r in recs:
            q = r['q']
            if r['src'][:3].hex() not in REAL_OUI:
                continue
            st, op = q[110], q[9]
            f['op%02x records' % op] += 1
            f['op%02x state %02x' % (op, st)] += 1
            if st == GRANTED and q[30] != q[31]:
                f['op%02x GRANTED with body[30] != body[31]' % op] += 1
            if st != GRANTED:
                f['non-granted'] += 1
                if q[30] != q[31]:
                    f['non-granted with body[30] != body[31]'] += 1
                if st == WAITING and q[31] == 0:
                    f['waiting with granted cell 0'] += 1
                if q[108] & 0x04:
                    f['non-granted with body[108] bit 0x04'] += 1
            elif q[108] & 0x04:
                f['GRANTED with body[108] bit 0x04'] += 1
            a = r['a']
            if a is not None and a[8] == 0x82 and a[9] == 0x01 and a[34] == 0xfa:
                key = 'granted' if st == GRANTED else 'non-granted'
                f['0xfa answers to %s' % key] += 1
                if a[28] == 0x10 | q[108]:
                    f['0xfa answers with body[28] == 0x10 | record body[108]'] += 1
                if st != GRANTED and a[28] == 0x14:
                    f['0xfa answers to non-granted with body[28] 0x14'] += 1
            root = q[36:44] == b'\0' * 8
            groups[(r['src'], q[9], name(q), root)].append(st)
        for k, sts in groups.items():
            if any(s not in RANK for s in sts):
                f['groups with an unknown state'] += 1
                continue
            ranks = [RANK[s] for s in sts]
            if len(set(sts)) > 1:
                f['groups mixing states'] += 1
                if ranks == sorted(ranks):
                    f['...in queue order (granted, converting, waiting)'] += 1
                else:
                    order_bad.append((os.path.basename(p), k[2]))
    return f, order_bad


def truth_check(repo):
    """Every record of the vms-0db arms, cell by cell, against SDA."""
    base = os.path.join(repo, 'tests', 'lab', 'captures', CAP)
    got = {}
    for cap, truth in TRUTH.items():
        rows = []
        for r in records(os.path.join(base, cap)):
            q = r['q']
            if name(q) not in TREES + (b'W0', b'S0', b'S1'):
                continue
            lk = le32(q, 24)
            if lk not in truth:
                continue
            rows.append((lk, q[9], q[110], q[30], q[31],
                         truth[lk] == (q[110], q[30], q[31]),
                         r['a'] is not None and r['a'][34] in (0xf9, 0xfa)))
        got[cap] = rows
    return got


def report(paths):
    f, bad = figures(paths)
    for k in sorted(f):
        print('  %-58s %d' % (k, f[k]))
    for b in bad:
        print('  OUT OF QUEUE ORDER: %s %r' % b)
    return 0


def selftest(repo):
    ok = True

    def check(label, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', label, got,
                                   '' if good else ' (want %r)' % (want,)))

    # 1. THE ARMS, against SDA: every LKB recorded, every cell as SDA says.
    t = truth_check(repo)
    for cap, rows in sorted(t.items()):
        lks = {r[0] for r in rows}
        check('%s: every SDA-listed holder LKB has a record' % cap,
              sorted(set(TRUTH[cap]) - lks), [])
        check('%s: (body[110], body[30], body[31]) == SDA (state, rq, gr)' % cap,
              [hex(r[0]) for r in rows if not r[5]], [])
        check('%s: every record answered (0xfa, or 0xf9 to a sender that becomes the master)' % cap,
              [hex(r[0]) for r in rows if not r[6]], [])
    a5 = records(os.path.join(repo, 'tests', 'lab', 'captures', CAP,
                              'armA5-remaster-WQRE.pcap'))
    check('WQRE: the old master\'s own GRANTED root (RMHOLD EX -> CR, SDA on '
          'the new master: CR on the granted queue) is (5,1) with state 0x01 '
          '-- the modes alone do not mark a converting lock',
          [(r['q'][30], r['q'][31], r['q'][110]) for r in a5
           if r['q'][9] == 0x12 and le32(r['q'], 24) == 0x5D000608],
          [(5, 1, 1)])

    # 2. THE CORPUS: body[110] is a queue state, and the rule holds everywhere.
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    f, bad = figures(paths)
    # Before vms-0db's arms the corpus already held 25 non-granted records,
    # all system locks (CACHE$ converting EX over CW, AUDRSV$ NL -> PW, a
    # waiting UCX$INETACP_ EX) in departures and the E4 shutdown arm.
    check('op-0x12 records by body[110] (granted/converting/waiting)',
          (f['op12 state 01'], f['op12 state 00'], f['op12 state ff']),
          (3948, 7, 2))
    check('op-0x08 records by body[110]',
          (f['op08 state 01'], f['op08 state 00'], f['op08 state ff']),
          (2188, 29, 4))
    check('records whose body[110] is none of 01/00/ff',
          f['op12 records'] + f['op08 records']
          - sum(f['op%s state %s' % (o, s)] for o in ('12', '08')
                for s in ('01', '00', 'ff')), 0)
    check('GRANTED records whose body[30] != body[31] (op-0x12, op-0x08): '
          'differing modes are NOT a waiting marker',
          (f['op12 GRANTED with body[30] != body[31]'],
           f['op08 GRANTED with body[30] != body[31]']), (2396, 2))
    check('non-granted records / with body[30] != body[31]',
          (f['non-granted'], f['non-granted with body[30] != body[31]']),
          (42, 42))
    check('waiting records with granted cell body[31] == 0',
          f['waiting with granted cell 0'], 6)
    check('body[108] bit 0x04: every non-granted / granted ones too',
          (f['non-granted with body[108] bit 0x04'],
           f['GRANTED with body[108] bit 0x04']), (42, 10))
    check('0xfa answers (granted, non-granted) / body[28] == 0x10 | the '
          'record\'s body[108] / non-granted answered 0x14',
          (f['0xfa answers to granted'], f['0xfa answers to non-granted'],
           f['0xfa answers with body[28] == 0x10 | record body[108]'],
           f['0xfa answers to non-granted with body[28] 0x14']),
          (5661, 33, 5694, 33))
    check('sender/resource groups mixing states, all in queue order',
          (f['groups mixing states'],
           f['...in queue order (granted, converting, waiting)'], bad),
          (4, 4, []))
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    return report(rc.walk_inputs(argv[1:]))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
