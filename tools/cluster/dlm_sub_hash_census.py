#!/usr/bin/env python3
"""dlm_sub_hash_census.py -- THE VALUE A SUB-LOCK REQUEST CARRIES AT
body[128:132], and what it is a function of (rd vms-d63, the r5 lab finding).

usage: dlm_sub_hash_census.py <pcap|dir> ...     the census
       dlm_sub_hash_census.py --selftest <repo>  pin the figures

WHY THIS EXISTS. vms-5fa r5 (tests/lab/captures/vms-5fa-shared-volume-r5-
20261103): OVMX's F11B$s<12> sub-request reached VAX1, the tree's master,
with the right parent pair and flags, and VAX1 granted every one of 161 with
a (0,0) value block and vbseq 0 -- a resource it had just made -- while
VAX1's own F11B$s<12> RSB under the same F11B$vEVAC5FA parent never held
OVMX's lock (SDA, 0 of 600 samples). OVMX's root requests, which carry the
directory hash, DO join VAX1's existing RSB (its mount lock is on 879E7A00).
Its sub requests carried ZERO at body[128:132]; VAX2's carry a value.

WHAT THE VALUE IS. Black-box from the (identity, value) pairs real VAX nodes
put on the wire in the clear (Rule 8, the same evidence class as the root
hash in src/kernel-core/vms_dlm_hash.h):

    sub = ((fold(sub identity) ^ (parent_value >> 16)) * 0xA53F19B7) mod 2^32

where fold() is the root function's rotate-and-XOR fold over the identity
block body[44:48+n], and parent_value is the value the PARENT's own request
carried. The census pairs every real op-0x01 sub request with its parent's
request (same sender, parent's own handle body[36:40] == the parent request's
body[24:28]) and checks the formula; it does the same for the op-0x12 / op-
0x08 SUB records, which carry the same value.

WHAT IT IS NOT: a function of the name alone. The same names S0/S1/S2 carry
different values in two arms (rd vms-b67a) because their parents differ --
the census pins that both arms match the formula with their own parents.

CLEAN-ROOM (Rule 8): reads captured frames only. No VSI/HPE source or binary.
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402
import f11b_lock_census as fc  # noqa: E402
import f11b_ser_valblk_census as sv  # noqa: E402

M32 = 0xffffffff
MULT = 0xA53F19B7
ROT = 9


def rotl(x, n):
    n %= 32
    return ((x << n) | (x >> (32 - n))) & M32


def fold(ident):
    """The root fold over an identity block (body[44:48] + the name)."""
    n = ident[3]
    blk = bytes(ident[:4 + n]) + bytes((-n) % 4)
    acc = 0
    for i in range(0, len(blk), 4):
        acc = rotl(acc ^ int.from_bytes(blk[i:i + 4], 'little'), ROT)
    return acc


def root_hash(ident):
    return (fold(ident) * MULT) & M32


def sub_hash(parent_value, ident):
    return ((fold(ident) ^ (parent_value >> 16)) * MULT) & M32


class Census:
    def __init__(self):
        self.root = collections.Counter()     # True/False: root formula holds
        self.sub = collections.Counter()      # (op, matched)
        self.unresolved = 0
        self.captures = set()
        self.pairs = set()                    # distinct (parent, sub) idents
        self.vectors = {}                     # name -> (parent ident, value)

    def walk(self, path):
        own = {}  # (sender, own handle) -> the value its request carried
        for _idx, src, _dst, fr in sv.frames(path):
            if not sv.is_dlm(fr) or not fc.is_real(src):
                continue
            b = fr[72:]
            if b[8] != 0x02 or b[9] not in (0x01, 0x08, 0x12):
                continue
            n = b[47]
            if n < 1 or n > 31:
                continue
            ident = bytes(b[44:48 + n])
            val = int.from_bytes(b[128:132], 'little')
            if b[36:44] == bytes(8):
                if b[9] == 0x01:
                    self.root[root_hash(ident) == val] += 1
                own[(src, bytes(b[24:28]))] = (ident, val)
                continue
            par = own.get((src, bytes(b[36:40])))
            if par is None:
                self.unresolved += 1
                continue
            ok = sub_hash(par[1], ident) == val
            self.sub[(b[9], ok)] += 1
            if b[9] == 0x01:
                self.captures.add(path)
                self.pairs.add((par[0], ident))
                self.vectors.setdefault(ident[4:], (par[0][4:], par[1], val))


def report(paths):
    c = Census()
    for p in paths:
        c.walk(p)
    print('root op-0x01 requests, root formula: %d match, %d do not'
          % (c.root[True], c.root[False]))
    for op in (0x01, 0x12, 0x08):
        print('SUB op-0x%02x: %d match the sub formula, %d do not'
              % (op, c.sub[(op, True)], c.sub[(op, False)]))
    print('distinct (parent, sub) identities %d over %d captures; '
          '%d subs whose parent request is not in the capture'
          % (len(c.pairs), len(c.captures), c.unresolved))
    return c


def selftest(repo):
    state = {'ok': True}

    def check(name, good, got):
        state['ok'] = state['ok'] and bool(good)
        print('  [%s] %s: %r' % ('PASS' if good else 'FAIL', name, got))

    # The function on the vms-5fa r5 frames themselves (f11b-frames.txt):
    # VAX2's F11B$s<42> (frame 11) sits under F11B$vSYSDSK1 and carries
    # d7 68 a6 0b; OVMX's F11B$vEVAC5FA root (frame 3493) carries 5f 1e 26 a0.
    sysdsk1 = bytes([0, 0, 0, 18]) + b'F11B$vSYSDSK1     '
    evac = bytes([0, 0, 0, 18]) + b'F11B$vEVAC5FA     '
    s42 = bytes([0, 0, 0, 10]) + b'F11B$s' + bytes([42, 0, 0, 0])
    check('r5 frame 3493: OVMX root F11B$vEVAC5FA = 0xa0261e5f',
          root_hash(evac) == 0xa0261e5f, hex(root_hash(evac)))
    check('r5 frame 11: VAX2 F11B$s<42> under F11B$vSYSDSK1 = 0x0ba668d7',
          sub_hash(root_hash(sysdsk1), s42) == 0x0ba668d7,
          hex(sub_hash(root_hash(sysdsk1), s42)))

    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    c = report(paths)
    check('the root formula: every real root request (floor), no miss',
          c.root[True] >= 11000 and c.root[False] == 0, dict(c.root))
    check('op-0x01 SUB requests: the sub formula on all (floor), no miss',
          c.sub[(0x01, True)] >= 27659 and c.sub[(0x01, False)] == 0,
          (c.sub[(0x01, True)], c.sub[(0x01, False)]))
    check('...over many captures and arrangements (floor)',
          len(c.captures) >= 20 and len(c.pairs) >= 459,
          (len(c.captures), len(c.pairs)))
    check('op-0x12 / op-0x08 SUB records carry the same value, no miss',
          c.sub[(0x12, True)] >= 2736 and c.sub[(0x08, True)] >= 397 and
          c.sub[(0x12, False)] == 0 and c.sub[(0x08, False)] == 0,
          dict(c.sub))
    # rd vms-b67a: the same short names, different values, both explained
    b67a = os.path.join(repo, 'tests', 'lab', 'captures',
                        'vms-b67a-sublock-arm-20261022', 'sublock.pcap.gz')
    seen = {}
    for _i, src, _d, fr in sv.frames(b67a):
        b = fr[72:]
        if sv.is_dlm(fr) and fc.is_real(src) and b[8] == 2 and b[9] == 1:
            seen.setdefault(bytes(b[48:48 + b[47]]),
                            (bytes(b[44:48 + b[47]]),
                             int.from_bytes(b[128:132], 'little')))
    rmt9, s0 = seen.get(b'RMT9'), seen.get(b'S0')
    check('rd vms-b67a arm 2: S0 under RMT9 carries the sub formula\'s value '
          'of RMT9\'s own carried value',
          rmt9 is not None and s0 is not None and
          sub_hash(rmt9[1], s0[0]) == s0[1], s0 and hex(s0[1]))
    print('VERDICT: %s' % ('PASS' if state['ok'] else 'FAIL'))
    return 0 if state['ok'] else 1


def main(argv):
    if len(argv) >= 2 and argv[1] == '--selftest':
        return selftest(argv[2] if len(argv) > 2 else
                        os.path.join(HERE, '..', '..'))
    if len(argv) < 2:
        print(__doc__)
        return 2
    report(rc.walk_inputs(argv[1:]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
