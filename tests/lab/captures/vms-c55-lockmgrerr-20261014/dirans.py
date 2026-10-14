#!/usr/bin/env python3
"""dirans.py <pcap>... -- every cat-0x82 op-0x01 DIRECTORY ANSWER (body[34] 0xf9/0xf8)
in the corpus, with the cells a requester reads, so a real VAX's answer and OVMX's can
be compared field by field."""
import struct, sys, collections
rows = []
for path in sys.argv[1:]:
    try: d = open(path, 'rb').read()
    except Exception: continue
    o = 24
    while o + 16 <= len(d):
        ts, tu, cl, ol = struct.unpack('<IIII', d[o:o+16]); o += 16
        f = d[o:o+cl]; o += cl
        if len(f) != 204 or f[12:14] != b'\x60\x07': continue
        b = f[72:]
        if len(b) < 132 or b[8] != 0x82 or b[9] != 0x01: continue
        if b[34] not in (0xf9, 0xf8): continue
        rows.append((path.split('/')[-1], f[6:12].hex()[-4:], bytes(b)))
print('directory answers: %d' % len(rows))
cells = collections.defaultdict(collections.Counter)
for name, src, b in rows:
    cells['[20:24] req handle'][b[20:24].hex()] += 1
    cells['[24:28]'][b[24:28].hex()] += 1
    cells['[28:32] master csid'][b[28:32].hex()] += 1
    cells['[30]'][b[30]] += 1
    cells['[32:34]'][b[32:34].hex()] += 1
    cells['[34] status'][hex(b[34])] += 1
    cells['[35]'][hex(b[35])] += 1
    cells['[36:40]'][b[36:40].hex()] += 1
for k in sorted(cells):
    v = cells[k]
    top = ', '.join('%s x%d' % (a, n) for a, n in v.most_common(4))
    print('  %-22s %2d distinct: %s' % (k, len(v), top))
for name, src, b in rows[:3]:
    print('\n%s src=%s' % (name, src))
    for o2 in range(16, 56, 16):
        print('   [%3d] %s' % (o2, b[o2:o2+16].hex()))
