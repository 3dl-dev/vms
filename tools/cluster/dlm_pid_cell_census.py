#!/usr/bin/env python3
"""dlm_pid_cell_census.py -- what real VMS nodes put in the DLM request cell at
body[20:24] (abs 92), and what a real directory ANSWERS when it is zero.

CLEAN-ROOM PROVENANCE (project Rule 8). Every number below is counted off the
bytes real OpenVMS VAX V7.3 nodes put on a wire we captured -- the pcaps and
demo-gate frame logs committed under tests/lab/captures. Nothing is
disassembled, nothing is computed from an unpublished algorithm, and no value
is inferred from anything but the (frame in, frame out) pairs themselves.

WHAT THE CELL IS. src/kernel-core/vms_cluster_codec_dlm.h states it: the lock
id ON THE MASTER'S NODE, and "a fresh op-01 request carries a PID-form
placeholder here (the requester has no master handle yet)". This tool is the
measurement behind that sentence, and behind rd vms-c55:

  census    per opcode, the FORM of the cell on frames real VMS nodes sent
  scope     is the cell per-RESOURCE (a handle) or per-PROCESS (an id)?
  answers   the directory outcome byte body[34] against the cell the answer
            echoes from the request it answers
  --selftest  re-derive the three headline figures from the committed corpus

usage:
    dlm_pid_cell_census.py census  [<capture-or-dir>...]
    dlm_pid_cell_census.py scope   [<capture-or-dir>...]
    dlm_pid_cell_census.py answers [<capture-or-dir>...]
    dlm_pid_cell_census.py --selftest
"""
import collections
import os
import struct
import sys

ETH_SCA = b'\x60\x07'
FRAME_LEN = 204
OFF_CAT, OFF_OP = 80, 81
OFF_CELL = 92            # body[20:24]
OFF_STATUS = 106         # body[34], the directory outcome
OFF_NAME_LEN = 119       # body[47]
OFF_NAME = 120           # body[48..]
DLM_REQ, DLM_ANS = 0x02, 0x82
# The two OUIs every real node in this corpus transmits from: DEC's own, and
# the DECnet-programmed AA-00-04 cluster-logical address.
REAL_OUI = (b'\x08\x00\x2b', b'\xaa\x00\x04')
HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.path.join(HERE, '..', '..', 'tests', 'lab', 'captures')


def pcap_frames(path):
    """Classic libpcap, either endianness. No pcapng, no dependencies."""
    d = open(path, 'rb').read()
    if len(d) < 24:
        return
    endian = '<' if d[0:4] in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1') else '>'
    i = 24
    while i + 16 <= len(d):
        cap = struct.unpack(endian + 'IIII', d[i:i + 16])[2]
        yield None, d[i + 16:i + 16 + cap]
        i += 16 + cap


def jsonl_frames(path):
    """The demo gate's L2 switch log: one {t, port, hex} object per line. The
    port NAME is what says whose frame it is there -- a browser-hosted node's
    MAC carries no OUI that could."""
    import json
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
            yield r.get('port'), bytes.fromhex(r['hex'])
        except Exception:
            continue


def captures(args):
    """Every capture file named, or found under a directory named."""
    out = []
    for a in args:
        if os.path.isdir(a):
            for root, _d, fs in os.walk(a):
                out += [os.path.join(root, f) for f in sorted(fs)
                        if f.endswith(('.pcap', '.jsonl'))]
        else:
            out.append(a)
    return sorted(out)


def sent_by_real_vms(port, frame):
    """Is this frame a REAL OpenVMS node's? In a pcap the sender is known by
    DEC's own OUI (or the DECnet-programmed AA-00-04 cluster-logical address);
    in a demo capture it is known by the port name, because a node hosted in a
    browser or under SIMH has whatever MAC its emulator was given. OVMX's own
    frames are what this tool exists to measure AGAINST, so they are excluded
    either way."""
    if port is not None:
        return not port.upper().startswith('OVMX')
    return frame[6:9] in REAL_OUI


def dlm_frames(paths, cat):
    """Every 204-byte cat-`cat` DLM frame a REAL VMS node sent."""
    for path in paths:
        gen = jsonl_frames if path.endswith('.jsonl') else pcap_frames
        for port, f in gen(path):
            if len(f) != FRAME_LEN or f[12:14] != ETH_SCA:
                continue
            if f[OFF_CAT] != cat or not sent_by_real_vms(port, f):
                continue
            yield f


def cell(f):
    return struct.unpack('<I', f[OFF_CELL:OFF_CELL + 4])[0]


def resname(f):
    n = f[OFF_NAME_LEN]
    return f[OFF_NAME:OFF_NAME + n] if 1 <= n <= 31 else None


def form(v):
    """'zero', 'pid-shaped' (the 0x20xxxxxx placeholder form every real fresh
    request carries) or 'lock-id' (anything else, which on an op-03/06/07 is
    the master's handle)."""
    if v == 0:
        return 'zero'
    if (v >> 24) == 0x20 and (v & 0xffff) < 0x1000:
        return 'pid-shaped'
    return 'lock-id'


def cmd_census(paths):
    """Per opcode, the form of the cell."""
    t = collections.Counter()
    for f in dlm_frames(paths, DLM_REQ):
        t[(f[OFF_OP], form(cell(f)))] += 1
    for (op, k), n in sorted(t.items()):
        print('op 0x%02x  %-11s %7d' % (op, k, n))
    return t


def cmd_scope(paths):
    """Is the cell a per-RESOURCE handle or a per-PROCESS id?"""
    names_of = collections.defaultdict(set)
    for f in dlm_frames(paths, DLM_REQ):
        if f[OFF_OP] != 0x01:
            continue
        n = resname(f)
        if n is not None:
            names_of[(f[6:12], cell(f))].add(n)
    widest = max((len(v) for v in names_of.values()), default=0)
    print('distinct resource NAMES per (sender, cell) value: widest = %d'
          % widest)
    hist = collections.Counter(len(v) for v in names_of.values())
    for k in sorted(hist):
        print('   %4d name(s): %d value(s)' % (k, hist[k]))
    return widest


def cmd_answers(paths):
    """The directory outcome against the cell the answer echoes."""
    t = collections.Counter()
    for f in dlm_frames(paths, DLM_ANS):
        if f[OFF_OP] != 0x01:
            continue
        t[('zero' if cell(f) == 0 else 'nonzero', f[OFF_STATUS])] += 1
    for (kind, st), n in sorted(t.items()):
        print('echoed cell %-7s  body[34] = 0x%02x  %6d' % (kind, st, n))
    return t


def selftest():
    """Re-derive the three figures rd vms-c55 and the codec header cite. If the
    corpus grows these move, and the numbers in the source must move with them
    -- which is the point of pinning them here."""
    paths = captures([CORPUS])
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok = ok and good
        print('  [%s] %s: %r' % ('PASS' if good else 'FAIL', name, got)
              + ('' if good else ' (want %r)' % (want,)))

    c = cmd_census(paths)
    print('-- a fresh op-0x01 request names the PROCESS, later ops the HANDLE')
    check('op-0x01 pid-shaped', c[(0x01, 'pid-shaped')], 20525)
    check('op-0x01 zero', c[(0x01, 'zero')], 15)
    check('op-0x03 lock-id', c[(0x03, 'lock-id')], 9875)
    check('op-0x03 zero', c[(0x03, 'zero')], 0)

    print('-- and the cell is per-PROCESS: one value, many resources')
    check('widest value spans this many names', cmd_scope(paths), 483)

    print('-- 0xfe answers a zero cell and nothing else')
    a = cmd_answers(paths)
    fe_zero = a[('zero', 0xfe)]
    fe_nonzero = a[('nonzero', 0xfe)]
    check('0xfe answers with a ZERO echoed cell', fe_zero, 12)
    check('0xfe answers with a NONZERO echoed cell', fe_nonzero, 0)
    check('answers to a NONZERO cell, all outcomes',
          sum(n for (k, _s), n in a.items() if k == 'nonzero'), 19534)
    check('answers to a ZERO cell, all outcomes',
          sum(n for (k, _s), n in a.items() if k == 'zero'), 44)
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[1] == '--selftest':
        return selftest()
    paths = captures(argv[2:] or [CORPUS])
    cmds = {'census': cmd_census, 'scope': cmd_scope, 'answers': cmd_answers}
    if argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[argv[1]](paths)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
