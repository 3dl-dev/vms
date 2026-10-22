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
import gzip
import os
import struct
import sys

SCA_ETHERTYPE = b'\x60\x07'
SCA_FRAME_LEN = 204            # the VMS_FCLS_SCS_MSG frame class
BODY_OFF = 72                  # abs 72 == body[0]  (spec sec 4(j))
BODY_LEN = 132
RM_STEPS = (0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14)
ANSWER_OPS = (0x01, 0x15)      # 82/01 answers a 0x12; 82/15 answers the rest


def read_capture(path):
    """The whole capture file's bytes, gzipped or not (rd vms-e80).

    28 of tests/lab/captures' capture files are `.pcap.gz`, and a census that
    reads only `.pcap` reads four fifths of the corpus. The compression is a
    property of the FILE, not of the frames: the same pcap bytes come back
    either way, so every reader below is unchanged.
    """
    try:
        if path.endswith('.gz'):
            with gzip.open(path, 'rb') as fp:
                return fp.read()
        with open(path, 'rb') as fp:
            return fp.read()
    except (OSError, EOFError, gzip.BadGzipFile):
        return b''


def sca_frames(path):
    """Yield (frame_index, whole_frame) for every SCS-MSG frame in a capture."""
    data = read_capture(path)
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
        yield idx, frame


def sca_bodies(path):
    """Yield (frame_index, src_tail, dst_tail, body) for every SCS-MSG frame."""
    for idx, frame in sca_frames(path):
        yield idx, frame[6:12].hex()[-4:], frame[0:6].hex()[-4:], frame[BODY_OFF:]


def sca_bodies_with_src(path):
    """The same frames as sca_bodies, with the WHOLE six-byte addresses.

    sca_bodies abbreviates them to the last two bytes, which identifies a node
    within one lab but throws away the OUI -- and the OUI is what tells a real
    DEC interface from an OVMX one (dlm_deq_census.py). Same discriminators,
    same body slice; only the address width differs.
    """
    for idx, frame in sca_frames(path):
        yield idx, frame[6:12], frame[0:6], frame[BODY_OFF:]


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


CAPTURE_SUFFIXES = ('.pcap', '.pcap.gz')


def walk_inputs(args):
    """Expand files and directories into a sorted list of capture paths.

    Both `.pcap` and `.pcap.gz` (rd vms-e80): a capture does not stop being
    evidence because it was compressed to fit in the repository.
    """
    out = []
    for a in args:
        if os.path.isdir(a):
            for root, _dirs, files in os.walk(a):
                out += [os.path.join(root, f) for f in files
                        if f.endswith(CAPTURE_SUFFIXES)]
        elif a.endswith(CAPTURE_SUFFIXES):
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


def _step_span(e):
    """(first, last) frame index of an episode's own STEPS (op-0x12 excluded)."""
    idx = [i for i, o, _s, _d in e['seq'] if o != 0x12]
    return min(idx), max(idx)


def attributable_episodes(eps):
    """The episodes an op-0x12 record can be ATTRIBUTED to (rd vms-e80).

    role_episodes collects an op-0x12 by its DESTINATION and frame span, because
    a SUB-resource record names its own resource and cannot be matched to a root
    name. In a busy capture several trees remaster onto the SAME new master at
    once, so those spans overlap and each episode collects the others' records.
    An episode is attributable only when no other episode in the same capture
    has the same new master and an overlapping span. Rule W2 is measured on
    these; over the whole corpus the figure is a property of the grouping, not
    of the protocol.
    """
    out = []
    for e in eps:
        lo, hi = _step_span(e)
        if any(f is not e and f['cap'] == e['cap'] and f['N'] == e['N']
               and not (_step_span(f)[1] < lo or _step_span(f)[0] > hi)
               for f in eps):
            continue
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

    def exact_ratio(what, hits, total, want_hits, want_total):
        """A rule with MEASURED exceptions: both halves are pinned, so any
        drift in either reddens -- the same strength as exact_all, without
        asserting an N-of-N the corpus falsifies (rd vms-e80)."""
        if (hits, total) != (want_hits, want_total):
            bad.append('%s: %d of %d, and the measured figure is %d of %d'
                        % (what, hits, total, want_hits, want_total))

    floor('02/0e -> 82/15 pairs', counts['0e/15'], 808)
    floor('02/0f -> 82/15 pairs', counts['0f/15'], 1195)
    floor('02/10 -> 82/15 pairs', counts['10/15'], 798)
    floor('02/14 -> 82/15 pairs', counts['14/15'], 1174)
    floor('02/12 -> 82/01 pairs', counts['12/01'], 3521)
    floor('02/11 notifications', sum(1 for op, _b in notifs if op == 0x11), 386)
    floor('02/13 notifications', sum(1 for op, _b in notifs if op == 0x13), 1180)

    # RULE 1: the 82/15 answer sets body[8], body[9], body[34] and -- for an
    # op-0x0f answered by the new master -- body[28:32].
    acked = [(q, a) for (s, o, q, a) in pairs if o == 0x15]
    exact_all('82/15 answers with body[34] == 0xfa',
              sum(1 for _q, a in acked if a[34] == 0xfa), len(acked))
    exact_all('82/15 answers with body[8:10] == 82 15',
              sum(1 for _q, a in acked if a[8] == 0x82 and a[9] == 0x15), len(acked))
    # ...AND THE REST OF THE BODY IS NOT AN ECHO, which is what the widened
    # corpus settled (rd vms-e80). Over the plain-.pcap view every one of 3,527
    # answers differed from its request in ONLY those cells, so the rule was
    # stated as an echo. Over the whole corpus 448 of 3,975 differ elsewhere --
    # 445 of them op-0x14 answers, all inside two captures -- and the differing
    # bytes carry unrelated message text (one answer's name span reads
    # ":39.50  ", another's "MSCP$SIGNAL"), i.e. the answering node's own send
    # buffer. The echo is what a reused receive buffer looks like, not a
    # protocol invariant. OVMX's builder echoes, which is the commonest real
    # form (3,527 of 3,975); what a RECIPIENT may read is only the cells above.
    stray = [(q, a) for q, a in acked
             if any(q[i] != a[i] for i in range(4, BODY_LEN)
                    if i not in (8, 9, 34) and not 28 <= i < 32)]
    exact_ratio('82/15 answers that differ from the request ONLY in body[8], '
                '[9], [28:32] and [34]',
                len(acked) - len(stray), len(acked), 3553, 4001)
    exact_ratio('...of those, the ones answering an op-0x14 (the other 3 answer '
                'an op-0x10)',
                sum(1 for q, _a in stray if q[9] == 0x14), len(stray), 445, 448)
    f0 = [(q, a) for (s, o, q, a) in pairs if s == 0x0f and o == 0x15]
    floor('op-0x0f answers that CLEAR body[28:32] (the new-master role)',
          sum(1 for _q, a in f0 if a[28:32] == b'\0\0\0\0'), 811)
    exact_all('real op-0x0f requests carrying 01 00 .. 00 at body[32:36] '
              '(why the narrowed answer builder is byte-identical there)',
              sum(1 for q, _a in f0 if q[32] == 0x01 and q[33] == 0 and q[35] == 0),
              len(f0))

    # RULE 2: the 82/01 answer to an op-0x12 is the grant record.
    g = [(q, a) for (s, o, q, a) in pairs if s == 0x12 and o == 0x01]
    # The two cells that hold over every one of the 3,521 pairs the widened
    # corpus carries, and the ones OVMX's answer builder asserts.
    exact_all('82/01 answers with body[34] == 0xfa (granted)',
              sum(1 for _q, a in g if a[34] == 0xfa), len(g))
    exact_all('82/01 answers that ECHO body[24:28] (the holder\'s own handle)',
              sum(1 for q, a in g if q[24:28] == a[24:28]), len(g))
    # body[28], the grant flag: 12 exceptions, all in ONE capture, the arm whose
    # own RESULTS.md records a node wedged at 87% CPU and removed by the
    # survivors (vms-cab-op0f-20261010/rule2/heldout-E4). 0x14 x8 and 0x18 x4 --
    # 0x14 being the QUEUED form dlm_queued_answer_census measures, so an answer
    # to an op-0x12 is not unconditionally a GRANT. What selects it is NOT
    # guessed here (rd vms-dd87).
    exact_ratio('82/01 answers with body[28] == 0x10 (the grant flag)',
                sum(1 for _q, a in g if a[28] == 0x10), len(g), 3509, 3521)
    subs = [(q, a) for q, a in g if not is_root_record(q)]
    exact_ratio('82/01 answers to a SUB record with body[30:32] == 0 0',
                sum(1 for _q, a in subs if a[30] == 0 and a[31] == 0),
                len(subs), 2549, 2729)
    exact_all('...and every exception carries 02 00 there, in one capture',
              sum(1 for _q, a in subs
                  if (a[30], a[31]) in ((0, 0), (2, 0))), len(subs))
    roots = [(q, a) for q, a in g if is_root_record(q)]
    floor('82/01 answers to a ROOT record (the unnamed body[30:32] cell)',
          len(roots), 792)
    # The ROOT form, now measured 88x over: f1 01 x698, f0 01 x88, and SIX
    # others (f2 01 x1, 05 02 x4, 04 00 x1) in the same two distressed
    # captures. The codec header named only f0 01 / f1 01 off 9 records.
    root_forms = collections.Counter(bytes(a[30:32]) for _q, a in roots)
    exact_ratio('ROOT 82/01 answers carrying f0 01 or f1 01 at body[30:32]',
                root_forms[b'\xf0\x01'] + root_forms[b'\xf1\x01'], len(roots),
                786, 792)
    exact_all('ROOT 82/01 answers whose body[31] is 01 or whose body[30:32] is '
              'one of the three measured oddities',
              sum(1 for _q, a in roots
                  if a[31] == 0x01 or bytes(a[30:32]) in (b'\x05\x02', b'\x04\x00')),
              len(roots))
    floor('82/01 answers carrying 0x0001 at body[32:34] (what the codec writes)',
          sum(1 for _q, a in g if a[32] == 0x01 and a[33] == 0), 1302)
    # rd vms-ed5: the answer's value-block span IS the record's own, ROOT and
    # sub alike -- which is what makes it a read-back of the RSB the adoption
    # engine filled and not an echo. 185 exceptions, in three captures.
    exact_ratio("82/01 answers whose body[36:56] is the record's body[88:108]",
                sum(1 for q, a in g if q[88:108] == a[36:56]), len(g),
                3336, 3521)
    # ...and the 15 records whose value block and sequence disagree all
    # disagree the SAME way: a sequence with no value block behind it.
    exact_ratio('op-0x12 records whose value block and sequence agree on being '
                'present (neither is read as an assertion about the other)',
                sum(1 for q, _a in g
                    if (q[88:104] != b'\0' * 16) == (q[104:108] != b'\0' * 4)),
                len(g), 3506, 3521)
    exact_all('...and every disagreement is a SEQUENCE with no value block',
              sum(1 for q, _a in g
                  if (q[88:104] != b'\0' * 16) == (q[104:108] != b'\0' * 4)
                  or q[88:104] == b'\0' * 16), len(g))

    # rd vms-ed5 RULES W1/W2: WHEN the new master sends its op-0x14s.
    eps = role_episodes(paths)
    rt = role_tally(eps)
    floor('remaster episodes that reach an op-0x14', rt['episodes'], 794)
    # W1's load-bearing half -- never an op-0x14 to a system that sent no
    # op-0x13 -- holds over all 1,174 op-0x14s, and that is the half OVMX's arm
    # implements. The other direction has 3 exceptions, all in the SHUTDOWN arm
    # (heldout-E4) and all the same departing system: it sent its op-0x13 and
    # had stopped transmitting (its last frame is 47144) before the three
    # op-0x14s went out at 47155-47160. The recipients are the op-0x13 senders
    # STILL IN THE CLUSTER.
    if rt['op14_without_a_13']:
        bad.append('%d op-0x14(s) went to a system that had sent no op-0x13 '
                   '(rule W1)' % rt['op14_without_a_13'])
    exact_ratio('episodes whose op-0x13 SENDERS are exactly its op-0x14 '
                'RECIPIENTS (rule W1)', rt['sets_equal'], rt['episodes'],
                795, 798)
    floor('episodes with more than one op-0x13', rt['multi13'], 383)
    exact_all('multi-op-0x13 episodes whose op-0x14s ALL follow the LAST '
              'op-0x13 (rule W2)', rt['multi13_batched'], rt['multi13'])
    # W2's "no op-0x12 after the last op-0x13" is measured on the episodes an
    # op-0x12 can be ATTRIBUTED to. role_episodes collects a record by its
    # destination and frame span, so where several trees remaster onto one new
    # master at once each episode collects the others' records -- 172 of 794
    # episodes "carry a late record" for that reason alone, which is a property
    # of the grouping. On the 31 attributable episodes the rule holds exactly.
    at = role_tally(attributable_episodes(eps))
    floor('episodes an op-0x12 can be attributed to', at['episodes'], 31)
    exact_all('attributable episodes with NO op-0x12 after the last op-0x13 '
              '(rule W2)', at['episodes'] - at['record_after_last_13'],
              at['episodes'])
    exact_ratio('episodes with a late op-0x12 once the overlapping ones are '
                'counted too (the grouping, not the protocol)',
                rt['record_after_last_13'], rt['episodes'], 172, 798)

    # RULE 3: the op-0x14's body[20:40] is residue.
    c = classify_op14_residue(pairs)
    floor('op-0x14 frames', c['total'], 1174)
    floor('op-0x14s whose body[20:40] is a stale GRANT record',
          c['stale_grant_record'], 72)
    floor('op-0x14s whose body[20:40] is a stale DIRECTORY answer',
          c['stale_directory_answer'], 36)
    floor('... of those, with a VAX S0 kernel address at body[29:32]',
          c['with_vax_s0_kernel_address'], 23)
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
