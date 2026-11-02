#!/usr/bin/env python3
"""rec12_holder_census.py -- what a HOLDER's op-0x12 record carries (rd vms-33a).

usage: rec12_holder_census.py <pcap|dir> ...        the per-cell census
       rec12_holder_census.py --holder <pcap|dir> ...  who sends, and when (H1-H4)
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


def holder_figures(paths):
    """WHO sends a new master records and an op-0x13, and WHEN (rd vms-33a,
    vms_cluster_codec_dlm.h H1..H4). Every figure is a count over the frames;
    none names a cell beyond the role it is measured in."""
    f = collections.Counter()
    nm_by_cap = {}
    for p in paths:
        rows = list(rc.sca_bodies(p))
        cur, last0f, told = {}, {}, set()
        recs_by = collections.defaultdict(list)
        for i, s, d, b in rows:
            if b[8] == 0x02 and b[9] == 0x12:
                recs_by[s].append((i, bytes(b[24:28])))
        out = collections.defaultdict(list)      # (s, d) -> unanswered txns
        h0e = {}
        for i, s, d, b in rows:
            cat, op = b[8], b[9]
            k = rc.rm_ident(b) if cat == 0x02 and op != 0x12 else None
            if cat == 0x02 and op == 0x0e and k:
                cur[k] = (s, d)
                h0e[k] = bytes(b[128:132])
            elif cat == 0x02 and op == 0x0f and k in cur and s == cur[k][0]:
                nm = int.from_bytes(b[28:32], 'little')
                f['0x0f from the old master'] += 1
                if nm >> 16 == 0x0001 and (nm & 0xffff) != 0:
                    f['0x0f naming a CSID-shaped member'] += 1
                nm_by_cap.setdefault(p, collections.defaultdict(set))[
                    cur[k][1]].add(nm)
                last0f[(k, d)] = i
            elif cat == 0x02 and op == 0x11 and k:
                f['0x11'] += 1
                if (k, d) in last0f:
                    f['0x11 after a 0x0f for the tree to that member'] += 1
                told.add((k, d))
            elif cat == 0x02 and op == 0x13 and k in cur:
                o = cur[k][0]
                if (k, s) in told and d == cur[k][1]:
                    f['holder 0x13'] += 1
                    if b[28] == 0:
                        f['holder 0x13 with body[28] = 0'] += 1
                    if bytes(b[128:132]) == h0e[k]:
                        f['holder 0x13 carrying the 0x0e hash'] += 1
                elif s == o:
                    f['old-master 0x13'] += 1
                    if b[28] in (1, 2):
                        f['old-master 0x13 with body[28] in {1,2}'] += 1
            elif cat == 0x02 and op == 0x1a:
                lk = bytes(b[24:28])
                later = [j for j, l in recs_by[d] if j > i and l == lk]
                if later:
                    f['op-0x1a a holder relays'] += 1
                    rec = [bb for (ii, ss, dd, bb) in rows if ii == later[0]]
                    if rec and bytes(rec[0][88:108]) == bytes(b[36:56]):
                        f['...relayed byte for byte at body[88:108]'] += 1
                    if any(j < i for (kk, mm), j in last0f.items()
                           if mm == d):
                        f['...after a 0x0f to that holder'] += 1
            if cat == 0x02 and op == 0x12:
                f['records'] += 1
                if not out[(s, d)]:
                    f['records sent with none of that holder unanswered'] += 1
                out[(s, d)].append(bytes(b[4:8]))
            elif cat == 0x82 and op == 0x01 and bytes(b[4:8]) in out[(d, s)]:
                out[(d, s)].remove(bytes(b[4:8]))
    inj, multi = 0, 0
    for cap, m in nm_by_cap.items():
        if len(m) > 1:
            multi += 1
            single = all(len(v) == 1 for v in m.values())
            vals = [next(iter(v)) for v in m.values() if len(v) == 1]
            if single and len(set(vals)) == len(vals):
                inj += 1
    f['captures with >1 new master'] = multi
    f['...where body[28:32] is one value per new master, distinct'] = inj
    # Rule H2 and H3, on the episodes the census groups.
    for e in rc.role_episodes(paths):
        seq = e['seq']
        for m in {d for _i, o, _s, d in seq if o == 0x11}:
            f['0x11 recipients (completed episodes)'] += 1
            s13 = [i for i, o, s, _d in seq if o == 0x13 and s == m]
            if s13:
                f['...that send the new master a 0x13'] += 1
                i11 = min(i for i, o, _s, d in seq if o == 0x11 and d == m)
                if min(s13) > i11:
                    f['...after the 0x11'] += 1
            if not any(o == 0x12 and s == m for _i, o, s, _d in seq):
                f['...that sent no record at all'] += 1
    for e in rc.attributable_episodes(rc.role_episodes(paths)):
        seq = e['seq']
        for m in {d for _i, o, _s, d in seq if o == 0x11}:
            i11 = min(i for i, o, _s, d in seq if o == 0x11 and d == m)
            i12 = [i for i, o, s, _d in seq if o == 0x12 and s == m]
            i13 = [i for i, o, s, _d in seq if o == 0x13 and s == m]
            if i12 and i13:
                f['attributable holders with records'] += 1
                if min(i12) > i11 and max(i12) < min(i13):
                    f['...records after the 0x11 and before its 0x13'] += 1
    return f


def report_holder(paths):
    for k, v in holder_figures(paths).items():
        print('  %-60s %d' % (k, v))
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

    # rd vms-e80 taught the shared walker to read `.pcap.gz`, which is 32 more
    # captures: 420 -> 3,521 records (ROOT 9 -> 792, SUB 411 -> 2,729). Every
    # requester is still a real DEC OUI (2,780 aa0004 + 741 08002b); OVMX has
    # never sent an op-0x12, which is rd vms-33a's whole point.
    # rd vms-874f: the RA1b VAX-only departure capture adds 419 records, every
    # one a ROOT record (the departing VAX1 hands CACHE$/F11B$/SYS$_/VCC$ roots
    # to a survivor) -- SUB does not move, and neither does the constant-cell
    # list below.
    check('records paired with an answer', len(recs), 3940)   # was 420, 3521
    check('ROOT records', len(root), 1211)                    # was 9, 792
    check('SUB records', len(sub), 2729)                      # was 411

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
    # THE CAUTION IN THE HEADER WAS RIGHT, AND THE WIDER CORPUS PROVED IT (rd
    # vms-e80). Over 420 records forty-four cells looked constant; over 3,521
    # thirty-four do. TEN DROPPED OUT, and they are exactly the cells the
    # header warned a builder not to treat as constants:
    #   body[ 44]  0 x3,519 / 1 x2        the UIC group the header named: a
    #                                     holder with a non-zero group exists
    #   body[ 63]  0x20 x3,416, then 0x00/0x53/0x44/0x72    name tail, not padding
    #   body[ 64]  0x20 x3,421, then 0x50/0xc0/0x4b
    #   body[ 65]  0x20 x3,422, then 0x5a/0x57/0x31
    #   body[ 17]  0 x3,148 / 1 x187 / 2 x186
    #   body[ 34]  1 x3,089 / 0 x432
    #   body[ 71]  0 x3,224 / 0x25 x193 / 0x49 x49 / 0x87 x47
    #   body[108]  0 x3,509 / 4 x8 / 8 x4
    #   body[109]  0 x3,339 / 2 x182
    #   body[110]  1 x3,516 / 0 x5
    # None is named here and none is guessed: a cell gets a name in the codec
    # header, with a capture or an SDA page behind it (Rule 8). What this
    # records is that a builder writing any of the ten as a constant would be
    # writing the OLD corpus's accident.
    check('cells constant across ALL 3,521 records',
          [off for off, _v in agree],
          [5, 8, 9, 12, 13, 15, 18, 19, 26, 33, 35, 38, 42, 45, 86, 87,
           106, 107, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122,
           123, 124, 125, 126, 127])
    check('and their values',
          [v for _off, v in agree],
          [0x00, 0x02, 0x12, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
           0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
           0xff])

    # body[36:44] is the PARENT PAIR, and it is what tells the two apart.
    check('every ROOT record has an all-zero body[36:44]',
          all(q[36:44] == b'\0' * 8 for (q, _a) in root), True)
    check('every SUB record has a NON-zero body[36:44]',
          all(q[36:44] != b'\0' * 8 for (q, _a) in sub), True)

    # rd vms-33a: WHO sends, and WHEN -- the figures vms_cluster_codec_dlm.h
    # H1..H4 quotes and the holder half of vms_dlm_rm_fsm.c implements.
    h = holder_figures(paths)
    pin = [
        ('0x11', 394),
        ('0x11 after a 0x0f for the tree to that member', 394),   # H1
        ('0x0f from the old master', 1257),
        ('0x0f naming a CSID-shaped member', 1257),                # H1
        ('captures with >1 new master', 17),
        ('...where body[28:32] is one value per new master, distinct', 17),
        ('0x11 recipients (completed episodes)', 391),             # H2
        ('...that send the new master a 0x13', 391),
        ('...after the 0x11', 391),
        ('...that sent no record at all', 15),
        ('attributable holders with records', 7),                  # H3
        ('...records after the 0x11 and before its 0x13', 7),
        ('records', 3522),
        ('records sent with none of that holder unanswered', 2991),
        ('holder 0x13', 391),                                      # H4
        ('holder 0x13 with body[28] = 0', 391),
        ('holder 0x13 carrying the 0x0e hash', 391),
        ('old-master 0x13', 841),
        ('old-master 0x13 with body[28] in {1,2}', 841),
        ('op-0x1a a holder relays', 259),
        ('...after a 0x0f to that holder', 259),
        ('...relayed byte for byte at body[88:108]', 251),
    ]
    for name, want in pin:
        check('holder: ' + name, h[name], want)
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    if argv[1] == '--holder':
        return report_holder(rc.walk_inputs(argv[2:]))
    return report(rc.walk_inputs(argv[1:]))


if __name__ == '__main__':
    sys.exit(main(sys.argv))
