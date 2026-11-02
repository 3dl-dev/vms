#!/usr/bin/env python3
"""dlm_dirreg_census.py -- what a real cat-0x02 op-0x0d record carries (rd vms-20c).

usage: dlm_dirreg_census.py <pcap|dir> ...        the census
       dlm_dirreg_census.py --selftest <repo>     pin the headline figures

WHY THIS EXISTS. rd vms-20c (FC-P5.5) was written on the premise that op-0x0d
is a LOCK-STATE record -- that a survivor describes each lock it holds (mode,
its lock id, its CSID, the master's lock id, a value block) to a new master in
one. The signed design left those offsets "to be pinned by a twin test against
the 307 captured frames". This census is that test's evidence, and it refutes
the premise: across every committed capture, a real VMS sender WRITES only

    body[0:8]        the envelope (send/ack message numbers, transaction)
    body[8:10]       02 0d
    body[12:16]      0x0001 + the cluster DLM rebuild sequence (rd vms-64b)
    body[44:48+L]    the resource identity: UIC group, access mode, length, name
    body[128:132]    the directory hash OF THAT IDENTITY

and every other byte is whatever the message buffer held before. The proof is
the BUFFER SLOT: a sender reuses a small set of message buffers, one per open
transaction (body[4:6]), and between two consecutive uses of the same slot
nothing outside the set above changes. So op-0x0d is the MASTER REGISTERING a
root resource with that resource's DIRECTORY node (rd vms-8219 measured the
same on a different topology) and it carries no lock-holder fields at all.

The hash check uses tools/cluster/dlm_hash/refhash.py, which is pinned to the
product's C function (src/kernel-core/vms_dlm_hash.c) by
tools/cluster/dlm_hash/check_heldout_run.py. It makes no claim about what any
residue byte means; it shows only that the sender did not write it.

Clean-room: reads only our own lab captures. No VMS binary or source.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'dlm_hash'))
import remaster_census as rc  # noqa: E402
import refhash  # noqa: E402

OP_REBUILD = 0x0d
CAT_REQ, CAT_ANS = 0x02, 0x82
STAMP_OFF = 34                 # the answer's result stamp (spec sec 4(p))
REJOIN_PCAP = os.path.join('tests', 'lab', 'captures',
                           'vms-20c-rebuild-1789489597', 'rejoin-rebuild.pcap')


def ident(body):
    """(group, mode, name) off body[44:48+L], or None for a length outside 1..31."""
    n = body[47]
    if not 1 <= n <= 31:
        return None
    return (int.from_bytes(body[44:46], 'little'), body[46], bytes(body[48:48 + n]))


def written_set(body):
    """The offsets a real sender writes in an op-0x0d (this file's header)."""
    n = body[47]
    s = set(range(0, 16)) | {STAMP_OFF} | set(range(44, 48 + n)) | set(range(128, 132))
    # body[10:12] and body[16:...] are NOT in it; [8:10] and [12:16] are.
    return s - {10, 11}


def records(path):
    """(idx, src6, dst6, body) for every cat-0x02 op-0x0d with body[12:14] = 1."""
    for idx, src, dst, body in rc.sca_bodies_with_src(path):
        if body[8] == CAT_REQ and body[9] == OP_REBUILD and body[12:14] == b'\x01\x00':
            yield idx, src, dst, bytes(body)


def measure(paths):
    m = collections.Counter()
    per_cap = collections.Counter()
    residue_change = collections.Counter()
    senders, receivers = set(), set()
    for p in paths:
        recs = list(records(p))
        if not recs:
            continue
        per_cap[os.path.basename(p)] += len(recs)
        answers = set()
        for _i, src, dst, body in rc.sca_bodies_with_src(p):
            if body[8] == CAT_ANS and body[9] == OP_REBUILD:
                answers.add((dst, src, bytes(body[4:8])))
        last = {}
        reused = set()      # slots another message from that sender used since
        for _i, src, dst, b in rc.sca_bodies_with_src(p):
            if not (b[8] == CAT_REQ and b[9] == OP_REBUILD and b[12:14] == b'\x01\x00'):
                for k in [k for k in last if k[0] == src and k[2] == b[4:6]]:
                    reused.add(k)
                continue
            b = bytes(b)
            m['records'] += 1
            senders.add(src)
            receivers.add(dst)
            idn = ident(b)
            if idn is None:
                m['bad_len'] += 1
                continue
            g, mode, name = idn
            if refhash.name_hash(name, mode, g) == int.from_bytes(b[128:132], 'little'):
                m['hash_of_identity'] += 1
            if (src, dst, b[4:8]) in answers:
                m['answered'] += 1
            m['group_nonzero'] += g != 0
            m['mode_%d' % mode] += 1
            slot = (src, dst, b[4:6])
            if slot in last:
                prev = last[slot]
                ws = written_set(b)
                moved = [o for o in range(rc.BODY_LEN) if o not in ws and b[o] != prev[o]]
                m['slot_pairs'] += 1
                if not moved:
                    m['slot_pairs_residue_unchanged'] += 1
                elif slot in reused:
                    m['slot_pairs_moved_after_other_use'] += 1
                else:
                    m['slot_pairs_moved_no_visible_use'] += 1
                for o in moved:
                    residue_change[o] += 1
            last[slot] = b
            reused.discard(slot)
    m['captures'] = len(per_cap)
    m['senders'] = len(senders)
    m['receivers'] = len(receivers)
    m['receivers_ovmx'] = sum(1 for r in receivers if r[:3] == b'\x52\x54\x00')
    return m, per_cap, residue_change


def rejoin_figures(path):
    """The vms-20c rejoin capture's own figures: one sender, one receiver."""
    f = collections.Counter()
    recs = list(records(path))
    f['records'] = len(recs)
    if not recs:
        return f
    src, dst = recs[0][1], recs[0][2]
    f['one_pair'] = int(all(r[1] == src and r[2] == dst for r in recs))
    f['slots'] = len({r[3][4:6] for r in recs})
    f['distinct_identities'] = len({ident(r[3]) for r in recs})
    f['dlm_seq_4'] = sum(1 for r in recs if r[3][14:16] == b'\x04\x00')
    # Directory addressing: entry[(hash >> 16) mod n]; in a two-member vector
    # every record to ONE receiver lands on ONE parity.
    f['hash_parity_odd'] = sum(1 for r in recs
                               if (int.from_bytes(r[3][128:132], 'little') >> 16) % 2)
    names = {ident(r[3])[2] for r in recs}
    last_idx = recs[-1][0]
    for idx, s, d, body in rc.sca_bodies_with_src(path):
        if idx <= last_idx or body[8] != CAT_REQ or body[9] != 0x01:
            continue
        n = body[47]
        if not 1 <= n <= 31 or bytes(body[48:48 + n]) not in names:
            continue
        if s == dst and d == src:
            f['later_op01_receiver_to_sender'] += 1
        elif s == src and d == dst:
            f['later_op01_sender_to_receiver'] += 1
    return f


def report(paths):
    m, per_cap, rch = measure(paths)
    for k in sorted(m):
        print('%-32s %d' % (k, m[k]))
    print('per capture:')
    for k, v in sorted(per_cap.items()):
        print('  %5d  %s' % (v, k))
    print('residue offsets that changed between two uses of one slot:')
    print('  ' + ' '.join('%d:%d' % kv for kv in sorted(rch.items())))


def selftest(repo):
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r%s' % ('PASS' if good else 'FAIL', name, got,
                                   '' if good else ' (want %r)' % (want,)))

    print('-- the vms-20c rejoin capture (VAX1 survivor -> VAX2 rejoiner)')
    f = rejoin_figures(os.path.join(repo, REJOIN_PCAP))
    check('op-0x0d records', f['records'], 307)
    check('all from one sender to one receiver', f['one_pair'], 1)
    check('distinct transaction slots body[4:6]', f['slots'], 6)
    check('distinct identities (no record repeats)', f['distinct_identities'], 307)
    check('body[14:16] == 4 (rebuild sequence)', f['dlm_seq_4'], 307)
    check('(hash >> 16) odd: one directory entry of two', f['hash_parity_odd'], 307)
    check('later op-0x01 receiver->sender naming a registered resource',
          f['later_op01_receiver_to_sender'], 223)
    check('later op-0x01 sender->receiver naming one', f['later_op01_sender_to_receiver'], 0)
    m, _pc, _r = measure([os.path.join(repo, REJOIN_PCAP)])
    check('records answered 82/0d', m['answered'], 307)
    check('body[128:132] == hash(group, mode, name)', m['hash_of_identity'], 307)
    check('consecutive same-slot pairs', m['slot_pairs'], 301)
    check('... whose residue is byte-for-byte unchanged', m['slot_pairs_residue_unchanged'], 301)

    print('-- the whole committed corpus (topology varies: 2- and 3-node,')
    print('   VAX->VAX, VAX->OVMX joiner, several senders)')
    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    m, per_cap, rch = measure(paths)
    check('captures carrying op-0x0d', m['captures'], CORPUS['captures'])
    check('records', m['records'], CORPUS['records'])
    check('name length outside 1..31', m['bad_len'], 0)
    check('body[128:132] == hash(group, mode, name)', m['hash_of_identity'], m['records'])
    check('distinct senders', m['senders'], CORPUS['senders'])
    check('distinct receivers', m['receivers'], CORPUS['receivers'])
    check('receivers that are OVMX', m['receivers_ovmx'], CORPUS['receivers_ovmx'])
    check('consecutive same-slot pairs', m['slot_pairs'], CORPUS['slot_pairs'])
    check('... residue unchanged', m['slot_pairs_residue_unchanged'],
          CORPUS['slot_pairs_residue_unchanged'])
    check('... moved, the slot having carried another message in between',
          m['slot_pairs_moved_after_other_use'], CORPUS['moved_after_other_use'])
    check('... moved with no intervening frame in the capture',
          m['slot_pairs_moved_no_visible_use'], CORPUS['moved_no_visible_use'])
    check('distinct mode values (kernel/executive/user)',
          sorted(k for k in m if k.startswith('mode_')), ['mode_0', 'mode_1', 'mode_3'])
    check('records with a non-zero UIC group', m['group_nonzero'], CORPUS['group_nonzero'])
    print('PASS' if ok else 'FAIL')
    return 0 if ok else 1


# MEASURED over tests/lab/captures (.pcap and .pcap.gz). A corpus change has to
# move these deliberately, and whatever quotes them must move with them.
# The 118 "no intervening frame" pairs are 117 in oracle-3node-fault-f1 plus one
# in ovmx-B3-7-s8: the changed bytes carry other DLM messages' shapes (a grant
# record's 0x10 ... 0xfa, a directory answer's 0xf9), i.e. the slot carried a
# message this capture did not see -- not a field the op-0x0d writes.
CORPUS = {
    'captures': 23, 'records': 16800, 'senders': 15, 'receivers': 16,
    'receivers_ovmx': 6, 'slot_pairs': 16406,
    'slot_pairs_residue_unchanged': 16147, 'moved_after_other_use': 141,
    'moved_no_visible_use': 118, 'group_nonzero': 63,
}


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else '.')
    report(rc.walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
