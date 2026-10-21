#!/usr/bin/env python3
"""remaster_census.py -- the lock-tree remastering census (rd vms-380).

usage: remaster_census.py <pcap|dir> ...          the per-cell census
       remaster_census.py --selftest <repo-root>  pin the headline figures
       remaster_census.py --pairs <pcap> ...      emit request/answer pairs for
                                                  tests/cluster/host/score_remaster
       remaster_census.py --roles <pcap|dir> ...  the per-episode ROLE GRAPH:
                                                  who sent which step to whom

WHAT IT MEASURES. A real V7.3 lock-tree remaster is seven cat-0x02 steps
(0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14). This tool walks every SCA frame in
the given captures, pairs each step with its cat-0x82 answer BY THE SCS
TRANSACTION ID at body[4:8] -- not by time, not by order -- and reports, per
(step, answer) pair, which body offsets the answer rewrote and what it wrote
there. The three rules in vms_cluster_codec_dlm.h's "LOCK-TREE REMASTERING ON
THE WIRE" are the output of this tool, and `--selftest` is what keeps the
numbers quoted there from drifting away from the captures they came from.

WHY PAIR BY TRANSACTION. Concurrent remasters of different trees interleave,
and in a busy capture the same tree is remastered many times. Pairing a 0x14
with "the next 82/15" or deciding a node's role from "the 0x0e sender for that
tree name" both mislabel in those captures -- which is how a six-frame sample
produced the role rules the full corpus falsifies (see the op-0x14 note in the
codec header).

CLEAN-ROOM: reads captured frames only. No VSI/HPE source, no binaries, no
recomputation of an unpublished algorithm.
"""
import collections
import os
import struct
import sys

SCA_ETHERTYPE = b'\x60\x07'
SCA_FRAME_LEN = 204            # the VMS_FCLS_SCS_MSG frame class
BODY_OFF = 72                  # abs 72 == body[0]  (spec sec 4(j))
BODY_LEN = 132
RM_STEPS = (0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14)
ANSWER_OPS = (0x01, 0x15)      # 82/01 answers a 0x12; 82/15 answers the rest


def sca_bodies(path):
    """Yield (frame_index, src_tail, dst_tail, body) for every SCS-MSG frame."""
    try:
        with open(path, 'rb') as fp:
            data = fp.read()
    except OSError:
        return
    if len(data) < 24:
        return
    off, idx = 24, 0
    while off + 16 <= len(data):
        _ts, _us, caplen, _origlen = struct.unpack('<IIII', data[off:off + 16])
        off += 16
        frame = data[off:off + caplen]
        off += caplen
        idx += 1
        if len(frame) != SCA_FRAME_LEN or frame[12:14] != SCA_ETHERTYPE:
            continue
        # The SCS-MSG discriminators the codec's own classifier uses.
        if frame[31] != 0x13 or frame[55] != 0x02:
            continue
        yield idx, frame[6:12].hex()[-4:], frame[0:6].hex()[-4:], frame[BODY_OFF:]


def pair_one_capture(path):
    """Return (pairs, notifications) for one capture.

    pairs: list of (step_op, answer_op, request_body, answer_body)
    notifications: list of (step_op, request_body) -- body[4:8] == 0
    """
    pairs, notifs, pending = [], [], {}
    for _idx, src, dst, body in sca_bodies(path):
        cat, op = body[8], body[9]
        if cat == 0x02 and op in RM_STEPS:
            if body[4:8] == b'\0\0\0\0':
                notifs.append((op, bytes(body)))
            else:
                pending[(src, dst, bytes(body[4:8]))] = (op, bytes(body))
        elif cat == 0x82 and op in ANSWER_OPS:
            key = (dst, src, bytes(body[4:8]))
            if key in pending:
                step_op, req = pending.pop(key)
                pairs.append((step_op, op, req, bytes(body)))
    return pairs, notifs


def walk_inputs(args):
    """Expand files and directories into a sorted list of .pcap paths."""
    out = []
    for a in args:
        if os.path.isdir(a):
            for root, _dirs, files in os.walk(a):
                out += [os.path.join(root, f) for f in files if f.endswith('.pcap')]
        elif a.endswith('.pcap'):
            out.append(a)
    return sorted(out)


def collect(paths):
    pairs, notifs = [], []
    for p in paths:
        a, b = pair_one_capture(p)
        pairs += a
        notifs += b
    return pairs, notifs


def is_root_record(req):
    """An op-0x12 ROOT record carries no parent: body[36:44] is all zero."""
    return req[36:44] == b'\0' * 8


def answer_rewrites(pairs, step_op, answer_op, select=None):
    """Per offset: how many answers rewrote it, and what they wrote."""
    sel = [(q, a) for (s, o, q, a) in pairs
           if s == step_op and o == answer_op and (select is None or select(q))]
    changed = collections.Counter()
    wrote = collections.defaultdict(collections.Counter)
    for req, ans in sel:
        for i in range(4, BODY_LEN):          # body[0:4] is the CM's envelope
            wrote[i]['%02x' % ans[i]] += 1
            if req[i] != ans[i]:
                changed[i] += 1
    return len(sel), changed, wrote


def classify_op14_residue(pairs):
    """What the op-0x14's body[20:40] actually holds, per the rd vms-42de test
    for residue: does the span carry, verbatim, the record of another message
    type?"""
    f14 = [q for (s, _o, q, _a) in pairs if s == 0x14]
    stale_grant = [b for b in f14
                   if b[28] == 0x10 and b[29] == 0 and b[30] == 0 and
                   b[31] == 0 and b[32] == 0x01 and b[34] == 0xfa]
    stale_dir = [b for b in f14 if b[34] == 0xf9]
    kernel_addr = [b for b in stale_dir if b[31] == 0x87]
    return dict(total=len(f14), stale_grant_record=len(stale_grant),
                stale_directory_answer=len(stale_dir),
                with_vax_s0_kernel_address=len(kernel_addr),
                neither=len(f14) - len(stale_grant) - len(stale_dir))


def rm_ident(body):
    """The resource identity a remaster frame names, or None."""
    n = body[47]
    if n < 1 or n > 31:
        return None
    return (bytes(body[44:46]), body[46], bytes(body[48:48 + n]))


def role_episodes(paths):
    """Group the remaster steps of each capture into EPISODES and report, per
    episode, who played which part (rd vms-ed5 rules W1/W2).

    An episode is opened by an op-0x0e for a root identity and collects every
    later step naming that identity plus every op-0x12 addressed to its new
    master inside its index span -- a SUB-resource record names its OWN
    resource, so it cannot be grouped by the root name.

    Each episode is a dict: cap, name, O, N, and `seq`, a list of
    (frame_index, op, src, dst).
    """
    out = []
    for p in paths:
        rows = [(i, s, d, b) for (i, s, d, b) in sca_bodies(p)
                if b[8] == 0x02 and b[9] in RM_STEPS and rm_ident(b) is not None]
        cur, eps = {}, []
        for i, s, d, b in rows:
            k = rm_ident(b)
            if b[9] == 0x0e:
                e = dict(cap=os.path.basename(p), name=k[2].decode('latin1'),
                         O=s, N=d, seq=[])
                cur[k] = e
                eps.append(e)
            e = cur.get(k)
            if e is not None and b[9] != 0x12:
                e['seq'].append((i, b[9], s, d))
        for e in eps:
            if not any(op == 0x14 for _i, op, _s, _d in e['seq']):
                continue          # the hand-over never completed in this capture
            lo = e['seq'][0][0]
            hi = max(i for i, _o, _s, _d in e['seq'])
            e['seq'] += [(i, 0x12, s, d) for (i, s, d, b) in rows
                         if b[9] == 0x12 and d == e['N'] and lo <= i <= hi + 1]
            e['seq'].sort()
            out.append(e)
    return out


def role_tally(eps):
    """The W1/W2 figures over a set of episodes."""
    t = dict(episodes=len(eps), op14=0, op13=0, sets_equal=0,
             op14_without_a_13=0, record_after_last_13=0,
             multi13=0, multi13_batched=0)
    for e in eps:
        s13 = sorted({s for _i, o, s, _d in e['seq'] if o == 0x13})
        t14 = [(i, d) for i, o, _s, d in e['seq'] if o == 0x14]
        t['op13'] += sum(1 for _i, o, _s, _d in e['seq'] if o == 0x13)
        t['op14'] += len(t14)
        if s13 == sorted({d for _i, d in t14}):
            t['sets_equal'] += 1
        i13 = [(i, s) for i, o, s, _d in e['seq'] if o == 0x13]
        for i14, d14 in t14:
            if not any(i < i14 and s == d14 for i, s in i13):
                t['op14_without_a_13'] += 1
        last13 = max([i for i, _s in i13], default=-1)
        if any(o == 0x12 and i > last13 for i, o, _s, _d in e['seq']):
            t['record_after_last_13'] += 1
        if len(i13) > 1:
            t['multi13'] += 1
            if all(i14 > i13max for i14, _d in t14
                   for i13max in [max(i for i, _s in i13)]):
                t['multi13_batched'] += 1
    return t


def report_roles(paths):
    eps = role_episodes(paths)
    for e in eps:
        print('%-30s %-16s O=%s N=%s' % (e['cap'][:30], e['name'][:16],
                                         e['O'], e['N']))
        print('   ' + ' '.join('%d:%02x:%s>%s' % (i, o, s, d)
                               for i, o, s, d in e['seq']))
    print()
    for k, v in role_tally(eps).items():
        print('   %-26s %d' % (k, v))


def report(paths):
    pairs, notifs = collect(paths)
    counts = collections.Counter('%02x/%02x' % (s, o) for s, o, _q, _a in pairs)
    ncount = collections.Counter('%02x' % op for op, _b in notifs)
    print('captures: %d   answered pairs: %s   notifications: %s'
          % (len(paths), dict(sorted(counts.items())), dict(sorted(ncount.items()))))

    for step in (0x0e, 0x0f, 0x10, 0x14):
        n, changed, wrote = answer_rewrites(pairs, step, 0x15)
        print('\n=== 02/%02x -> 82/15   n=%d' % (step, n))
        for off in sorted(changed):
            print('   body[%3d] rewritten %3d/%3d   answer values %s'
                  % (off, changed[off], n, dict(wrote[off].most_common(4))))

    for label, sel in (('ROOT', is_root_record),
                       ('sub', lambda q: not is_root_record(q))):
        n, changed, wrote = answer_rewrites(pairs, 0x12, 0x01, sel)
        print('\n=== 02/12 -> 82/01 (%s records)   n=%d' % (label, n))
        for off in sorted(changed):
            print('   body[%3d] rewritten %3d/%3d   answer values %s'
                  % (off, changed[off], n, dict(wrote[off].most_common(4))))

    print('\n=== op-0x14 body[20:40], classified (rd vms-42de residue test)')
    for k, v in classify_op14_residue(pairs).items():
        print('   %-30s %d' % (k, v))


# ---------------------------------------------------------------------------
# --selftest: the figures vms_cluster_codec_dlm.h and the frame-grounding rows
# quote. A capture added to the tree can only RAISE these, so each is checked
# as a floor -- except the ones the codec states as "N of N", which must stay
# exactly equal or the rule they ground has an exception.
# ---------------------------------------------------------------------------
def selftest(root):
    paths = walk_inputs([os.path.join(root, 'tests', 'lab', 'captures')])
    pairs, notifs = collect(paths)
    counts = collections.Counter('%02x/%02x' % (s, o) for s, o, _q, _a in pairs)
    bad = []

    def floor(what, got, want):
        if got < want:
            bad.append('%s: %d, expected at least %d' % (what, got, want))

    def exact_all(what, hits, total):
        if hits != total:
            bad.append('%s: %d of %d, and the rule says all of them' % (what, hits, total))

    floor('02/0e -> 82/15 pairs', counts['0e/15'], 63)
    floor('02/0f -> 82/15 pairs', counts['0f/15'], 70)
    floor('02/10 -> 82/15 pairs', counts['10/15'], 61)
    floor('02/14 -> 82/15 pairs', counts['14/15'], 68)
    floor('02/12 -> 82/01 pairs', counts['12/01'], 420)
    floor('02/11 notifications', sum(1 for op, _b in notifs if op == 0x11), 7)
    floor('02/13 notifications', sum(1 for op, _b in notifs if op == 0x13), 68)

    # RULE 1: the 82/15 answer rewrites body[8], body[9], body[34] and -- for
    # an op-0x0f answered by the new master -- body[28:32]. Nothing else.
    acked = [(q, a) for (s, o, q, a) in pairs if o == 0x15]
    exact_all('82/15 answers with body[34] == 0xfa',
              sum(1 for _q, a in acked if a[34] == 0xfa), len(acked))
    exact_all('82/15 answers with body[8:10] == 82 15',
              sum(1 for _q, a in acked if a[8] == 0x82 and a[9] == 0x15), len(acked))
    stray = 0
    for q, a in acked:
        for i in range(4, BODY_LEN):
            if i in (8, 9, 34) or 28 <= i < 32:
                continue
            if q[i] != a[i]:
                stray += 1
    if stray:
        bad.append('82/15 answers rewrote %d byte(s) outside body[8], [9], '
                   '[28:32] and [34]' % stray)
    f0 = [(q, a) for (s, o, q, a) in pairs if s == 0x0f and o == 0x15]
    floor('op-0x0f answers that CLEAR body[28:32] (the new-master role)',
          sum(1 for _q, a in f0 if a[28:32] == b'\0\0\0\0'), 63)
    exact_all('real op-0x0f requests carrying 01 00 .. 00 at body[32:36] '
              '(why the narrowed answer builder is byte-identical there)',
              sum(1 for q, _a in f0 if q[32] == 0x01 and q[33] == 0 and q[35] == 0),
              len(f0))

    # RULE 2: the 82/01 answer to an op-0x12 is the grant record.
    g = [(q, a) for (s, o, q, a) in pairs if s == 0x12 and o == 0x01]
    exact_all('82/01 answers with body[28] == 0x10 (the grant flag)',
              sum(1 for _q, a in g if a[28] == 0x10), len(g))
    exact_all('82/01 answers with body[34] == 0xfa (granted)',
              sum(1 for _q, a in g if a[34] == 0xfa), len(g))
    exact_all('82/01 answers that ECHO body[24:28] (the holder\'s own handle)',
              sum(1 for q, a in g if q[24:28] == a[24:28]), len(g))
    subs = [(q, a) for q, a in g if not is_root_record(q)]
    exact_all('82/01 answers to a SUB record with body[30:32] == 0 0',
              sum(1 for _q, a in subs if a[30] == 0 and a[31] == 0), len(subs))
    roots = [(q, a) for q, a in g if is_root_record(q)]
    floor('82/01 answers to a ROOT record (the unnamed body[30:32] cell)',
          len(roots), 9)
    if any(a[30] not in (0xf0, 0xf1) or a[31] != 0x01 for _q, a in roots):
        bad.append('a ROOT 82/01 carries something other than f0 01 / f1 01 at '
                   'body[30:32] -- the codec header names only those two')
    floor('82/01 answers carrying 0x0001 at body[32:34] (what the codec writes)',
          sum(1 for _q, a in g if a[32] == 0x01 and a[33] == 0), 283)
    # rd vms-ed5: the answer's value-block span IS the record's own, ROOT and
    # sub alike -- which is what makes it a read-back of the RSB the adoption
    # engine filled and not an echo. An exception anywhere falsifies the rule.
    exact_all("82/01 answers whose body[36:56] is the record's body[88:108]",
              sum(1 for q, a in g if q[88:108] == a[36:56]), len(g))
    exact_all('op-0x12 records whose value block and sequence agree on being '
              'present (neither is read as an assertion about the other)',
              sum(1 for q, _a in g
                  if (q[88:104] != b'\0' * 16) == (q[104:108] != b'\0' * 4)),
              len(g))

    # rd vms-ed5 RULES W1/W2: WHEN the new master sends its op-0x14s.
    eps = role_episodes(paths)
    rt = role_tally(eps)
    floor('remaster episodes that reach an op-0x14', rt['episodes'], 61)
    exact_all('episodes whose op-0x13 SENDERS are exactly its op-0x14 '
              'RECIPIENTS (rule W1)', rt['sets_equal'], rt['episodes'])
    if rt['op14_without_a_13']:
        bad.append('%d op-0x14(s) went to a system that had sent no op-0x13 '
                   '(rule W1)' % rt['op14_without_a_13'])
    if rt['record_after_last_13']:
        bad.append('%d episode(s) carry an op-0x12 AFTER the last op-0x13 '
                   '(rule W2 assumes none)' % rt['record_after_last_13'])
    floor('episodes with more than one op-0x13', rt['multi13'], 7)
    exact_all('multi-op-0x13 episodes whose op-0x14s ALL follow the LAST '
              'op-0x13 (rule W2)', rt['multi13_batched'], rt['multi13'])

    # RULE 3: the op-0x14's body[20:40] is residue.
    c = classify_op14_residue(pairs)
    floor('op-0x14 frames', c['total'], 68)
    floor('op-0x14s whose body[20:40] is a stale GRANT record',
          c['stale_grant_record'], 23)
    floor('op-0x14s whose body[20:40] is a stale DIRECTORY answer',
          c['stale_directory_answer'], 26)
    floor('... of those, with a VAX S0 kernel address at body[29:32]',
          c['with_vax_s0_kernel_address'], 18)
    f14 = [q for (s, _o, q, _a) in pairs if s == 0x14]
    exact_all('op-0x14s with body[40:44] == 0',
              sum(1 for b in f14 if b[40:44] == b'\0\0\0\0'), len(f14))

    print('remaster_census --selftest: %d captures, pairs %s'
          % (len(paths), dict(sorted(counts.items()))))
    for b in bad:
        print('  FAIL', b)
    if bad:
        print('remaster_census --selftest: %d failure(s)' % len(bad))
        return 1
    print('remaster_census --selftest: every figure the codec quotes holds')
    return 0


def emit_pairs(paths):
    """One line per answered pair, for tests/cluster/host/score_remaster.c:

        <step-op-hex> <answer-op-hex> <request-body-hex> <answer-body-hex>

    The scorer is a C program that calls the COMMITTED builders, so a capture
    is scored by the shipping codec and not by this file's reading of it.
    """
    pairs, _notifs = collect(paths)
    for step, ans, req, abody in pairs:
        print('%02x %02x %s %s' % (step, ans, req.hex(), abody.hex()))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    if argv[1] == '--pairs':
        emit_pairs(walk_inputs(argv[2:]))
        return 0
    if argv[1] == '--roles':
        report_roles(walk_inputs(argv[2:]))
        return 0
    report(walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
