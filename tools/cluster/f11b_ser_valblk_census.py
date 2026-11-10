#!/usr/bin/env python3
"""f11b_ser_valblk_census.py -- WHAT THE XQP's PER-FILE SERIALIZATION LOCK
CARRIES IN ITS VALUE BLOCK, and how a real release WRITES it (rd vms-d63).

usage: f11b_ser_valblk_census.py <pcap|dir> ...     the census
       f11b_ser_valblk_census.py --selftest <repo>  pin the figures

WHY THIS EXISTS. On a volume a real VAX and OVMX both mount, OVMX appended to
EVAC.DAT and rewrote its header (the new EOF), and VAX1 then wrote ITS records
over OVMX's: VAX1's cached header never learned of the change
(tests/lab/captures/vms-5fa-shared-volume-r2-20261102). The XQP keeps a cached
header coherent with a lock per file -- 'F11B$s' + LE32(file number), a
sub-lock of 'F11B$v' + label (rd vms-f4c0's census) -- taken PW with
LCK$M_VALBLK and released with a value block that carries the file's change
counters: the lock-value-block cache-validation protocol of the OpenVMS
Programming Concepts Manual, applied to the file header. This tool measures,
from real VAX traffic only, the three facts OVMX's ACP needs to take part:

  (a) THE BLOCK'S SHAPE. On a real grant of an F11B$s lock, body[36:52] is
      two little-endian counters and eight zero bytes; each counter only ever
      moves UP between successive grants of a resource that persists, and a
      resource that was forgotten comes back all-zero and is initialised to
      (1,1) again -- the only way a counter is ever seen to go down.

  (b) WHICH COUNTER MEANS WHAT, read off one real sequence end to end
      (vms-b20-mscp-read-20261011/mscp-read-write.pcap, VAX2 COPYing a file
      onto VAX1's MSCP-served DATA2 -- the only capture whose MSCP writes and
      XQP locks share a wire): the first counter steps when the file's HEADER
      is written (the new file 13: allocation + header (0,0)->(2,1), then the
      close's header write (2,1)->(3,1)), the second when a DIRECTORY's
      contents change (the MFD, file 4, taking the new entry: (1,3)->(1,4)).

  (c) HOW A RELEASE WRITES IT. A real $DEQ (op 0x03) of an F11B$s lock carries
      the lock's 16-byte block at body[36:52]; when the release is marked 1 at
      body[30] the master STORES it (the next grant of the same resource hands
      it out), when it is marked 0 the master does not.

CLEAN-ROOM (Rule 8): reads captured frames only. No VSI/HPE source or binary.
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import remaster_census as rc  # noqa: E402
import f11b_lock_census as fc  # noqa: E402

SER_PREFIX = b'F11B$s'
SER_LEN = 10
B20 = os.path.join('tests', 'lab', 'captures', 'vms-b20-mscp-read-20261011',
                   'mscp-read-write.pcap')


def le32(b):
    return int.from_bytes(b, 'little')


def frames(path):
    """(record index, src, dst, whole frame) for every 0x6007 frame -- the
    SCS messages AND the 108-byte MSCP commands, which sca_frames skips."""
    data = rc.read_capture(path)
    off, idx = 24, 0
    while off + 16 <= len(data):
        _ts, _us, caplen, _ol = struct.unpack('<IIII', data[off:off + 16])
        off += 16
        fr = data[off:off + caplen]
        off += caplen
        idx += 1
        if len(fr) >= 14 and fr[12:14] == b'\x60\x07':
            yield idx, bytes(fr[6:12]), bytes(fr[0:6]), bytes(fr)


def is_dlm(fr):
    return (len(fr) >= 204 and fr[31] == 0x13 and fr[55] == 0x02 and
            (fr[80] & 0x7f) == 0x02)


def mscp_write_lbn(fr):
    """The LBN of an MSCP WRITE command (opcode at abs 80, LBN at abs 100)."""
    if len(fr) < 0x68 or len(fr) > 140 or fr[0x50] != 0x22:
        return None
    return le32(fr[0x64:0x68])


class Census:
    def __init__(self):
        self.grants = 0              # real F11B$s grants carrying a block
        self.tail_zero = 0           # ...with bytes 8..15 zero
        self.moved = collections.Counter()   # (counter, 'up'|'down')
        self.rel_nz = 0              # real F11B$s releases, block non-zero
        self.rel_eq = 0              # ...equal to the block the lock was granted
        self.rel_diff = 0            # ...different: the releases that WROTE
        self.applied = collections.Counter()  # (cell, stored?)

    def walk(self, path):
        pend, owner, gvb, last, wrote = {}, {}, {}, {}, {}
        for _idx, src, dst, fr in frames(path):
            if not is_dlm(fr):
                continue
            b = fr[72:]
            cat, op = b[8], b[9]
            if cat == 0x02 and op == 0x01:
                nm = fc.resname(b)
                if nm.startswith(SER_PREFIX) and len(nm) == SER_LEN:
                    pend[(src, dst, b[24:28])] = nm
            elif cat == 0x82 and op == 0x01:
                k = (dst, src, b[24:28])
                if k not in pend:
                    continue
                nm = pend.pop(k)
                key = (dst, src, b[20:24])       # requester, master, handle
                owner[key] = nm
                vb = b[36:52]
                gvb[key] = vb
                seq = le32(b[52:56])
                if fc.is_real(src) and seq != 0:
                    self.grants += 1
                    if vb[8:16] == bytes(8):
                        self.tail_zero += 1
                    res = (src, dst, nm)
                    prev = last.get(res)
                    if prev is not None and vb[8:16] == bytes(8) and \
                            prev[0][8:16] == bytes(8):
                        # The DLM's own value-block sequence going BACK means
                        # the master re-created the resource (a rebuild):
                        # its block started again from zero.
                        recreated = seq < prev[1]
                        for c in (0, 1):
                            a = le32(prev[0][4 * c:4 * c + 4])
                            n = le32(vb[4 * c:4 * c + 4])
                            if n > a:
                                self.moved[(c, 'up')] += 1
                            elif n < a and (n == 1 or recreated):
                                self.moved[(c, 'reinit')] += 1
                            elif n < a and n != 0:
                                self.moved[(c, 'down')] += 1
                    last[res] = (vb, seq)
                mres = (src, nm)                 # master, name
                if mres in wrote:
                    rvb, cell = wrote.pop(mres)
                    self.applied[(cell, vb == rvb)] += 1
            elif cat == 0x02 and op == 0x03 and fc.is_real(src):
                key = (src, dst, b[20:24])
                if key not in owner:
                    continue
                vb = b[36:52]
                if vb == bytes(16):
                    continue
                self.rel_nz += 1
                if vb == gvb[key]:
                    self.rel_eq += 1
                else:
                    self.rel_diff += 1
                    wrote[(dst, owner[key])] = (vb, b[30])


def b20_sequence(path):
    """The counters around each XQP lock cycle on file 13 and file 4 in the
    vms-b20 capture, with the header LBN writes between grant and release.
    Returns a list of (fid, granted block, released block, how, header-LBN
    written in between)."""
    pend, owner, open_ = {}, {}, {}
    out = []
    for _idx, src, dst, fr in frames(path):
        lbn = mscp_write_lbn(fr)
        if lbn is not None:
            for k in open_:
                if k[0] == src:
                    open_[k][2].append(lbn)
            continue
        if not is_dlm(fr):
            continue
        b = fr[72:]
        cat, op = b[8], b[9]
        if cat == 0x02 and op == 0x01:
            nm = fc.resname(b)
            if nm.startswith(SER_PREFIX) and len(nm) == SER_LEN:
                pend[(src, dst, b[24:28])] = nm
        elif cat == 0x82 and op == 0x01:
            k = (dst, src, b[24:28])
            if k in pend:
                nm = pend.pop(k)
                key = (dst, b[20:24])
                owner[key] = nm
                open_[key] = [le32(nm[6:10]), b[36:44], []]
        elif cat == 0x02 and op in (0x03, 0x06):
            key = (src, b[20:24])
            if key in open_:
                fid, gv, lbns = open_.pop(key)
                rv = b[36:44]
                if op == 0x03 and b[30] != 1:
                    continue                      # an unmarked release
                if gv != rv and fid in (4, 13):
                    out.append((fid, (le32(gv[0:4]), le32(gv[4:8])),
                                (le32(rv[0:4]), le32(rv[4:8])),
                                '$DEQ' if op == 0x03 else 'CONVERT',
                                445641 + fid in lbns))
    return out


def report(paths):
    c = Census()
    for p in paths:
        c.walk(p)
    print('F11B$s value-block census over %d captures' % len(paths))
    print('  grants carrying a block: %d, bytes 8..15 zero on %d'
          % (c.grants, c.tail_zero))
    print('  counter moves between successive grants: %s' % dict(c.moved))
    print('  releases with a non-zero body[36:52]: %d (equal to the grant %d,'
          ' different %d)' % (c.rel_nz, c.rel_eq, c.rel_diff))
    print('  writing releases whose next grant is on the wire: %s'
          % dict(c.applied))
    return c


def selftest(repo):
    state = {'ok': True}

    def check(name, good, got):
        state['ok'] = state['ok'] and bool(good)
        print('  [%s] %s: %r' % ('PASS' if good else 'FAIL', name, got))

    paths = rc.walk_inputs([os.path.join(repo, 'tests', 'lab', 'captures')])
    c = report(paths)
    # (a) the shape
    check('(a) real F11B$s grants carrying a block (floor)',
          c.grants >= 34000, c.grants)
    check('(a) bytes 8..15 zero on all but a handful (>= 99.8%)',
          c.tail_zero * 1000 >= c.grants * 998, (c.tail_zero, c.grants))
    check('(a) neither counter ever moves DOWN, except when the resource was '
          're-created (back to 1, or the DLM sequence restarted)',
          c.moved[(0, 'down')] == 0 and c.moved[(1, 'down')] == 0,
          dict(c.moved))
    check('(a) both counters do move up', c.moved[(0, 'up')] > 100 and
          c.moved[(1, 'up')] > 100, dict(c.moved))
    # (c) the release
    check('(c) real F11B$s releases carrying a block (floor)',
          c.rel_nz >= 34577, c.rel_nz)
    check('(c) ...the block the lock was GRANTED on most (>= 85%)',
          c.rel_eq * 100 >= c.rel_nz * 85, (c.rel_eq, c.rel_nz))
    check('(c) ...and a DIFFERENT block -- a write -- on the rest (floor)',
          c.rel_diff >= 3748, c.rel_diff)
    marked = c.applied[(1, True)] + c.applied[(1, False)]
    unmarked = c.applied[(0, True)] + c.applied[(0, False)]
    check('(c) a write MARKED 1 is stored by the master (>= 98%)',
          marked >= 1496 and c.applied[(1, True)] * 100 >= marked * 98,
          (c.applied[(1, True)], marked))
    check('(c) a write marked 0 is NOT stored (<= 5%)',
          unmarked >= 323 and c.applied[(0, True)] * 100 <= unmarked * 5,
          (c.applied[(0, True)], unmarked))
    # (b) the meaning, off the one sequence
    seq = b20_sequence(os.path.join(repo, B20))
    print('  vms-b20 sequence: %s' % seq)
    check('(b) file 13 created: (0,0) -> (2,1), its header written',
          (13, (0, 0), (2, 1), 'CONVERT', True) in seq, seq)
    check('(b) file 13 closed: (2,1) -> (3,1) on a marked $DEQ, its header '
          'written -- the HEADER counter', (13, (2, 1), (3, 1), '$DEQ', True)
          in seq, seq)
    check('(b) the MFD takes the entry: (1,3) -> (1,4) on a marked $DEQ, no '
          'header write -- the DATA counter',
          (4, (1, 3), (1, 4), '$DEQ', False) in seq, seq)
    print('VERDICT: %s' % ('PASS' if state['ok'] else 'FAIL'))
    return 0 if state['ok'] else 1


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0] == '--selftest':
        return selftest(argv[1] if len(argv) > 1
                        else os.path.join(HERE, '..', '..'))
    report(rc.walk_inputs(argv))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
