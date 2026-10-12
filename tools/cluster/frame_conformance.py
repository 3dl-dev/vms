#!/usr/bin/env python3
"""frame_conformance.py -- the frame-conformance gate (rd vms-ci.6).

usage: frame_conformance.py <grounding.tsv> <dump-file> [--require TYPE ...]

<dump-file> holds one line per frame (or SYSAP body) a REAL OVMX sender emitted
under a host test (tests/cluster/host/conformance_dump.h):

    <type> <base-abs-offset> <state k=v,...|-> <hex>

Every byte of every frame must be covered by exactly one grounding row that
applies to it, and the field's value must satisfy the row's rule. Fails (exit 1)
on: an uncovered byte, two rows covering one byte, a value outside its rule, a
row with no evidence, or a --require'd frame type that no test emitted. Every
`deviation` row that applied is listed, so a lab fire never carries one silently.
"""
import sys
import collections


def load_table(path):
    rows = []
    for ln, line in enumerate(open(path), 1):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.rstrip('\n').split('\t')
        if len(f) != 7:
            sys.exit("%s:%d: want 7 TAB columns, got %d" % (path, ln, len(f)))
        typ, sel, off, length, cls, rule, ev = f
        if cls not in ('const', 'copied', 'derived', 'minted', 'deviation'):
            sys.exit("%s:%d: unknown class %r" % (path, ln, cls))
        if not ev.strip():
            sys.exit("%s:%d: a row with no evidence" % (path, ln))
        rows.append(dict(line=ln, type=typ, sel=sel, off=int(off), len=int(length),
                         cls=cls, rule=rule, ev=ev))
    return rows


def applies(row, typ, frame_abs):
    if row['type'] != typ:
        return False
    if row['sel'] == '*':
        return True
    if row['sel'].startswith('op='):
        op = int(row['sel'][3:], 16)
        return frame_abs.get(80) == op
    sys.exit("grounding line %d: unknown selector %r" % (row['line'], row['sel']))


def check(row, val_bytes, state):
    rule = row['rule']
    v = int.from_bytes(val_bytes, 'little')
    kind, _, arg = rule.partition(':')
    if kind == 'const':
        want = int(arg, 16)
        if row['len'] > 8:
            return want == 0 and not any(val_bytes), 'all-zero span'
        return v == want, '== %#x' % want
    if kind == 'bytes':
        return val_bytes.hex() == arg.lower(), '== %s' % arg
    if kind == 'set':
        allowed = {int(x, 16) for x in arg.split(',')}
        return v in allowed, 'in {%s}' % arg
    if kind == 'state':
        if arg not in state:
            return False, 'state %s not recorded by the emitting test' % arg
        return v == int(state[arg], 0), '== state %s (%s)' % (arg, state[arg])
    if kind == 'crf':
        return 0x0079 <= (v >> 16) <= 0xffdc and 0x0001 <= (v & 0xffff) <= 0x0038, 'P.CRF real envelope'
    if kind == 'bufname':
        return (v >> 16) != 0 and (v & 0xffff) == 0, 'buffer name: hi16 != 0, lo16 == 0'
    if kind == 'conid':
        return (v >> 16) != 0 and 0x0001 <= (v & 0xffff) <= 0x0010, 'Con.ID: hi16 != 0, lo16 1..0x10'
    if kind == 'any':
        return True, 'any (%s)' % arg
    sys.exit("grounding line %d: unknown rule %r" % (row['line'], rule))


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    rows = load_table(argv[1])
    required = []
    if '--require' in argv:
        required = argv[argv.index('--require') + 1:]
    fails = []
    seen_types = collections.Counter()
    deviations = collections.Counter()
    nframes = 0
    for ln, line in enumerate(open(argv[2]), 1):
        parts = line.split()
        if len(parts) != 4:
            continue
        typ, base, st, hexs = parts
        base = int(base)
        b = bytes.fromhex(hexs)
        state = {}
        if st != '-':
            for kv in st.split(','):
                k, _, val = kv.partition('=')
                state[k] = val
        frame_abs = {base + i: b[i] for i in range(len(b))}
        nframes += 1
        seen_types[typ] += 1
        cover = [0] * len(b)
        for row in rows:
            if not applies(row, typ, frame_abs):
                continue
            lo = row['off'] - base
            if lo < 0 or lo + row['len'] > len(b):
                fails.append("frame %d (%s): row %d (abs %d+%d) outside the %d-byte frame"
                             % (ln, typ, row['line'], row['off'], row['len'], len(b)))
                continue
            for i in range(lo, lo + row['len']):
                cover[i] += 1
            ok, what = check(row, b[lo:lo + row['len']], state)
            if not ok:
                fails.append("frame %d (%s): abs %d+%d = %s fails row %d (%s %s): %s"
                             % (ln, typ, row['off'], row['len'], b[lo:lo + row['len']].hex(),
                                row['line'], row['cls'], row['rule'], what))
            elif row['cls'] == 'deviation':
                deviations[(row['line'], typ, row['off'], row['ev'])] += 1
        for i, c in enumerate(cover):
            if c == 0:
                fails.append("frame %d (%s): abs %d has NO grounding row" % (ln, typ, base + i))
            elif c > 1:
                fails.append("frame %d (%s): abs %d covered by %d rows" % (ln, typ, base + i, c))
    for t in required:
        if seen_types[t] == 0:
            fails.append("required frame type %s: no test emitted one" % t)
    print("frame_conformance: %d frames, types %s" % (nframes, dict(seen_types)))
    for (rl, typ, off, ev), n in sorted(deviations.items()):
        print("  DEVIATION (row %d, %s abs %d, %d frames): %s" % (rl, typ, off, n, ev))
    seen = set()
    for f in fails:
        key = f.split(': ', 1)[1] if ': ' in f else f
        if key in seen:
            continue
        seen.add(key)
        print("  FAIL", f)
    if fails:
        print("frame_conformance: %d failure(s)" % len(fails))
        return 1
    print("frame_conformance: every byte grounded")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
