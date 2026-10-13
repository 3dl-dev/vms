#!/usr/bin/env python3
# f7count.py <pcap> [name] -- rd vms-657: per (src->dst, cat/op[, body34]) counts of
# DLM frames naming the resource (default F7ROOT), plus every 0x82 answer with
# body[34] = 0xf7 (any resource). Station MACs mapped to SCSNODE names from the
# HELLO frames (abs 31 == 0), where the sender's SCSNODE sits at abs 41.
import struct, sys, collections, re
P = sys.argv[1]; NAME = (sys.argv[2] if len(sys.argv) > 2 else 'F7ROOT').encode()
d = open(P, 'rb').read(); o = 24
names = {}; c = collections.Counter(); f7 = []; first = None
while o + 16 <= len(d):
    ts, tu, cl, _ = struct.unpack('<IIII', d[o:o+16]); o += 16
    f = d[o:o+cl]; o += cl
    t = ts + tu / 1e6
    if first is None: first = t
    if len(f) < 60 or f[12:14] != b'\x60\x07': continue
    src = f[6:12].hex()
    # HELLO (abs 31 == 0): the sender's own SCSNODE at abs 41.
    if f[31] == 0 and len(f) > 47:
        m = re.match(rb'[A-Z0-9$_]{1,6}', f[41:47])
        if m:
            names.setdefault(src, collections.Counter())[m.group().decode()] += 1
    if len(f) != 204 or f[31] != 0x13 or f[55] != 2: continue
    b = f[72:]
    if b[8] in (2, 0x82):
        if NAME in b:
            c[(src, f[0:6].hex(), '%02x/%02x' % (b[8], b[9]),
               ('%02x' % b[34]) if b[8] == 0x82 else '')] += 1
        if b[8] == 0x82 and b[34] == 0xf7:
            f7.append((t - first, src, f[0:6].hex(), b[9], b[48:48 + b[47]] if b[47] < 64 else b''))
def nm(m):
    n = names.get(m)
    return n.most_common(1)[0][0] if n else m[-4:]
print('node map:', {m[-4:]: n.most_common(2) for m, n in names.items()})
for k, v in sorted(c.items(), key=lambda x: -x[1])[:30]:
    print('%-5s -> %-5s %s %s %d' % (nm(k[0]), nm(k[1]), k[2], k[3], v))
print('0xf7 answers:', len(f7))
for x in f7[:40]:
    print('  %8.3f %s -> %s op %02x %r' % (x[0], nm(x[1]), nm(x[2]), x[3], x[4]))
