#!/usr/bin/env python3
# framescan.py <frames.jsonl> [tail] -- decode the demo L2 switch's recorded
# 0x6007 frames: wall time, source port, SCA category/opcode, and for the DLM
# (cat 0x02) the resource name, so the LAST frames before a peer's bugcheck can
# be read by eye. rd: the V0.7-9 demo bugcheck.
import json, sys, collections
path = sys.argv[1]; tail = int(sys.argv[2]) if len(sys.argv) > 2 else 60
rows = []
for line in open(path):
    line = line.strip()
    if not line: continue
    try: r = json.loads(line)
    except Exception: continue
    b = bytes.fromhex(r['hex'])
    if len(b) < 204: 
        rows.append((r['t'], r['port'], b[0:6].hex(), b[6:12].hex(), None, None, len(b), b''))
        continue
    body = b[72:]
    rows.append((r['t'], r['port'], b[0:6].hex(), b[6:12].hex(), body[8], body[9], len(b), body))
print('total frames: %d' % len(rows))
c = collections.Counter((x[4], x[5]) for x in rows)
print('by cat/op:')
for (cat, op), n in sorted(c.items(), key=lambda kv: -kv[1]):
    print('   %s/%s  %d' % ('--' if cat is None else '%02x' % cat,
                            '--' if op is None else '%02x' % op, n))
print('\nlast %d frames:' % tail)
t0 = rows[0][0] if rows else 0
for t, port, dst, src, cat, op, ln, body in rows[-tail:]:
    name = ''
    if cat == 2 and len(body) > 48:
        L = body[47]
        if 0 < L <= 31:
            nm = body[48:48+L]
            if all(32 <= c2 < 127 for c2 in nm): name = '  name=%r' % nm
    print('  %8.3f %-6s %s->%s  %s/%s%s' % ((t - t0) / 1000.0, port, src[-4:], dst[-4:],
                                            '--' if cat is None else '%02x' % cat,
                                            '--' if op is None else '%02x' % op, name))
