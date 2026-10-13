#!/usr/bin/env python3
"""lnmrec.py <pcap>... -- rd vms-94a: decode every clusterwide logical-name record a cat-0x0b
request moved by block transfer (the DATA payload that follows the descriptor), using the
layout grounded in README.md:

  rec[0:2]  total length          rec[2:4]  function: 1 $CRELNM, 2 $CRELNT, 3 $DELLNM
  rec[4:8]  base address B        rec[8:12] argument count n
  rec[12:12+4n] argument pointers, each B-relative (0 = argument omitted)

Arguments are the system service's own, in its documented order:
  $CRELNM (attr, tabnam, lognam, acmode, itmlst)
  $CRELNT (attr, resnam, reslen, quota, promsk, tabnam, partab, acmode, +1 not decoded)
  $DELLNM (tabnam, lognam, acmode)
A descriptor argument points at {u32 length, u32 pointer}; itmlst is a list of
{u16 buflen, u16 code, u32 bufaddr, u32 retlen} ended by a zero longword
(code 2 LNM$_STRING, 3 LNM$_ATTRIBUTES)."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../tools/cluster'))
import cm_dialogue_audit as A  # noqa: E402

FN = {1: '$CRELNM', 2: '$CRELNT', 3: '$DELLNM'}


def u32(r, o):
    return struct.unpack_from('<I', r, o)[0]


def decode(r):
    ln, fn = struct.unpack_from('<HH', r, 0)
    base, n = u32(r, 4), u32(r, 8)
    ptr = [u32(r, 12 + 4 * i) for i in range(n)]
    off = [(p - base) if p else None for p in ptr]

    def desc(o):
        if o is None:
            return None
        l, p = u32(r, o), u32(r, o + 4)
        return r[p - base:p - base + l].decode('latin-1')

    def val(o):
        return None if o is None else u32(r, o)
    out = {'len': ln, 'fn': FN.get(fn, fn)}
    if fn == 1:
        out.update(attr=val(off[0]), table=desc(off[1]), name=desc(off[2]), acmode=val(off[3]))
        items, o, cur = [], off[4], 0
        while o is not None and u32(r, o) != 0:
            bl, code = struct.unpack_from('<HH', r, o)
            a = u32(r, o + 4) - base
            if code == 3:
                cur = u32(r, a)
            elif code == 2:
                items.append((r[a:a + bl].decode('latin-1'), cur))
            else:
                items.append(('?code%d' % code, None))
            o += 12
        out['equiv'] = items
    elif fn == 2:
        out.update(attr=val(off[0]), quota=val(off[3]), promsk=val(off[4]),
                   table=desc(off[5]), parent=desc(off[6]), acmode=val(off[7]))
    elif fn == 3:
        out.update(table=desc(off[0]), name=desc(off[1]), acmode=val(off[2]))
    return out


def records(path):
    open_req = set(); seen = set()
    frames = list(A.read_pcap(path)); t0 = frames[0][0]
    for ts, f in frames:
        if not A.is_sca(f):
            continue
        s, d = f[6:12], f[0:6]
        if A.is_cm(f) and f[55] == 0x02:
            if f[80] == 0x0b and f[81] in (0x04, 0x06):
                open_req.add((s, d))
            elif f[80] == 0x8b:
                open_req.discard((d, s))
            continue
        if f[55] == 0x0d and (s, d) in open_req:
            p = f[84:]
            if p not in seen:
                seen.add(p)
                yield ts - t0, s.hex()[-4:], d.hex()[-4:], p


if __name__ == '__main__':
    for path in sys.argv[1:]:
        for t, s, d, p in records(path):
            try:
                print('%9.3f %s->%s %s' % (t, s, d, decode(p)))
            except Exception as e:  # a record this layout does not explain is reported, never skipped
                print('%9.3f %s->%s UNDECODED (%s) %d bytes' % (t, s, d, e, len(p)))
