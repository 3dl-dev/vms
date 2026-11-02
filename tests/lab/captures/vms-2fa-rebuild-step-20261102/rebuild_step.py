#!/usr/bin/env python3
"""rebuild_step.py -- in which barrier step does a survivor send its op-0x08
rebuild records (rd vms-2fa)? Committed with PLAN.md BEFORE the fire it scores.

usage: rebuild_step.py <pcap> [MAC=LOCKDIRWT ...]
       rebuild_step.py --selftest <repo>     the corpus rule, re-derived

Per transition that carries a cat-0x02 op-0x08, per SENDER:
  rel_first / rel_last  the step index (body[16]) of the last cat-0x01 op-0x0c
                        release before the sender's first / last record;
  dir_step              the release that closes the transition's op-0x0d run
                        (the directory re-registration step), or None if the
                        transition carries no op-0x0d;
  closed                every record answered (82/01, same body[4:8]) before
                        the NEXT release;
  held                  the sender's next barrier frame after its last answer
                        is its own next step report (0b, participant) or
                        release (0c, coordinator), and none of its own barrier
                        frames lies between its first record and last answer.
The LOCKDIRWT column is what the caller states for that MAC (read on the node
with F$GETSYI("LOCKDIRWT")); the capture does not carry it.

Clean-room: reads our own lab captures only.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', '..', 'tools', 'cluster'))
import remaster_census as rc  # noqa: E402


def step(b):
    return struct.unpack_from('<I', b, 16)[0]


def epoch(b):
    return struct.unpack_from('<I', b, 12)[0]


def transitions(path):
    """[(epoch, [rows])] -- a transition runs from its first 01/08|09|0a to its
    release of step 12; the DLM frames in between belong to it."""
    rows = list(rc.sca_bodies_with_src(path))
    out, cur = [], None
    for r in rows:
        b = r[3]
        if b[8] == 0x01 and b[9] in (0x08, 0x09, 0x0a) and (
                cur is None or epoch(b) != cur[0]):
            cur = (epoch(b), [])      # an unfinished transition is superseded
        if cur is not None:
            cur[1].append(r)
            if b[8] == 0x01 and b[9] == 0x0c and step(b) == 12:
                out.append(cur)
                cur = None
    return out


def score(path, ldw=None):
    ldw = ldw or {}
    res = []
    for ep, rows in transitions(path):
        recs = [r for r in rows if r[3][8] == 0x02 and r[3][9] == 0x08]
        if not recs:
            continue
        rel = [(r[0], step(r[3]), bytes(r[1])) for r in rows
               if r[3][8] == 0x01 and r[3][9] == 0x0c]
        coord = rel[0][2]
        d0d = [r[0] for r in rows if r[3][8] == 0x02 and r[3][9] == 0x0d]
        dir_step = None
        if d0d:
            dir_step = min(st for idx, st, _ in rel if idx > max(d0d))
        ans = {}
        for r in rows:
            if r[3][8] == 0x82 and r[3][9] == 0x01:
                ans.setdefault((bytes(r[2]), bytes(r[1]), bytes(r[3][4:8])), []).append(r[0])
        for snd in sorted({bytes(r[1]) for r in recs}):
            mine = [r for r in recs if bytes(r[1]) == snd]

            def last_rel(i):
                b = [st for idx, st, _ in rel if idx < i]
                return b[-1] if b else None
            closed, last_a = True, 0
            for r in mine:
                a = [i for i in ans.get((bytes(r[1]), bytes(r[2]), bytes(r[3][4:8])), [])
                     if i > r[0]]
                nxt = [idx for idx, _st, _ in rel if idx > r[0]]
                if not a or (nxt and a[0] > nxt[0]):
                    closed = False
                if a:
                    last_a = max(last_a, a[0])
            own = [(r[0], r[3][9], step(r[3])) for r in rows
                   if bytes(r[1]) == snd and r[3][8] == 0x01 and r[3][9] in (0x0b, 0x0c)]
            inside = [o for o in own if mine[0][0] < o[0] < last_a]
            after = [o for o in own if o[0] > last_a]
            role = 'coordinator' if snd == coord else 'participant'
            want = 0x0c if role == 'coordinator' else 0x0b
            held = (not inside and bool(after) and after[0][1] == want
                    and after[0][2] == last_rel(mine[0][0]) + 1)
            res.append(dict(epoch=ep, sender=snd.hex(), role=role,
                            ldw=ldw.get(snd.hex()), n=len(mine),
                            rel_first=last_rel(mine[0][0]),
                            rel_last=last_rel(mine[-1][0]),
                            dir_step=dir_step, closed=closed, held=held,
                            first_idx=mine[0][0], last_idx=mine[-1][0]))
    return res


def rule(ldw):
    """THE CANDIDATE (PLAN.md H1): the release a survivor's records follow."""
    return 5 if ldw else 6


# The corpus, with each sender's LOCKDIRWT as its capture's README states it
# (vms-874f RA1b: equal weights; vms-cab rule2 RESULTS.md: E4 0/1/0, E5 0/0/0;
# vms-ci6-evac README: both VAXes 0; heldout1/2 the same vaxlab-3 pod;
# b36 oracle f1 and 6d3d coldform: the image default, not stated in-tree).
CORPUS = [
    ('vms-874f-failover-rebuild-20261011/ra1b-departure.pcap.gz',
     {'08002b962ac4': 1, '08002bb836ea': 1}),
    ('vms-b36-cnxmgrerr-20260925/oracle/oracle-3node-fault-f1.pcap.gz',
     {'08002b9cd904': 1}),
    ('vms-6d3d-coldform-ev2-20260924/coldform-ev2-formation.pcap',
     {'08002b067d8a': 1}),
    ('vms-cab-op0f-20261010/rule2/heldout-E4.pcap.gz', {'08002bf2dfa5': 0}),
    ('vms-cab-op0f-20261010/rule2/heldout-E5.pcap.gz', {'08002bf2dfa5': 0}),
    ('vms-cab-op0f-20261010/rule2/heldout-E5b.pcap.gz', {'08002bf2dfa5': 0}),
    ('vms-cab-op0f-20261010/heldout1.pcap.gz', {'08002bb15b7d': 0}),
    ('vms-cab-op0f-20261010/heldout2.pcap.gz', {'08002bb15b7d': 0}),
    ('vms-ci6-evac-20261010/run16/wire.pcap.gz', {'aa0004000204': 0}),
    # rd vms-2fa's own arms: VAX3 08002b98a48f, VAX2 08002b8b8727, each node's
    # weight read with F$GETSYI("LOCKDIRWT") before the arm (README.md).
    ('vms-2fa-rebuild-step-20261102/arm1-departure.pcap.gz', {'08002b98a48f': 0}),
    ('vms-2fa-rebuild-step-20261102/arm3b-departure.pcap.gz', {'08002b98a48f': 0}),
    ('vms-2fa-rebuild-step-20261102/arm2-departure.pcap.gz',
     {'08002b98a48f': 1, '08002b8b8727': 1}),
]
# The pinned outcome: records per (LOCKDIRWT, release they follow). A corpus
# change moves this deliberately.
WANT = {(1, 5): 1377, (0, 6): 588}


def show(rows):
    for r in rows:
        print('  ep=%x %s %-11s LDW=%s n=%-4d rel %s..%s dir_step=%s closed=%s held=%s'
              % (r['epoch'], r['sender'], r['role'], r['ldw'], r['n'], r['rel_first'],
                 r['rel_last'], r['dir_step'], r['closed'], r['held']))


def selftest(repo):
    root = os.path.join(repo, 'tests', 'lab', 'captures')
    ok, n, per = True, 0, {}
    for rel, ldw in CORPUS:
        rows = score(os.path.join(root, rel), ldw)
        print(rel)
        show(rows)
        for r in rows:
            n += r['n']
            k = (r['ldw'], r['rel_first'])
            per[k] = per.get(k, 0) + r['n']
            good = (r['ldw'] is not None and r['rel_first'] == r['rel_last'] == rule(r['ldw'])
                    and r['closed'] and r['held'])
            ok = ok and good
    print('records: %d, by (LOCKDIRWT, release): %r' % (n, per))
    ok = ok and per == WANT
    print('rebuild_step selftest: %s' % ('PASS' if ok else 'FAIL'))
    return 0 if ok else 1


def main(argv):
    if len(argv) == 3 and argv[1] == '--selftest':
        return selftest(argv[2])
    if len(argv) < 2:
        print(__doc__)
        return 2
    ldw = dict(a.lower().replace(':', '').split('=') for a in argv[2:])
    ldw = {k: int(v) for k, v in ldw.items()}
    show(score(argv[1], ldw))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
