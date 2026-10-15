#!/usr/bin/env python3
"""run_capture_gate.py -- the frame-conformance gate asked of a BOOTED node.

run_gate.sh asks "do the senders ctest can drive emit grounded bytes?".
This asks the question a bugcheck asks: of everything an OVMX node really put
in front of a real VAX's executive -- in every capture this repo has committed
-- which frames does the grounding table describe, and do the described ones
hold byte for byte?

THREE RULES, and the third is the ratchet (rd vms-ac4f):
  1. every DESCRIBED frame passes the per-field check;
  2. every UNDESCRIBED (category, opcode) shape is named in
     capture-shape-ratchet.tsv -- a shape that is not is a shape shipping
     ungrounded, and the gate reds;
  3. a ratchet row for a shape the table NOW covers is stale and the gate reds,
     so the list can only shrink. A new shape ships grounded or does not ship.

usage: run_capture_gate.py [--list]   (--list prints the ratchet as it should be)
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'cluster'))
import capture_conformance as cc  # noqa: E402

CAPTURES = os.path.join(ROOT, 'tests', 'lab', 'captures')
RATCHET = os.path.join(HERE, 'capture-shape-ratchet.tsv')
TABLE = os.path.join(ROOT, 'tests', 'cluster', 'conformance',
                     'frame-grounding.tsv')


def capture_files():
    """Every committed frame log. A pcap holds no port names, and the demo
    gate's .jsonl is where an OVMX node's own frames are identifiable."""
    out = []
    for root, _d, files in os.walk(CAPTURES):
        out += [os.path.join(root, f) for f in files if f.endswith('.jsonl')]
    return sorted(out)


def ovmx_ports(path):
    """The OVMX nodes that transmitted in this capture."""
    import json
    seen = set()
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            p = json.loads(line).get('port', '')
        except Exception:
            continue
        if p and p.upper().startswith('OVMX'):
            seen.add(p)
    return sorted(seen)


def scan(files=None):
    """(uncovered shape -> frame count, [field failures], frames, covered)."""
    uncovered = collections.Counter()
    fails = []
    total = covered_n = 0
    for path in (capture_files() if files is None else files):
        for port in ovmx_ports(path):
            fs = cc.frames(path, port, None)
            if not fs:
                continue
            covered, unc = cc.classify(fs)
            total += len(fs)
            covered_n += len(covered)
            uncovered.update(unc)
            if covered:
                bad, _unchecked = cc.check_fields(covered, TABLE)
                fails += ['%s %s: %s' % (os.path.relpath(path, ROOT), port, f)
                          for f in bad]
    return uncovered, fails, total, covered_n


def read_ratchet(path=RATCHET):
    rows = {}
    for line in open(path):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        cat, op, _rest = line.split('\t', 2)
        rows[(int(cat, 16), int(op, 16))] = line
    return rows


def shape(k):
    return 'cat 0x%02x op 0x%02x' % k


def judge(uncovered, fails, allowed, out):
    """The three rules. Returns 0 only when all three hold."""
    rc = 0
    for f in fails:
        out.append('  FAIL field check: %s' % f)
        rc = 1
    for k, n in sorted(uncovered.items(), key=lambda kv: -kv[1]):
        if k in allowed:
            out.append('  (ratcheted, %d frames) %s' % (n, shape(k)))
            continue
        out.append('  FAIL %s: %d frames of a shape NO row describes and the '
                   'ratchet does not name. A new shape ships grounded or does '
                   'not ship (rd vms-ac4f).' % (shape(k), n))
        rc = 1
    for k in allowed:
        if k not in uncovered:
            out.append('  FAIL %s: the ratchet names it, but no capture shows '
                       'it uncovered any more -- remove the row. The list only '
                       'shrinks.' % shape(k))
            rc = 1
    return rc


def selftest():
    """A gate that cannot fail proves nothing. Three single-factor fixtures,
    one per rule, each of which this gate must refuse."""
    import json
    import tempfile
    ok = True

    def check(name, cond):
        nonlocal ok
        ok = ok and cond
        print('  [%s] %s' % ('PASS' if cond else 'FAIL', name))

    # A frame of a COVERED shape, built the way the committed captures carry
    # one: a cat-0x04 op-0x00 ack with a zero body past the envelope.
    body = bytearray(132)
    body[8], body[9] = 0x04, 0x00
    frame = bytes(12) + b'\x60\x07' + bytes(58) + bytes(body)
    assert len(frame) == 204

    def capture(fr):
        f = tempfile.NamedTemporaryFile('w', suffix='.jsonl', delete=False)
        with f:
            f.write(json.dumps({'t': 0, 'port': 'OVMXA',
                                'hex': bytes(fr).hex()}) + '\n')
        return f.name

    def ratchet(rows):
        f = tempfile.NamedTemporaryFile('w', suffix='.tsv', delete=False)
        with f:
            for cat, op in rows:
                f.write('%02x\t%02x\t1\twhy\n' % (cat, op))
        return f.name

    clean = capture(frame)
    empty = ratchet([])
    unc, fails, total, cov = scan([clean])
    out = []
    rc = judge(unc, fails, read_ratchet(empty), out)
    check('a grounded frame with an empty ratchet passes', rc == 0 and cov == 1)

    # Rule 1: a byte outside its row's rule.
    bad = bytearray(frame)
    bad[72 + 8] = 0x04
    bad[72 + 20] = 0x5a          # body[20] is inside the const:0 residue span
    unc, fails, _t, _c = scan([capture(bad)])
    out = []
    check('a corrupted byte in a covered frame reds it',
          judge(unc, fails, read_ratchet(empty), out) == 1 and len(fails) > 0)

    # Rule 2: a shape no row describes and the ratchet does not name.
    novel = bytearray(frame)
    novel[72 + 9] = 0x77
    unc, fails, _t, _c = scan([capture(novel)])
    out = []
    check('an undescribed, unratcheted shape reds it',
          judge(unc, fails, read_ratchet(empty), out) == 1)
    out = []
    check('...and naming it in the ratchet lets it through',
          judge(unc, fails, read_ratchet(ratchet([(0x04, 0x77)])), out) == 0)

    # Rule 3: a ratchet row for a shape that is covered now.
    unc, fails, _t, _c = scan([clean])
    out = []
    check('a stale ratchet row reds it (the list only shrinks)',
          judge(unc, fails, read_ratchet(ratchet([(0x04, 0x00)])), out) == 1)
    return 0 if ok else 1


def main(argv):
    if '--selftest' in argv:
        return selftest()
    uncovered, fails, total, covered_n = scan()
    if '--list' in argv:
        for k, n in sorted(uncovered.items(), key=lambda kv: -kv[1]):
            print('%02x\t%02x\t%d\tWHY NOT GROUNDED YET' % (k[0], k[1], n))
        return 0
    print('committed captures: %d frames from OVMX nodes, %d of a covered '
          'type, %d of an uncovered one' % (total, covered_n,
                                            sum(uncovered.values())))
    out = []
    rc = judge(uncovered, fails, read_ratchet(), out)
    print('\n'.join(out))
    print('run_capture_gate: %s' % ('FAIL' if rc else
                                    'every emitted shape is grounded or '
                                    'ratcheted, and every grounded byte holds'))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv))
