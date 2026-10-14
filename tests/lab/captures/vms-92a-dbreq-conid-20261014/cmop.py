#!/usr/bin/env python3
# cmop.py <cat> <op> <pcap>... -- per-byte census of one cat/op CM frame,
# split by the sender's node-parameter version string at body[88:96] when the
# op carries one, else by whether body[16:] holds OVMX's own model text.
import struct, sys, collections
CAT, OP = int(sys.argv[1], 16), int(sys.argv[2], 16)
frames = []
for path in sys.argv[3:]:
    try: d = open(path, 'rb').read()
    except Exception: continue
    o = 24
    while o + 16 <= len(d):
        ts, tu, cl, ol = struct.unpack('<IIII', d[o:o+16]); o += 16
        f = d[o:o+cl]; o += cl
        if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13: continue
        b = bytes(f[72:])
        if len(b) < 132 or b[8] != CAT or b[9] != OP: continue
        frames.append((path, b))
print('=== cat %02x op %02x: %d frames' % (CAT, OP, len(frames)))
for i in range(0, 132):
    vals = collections.Counter(x[1][i] for x in frames)
    if len(vals) == 1 and list(vals)[0] == 0: continue
    print('  [%3d] %s' % (i, ' '.join('%02x:%d' % (v, c) for v, c in vals.most_common(6))))
print('\n-- distinct body[16:48] renderings:')
seen = collections.Counter(x[1][16:48] for x in frames)
for v, c in seen.most_common(10):
    txt = ''.join(chr(c2) if 32 <= c2 < 127 else '.' for c2 in v)
    print('   %4d  %s  |%s|' % (c, v.hex(), txt))
